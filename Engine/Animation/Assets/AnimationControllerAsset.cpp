// AnimationControllerAsset.cpp - Checked canonical controller-document persistence.

#include "Animation/Assets/AnimationControllerAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <tuple>

namespace TerraEngine::Animation {
namespace {
constexpr std::uint32_t kSkeletonDependencyType = 6U;
constexpr std::uint32_t kClipDependencyType = 7U;
constexpr std::uint32_t kControllerDependencyType = 8U;
constexpr std::uint32_t kMaskDependencyType = 9U;
constexpr std::size_t kMaximumAuthoredStringBytes = 4096U;

// Append one fixed-width value only after proving the vector can grow safely.
template <typename T> bool Put(std::vector<std::uint8_t> &bytes, const T &value) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (bytes.size() > std::numeric_limits<std::size_t>::max() - sizeof(T))
		return false;
	const std::size_t oldSize = bytes.size();
	bytes.resize(oldSize + sizeof(T));
	std::memcpy(bytes.data() + oldSize, &value, sizeof(T));
	return true;
}
// Read one fixed-width value without letting malformed input advance past its payload.
template <typename T> bool Get(std::span<const std::uint8_t> bytes, std::size_t &cursor, T &value) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (cursor > bytes.size() || bytes.size() - cursor < sizeof(T))
		return false;
	std::memcpy(&value, bytes.data() + cursor, sizeof(T));
	cursor += sizeof(T);
	return true;
}
// Strings have one shared authoring bound so every route rejects the same oversized tag or name.
bool PutString(std::vector<std::uint8_t> &bytes, const std::string &value) {
	if (value.size() > kMaximumAuthoredStringBytes || !Put(bytes, static_cast<std::uint32_t>(value.size())))
		return false;
	if (bytes.size() > std::numeric_limits<std::size_t>::max() - value.size())
		return false;
	bytes.insert(bytes.end(), value.begin(), value.end());
	return true;
}
bool GetString(std::span<const std::uint8_t> bytes, std::size_t &cursor, std::string &value) {
	std::uint32_t size = 0U;
	if (!Get(bytes, cursor, size) || size > kMaximumAuthoredStringBytes || cursor > bytes.size() || bytes.size() - cursor < size)
		return false;
	value.assign(reinterpret_cast<const char *>(bytes.data() + cursor), size);
	cursor += size;
	return true;
}
bool KnownType(AnimationControllerParameterType type) noexcept {
	return type <= AnimationControllerParameterType::Tag;
}
bool KnownNode(AnimationControllerNodeKind kind) noexcept {
	return kind <= AnimationControllerNodeKind::ProceduralPose;
}
// Internal blend/procedural nodes are self-contained; only these kinds name another asset generation.
bool RequiresExternalDependency(AnimationControllerNodeKind kind) noexcept {
	return kind == AnimationControllerNodeKind::Clip || kind == AnimationControllerNodeKind::Subgraph ||
		   kind == AnimationControllerNodeKind::MotionMatch;
}
bool ValidValue(AnimationControllerParameterType type, const AnimationControllerParameterValue &value) noexcept {
	switch (type) {
	case AnimationControllerParameterType::Float:
		return std::holds_alternative<float>(value) && std::isfinite(std::get<float>(value));
	case AnimationControllerParameterType::Integer:
		return std::holds_alternative<std::int32_t>(value);
	case AnimationControllerParameterType::Boolean:
		return std::holds_alternative<bool>(value);
	case AnimationControllerParameterType::Trigger:
		return std::holds_alternative<std::monostate>(value);
	case AnimationControllerParameterType::Vector:
		return std::holds_alternative<AnimationFloat3>(value) && IsFinite(std::get<AnimationFloat3>(value));
	case AnimationControllerParameterType::Transform:
		return std::holds_alternative<AnimationTransform>(value) && IsFinite(std::get<AnimationTransform>(value)) &&
			   IsNormalized(std::get<AnimationTransform>(value).rotation);
	case AnimationControllerParameterType::Tag:
		return std::holds_alternative<std::string>(value) && !std::get<std::string>(value).empty() &&
			   std::get<std::string>(value).size() <= kMaximumAuthoredStringBytes;
	}
	return false;
}
bool PutValue(std::vector<std::uint8_t> &bytes, AnimationControllerParameterType type, const AnimationControllerParameterValue &value) {
	if (!ValidValue(type, value))
		return false;
	switch (type) {
	case AnimationControllerParameterType::Float:
		return Put(bytes, std::get<float>(value));
	case AnimationControllerParameterType::Integer:
		return Put(bytes, std::get<std::int32_t>(value));
	case AnimationControllerParameterType::Boolean:
		return Put(bytes, static_cast<std::uint8_t>(std::get<bool>(value) ? 1U : 0U));
	case AnimationControllerParameterType::Trigger:
		return true;
	case AnimationControllerParameterType::Vector:
		return Put(bytes, std::get<AnimationFloat3>(value));
	case AnimationControllerParameterType::Transform:
		return Put(bytes, std::get<AnimationTransform>(value));
	case AnimationControllerParameterType::Tag:
		return PutString(bytes, std::get<std::string>(value));
	}
	return false;
}
bool GetValue(std::span<const std::uint8_t> bytes, std::size_t &cursor, AnimationControllerParameterType type,
			  AnimationControllerParameterValue &value) {
	if (!KnownType(type))
		return false;
	switch (type) {
	case AnimationControllerParameterType::Float: {
		float v{};
		if (!Get(bytes, cursor, v))
			return false;
		value = v;
		return true;
	}
	case AnimationControllerParameterType::Integer: {
		std::int32_t v{};
		if (!Get(bytes, cursor, v))
			return false;
		value = v;
		return true;
	}
	case AnimationControllerParameterType::Boolean: {
		std::uint8_t v{};
		if (!Get(bytes, cursor, v) || v > 1U)
			return false;
		value = (v != 0U);
		return true;
	}
	case AnimationControllerParameterType::Trigger:
		value = std::monostate{};
		return true;
	case AnimationControllerParameterType::Vector: {
		AnimationFloat3 v{};
		if (!Get(bytes, cursor, v))
			return false;
		value = v;
		return true;
	}
	case AnimationControllerParameterType::Transform: {
		AnimationTransform v{};
		if (!Get(bytes, cursor, v))
			return false;
		value = v;
		return true;
	}
	case AnimationControllerParameterType::Tag: {
		std::string v;
		if (!GetString(bytes, cursor, v))
			return false;
		value = std::move(v);
		return true;
	}
	}
	return false;
}
template <typename Record> void SortById(std::vector<Record> &values) {
	std::sort(values.begin(), values.end(), [](const Record &a, const Record &b) { return a.stableId < b.stableId; });
}
void Canonicalize(AnimationControllerAsset &asset) {
	SortById(asset.parameters);
	SortById(asset.graphVariables);
	SortById(asset.reusableBindings);
	SortById(asset.nodes);
	SortById(asset.states);
	SortById(asset.transitions);
	SortById(asset.layers);
	SortById(asset.syncGroups);
	SortById(asset.subgraphDependencies);
	for (auto &group : asset.syncGroups)
		SortById(group.markers);
}
bool ContainsId(const auto &values, std::uint64_t id) {
	return std::any_of(values.begin(), values.end(), [id](const auto &v) { return v.stableId == id; });
}
AnimationStatus CheckNodeCount(std::size_t count) noexcept {
	return ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, count);
}
bool CheckedAddCount(std::size_t &total, std::size_t value) noexcept {
	if (total > std::numeric_limits<std::size_t>::max() - value)
		return false;
	total += value;
	return true;
}
// Validate every on-disk count before a deserializer allocates its corresponding vector.
AnimationStatus ValidateControllerCounts(const std::uint32_t counts[9]) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, counts[0]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, counts[1]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, counts[2]) != AnimationStatus::Ok ||
		CheckNodeCount(counts[3]) != AnimationStatus::Ok || CheckNodeCount(counts[4]) != AnimationStatus::Ok ||
		CheckNodeCount(counts[5]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerLayers, counts[6]) != AnimationStatus::Ok ||
		CheckNodeCount(counts[7]) != AnimationStatus::Ok || CheckNodeCount(counts[8]) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	return AnimationStatus::Ok;
}

// In-memory vectors use size_t, which can be wider than the uint32 fields on
// disk. Validate them before narrowing so an enormous authored collection can
// never wrap to a small serialized count.
AnimationStatus ValidateControllerCollectionSizes(const AnimationControllerAsset &asset) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, asset.parameters.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, asset.graphVariables.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, asset.reusableBindings.size()) != AnimationStatus::Ok ||
		CheckNodeCount(asset.nodes.size()) != AnimationStatus::Ok || CheckNodeCount(asset.states.size()) != AnimationStatus::Ok ||
		CheckNodeCount(asset.transitions.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerLayers, asset.layers.size()) != AnimationStatus::Ok ||
		CheckNodeCount(asset.syncGroups.size()) != AnimationStatus::Ok || CheckNodeCount(asset.subgraphDependencies.size()) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	return AnimationStatus::Ok;
}
} // namespace

AnimationStatus ValidateAnimationControllerAsset(const AnimationControllerAsset &asset) noexcept {
	if (!asset.assetId.IsValid() || !asset.skeletonAssetId.IsValid() || asset.compatibilityFingerprint == 0U ||
		asset.formatVersion != kAnimationControllerAssetCurrentVersion)
		return AnimationStatus::InvalidArgument;
	if (ValidateControllerCollectionSizes(asset) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	const std::uint32_t counts[] = {static_cast<std::uint32_t>(asset.parameters.size()),
									static_cast<std::uint32_t>(asset.graphVariables.size()),
									static_cast<std::uint32_t>(asset.reusableBindings.size()),
									static_cast<std::uint32_t>(asset.nodes.size()),
									static_cast<std::uint32_t>(asset.states.size()),
									static_cast<std::uint32_t>(asset.transitions.size()),
									static_cast<std::uint32_t>(asset.layers.size()),
									static_cast<std::uint32_t>(asset.syncGroups.size()),
									static_cast<std::uint32_t>(asset.subgraphDependencies.size())};
	if (ValidateControllerCounts(counts) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	std::set<std::uint64_t> ids;
	for (const auto &value : asset.parameters)
		if (value.stableId == 0U || value.name.empty() || value.name.size() > kMaximumAuthoredStringBytes || !KnownType(value.type) ||
			!ValidValue(value.type, value.defaultValue) || !ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	ids.clear();
	for (const auto &value : asset.graphVariables)
		if (value.stableId == 0U || value.name.empty() || value.name.size() > kMaximumAuthoredStringBytes || !KnownType(value.type) ||
			!ValidValue(value.type, value.defaultValue) || !ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	ids.clear();
	for (const auto &value : asset.reusableBindings) {
		const auto parameter = std::find_if(asset.parameters.begin(), asset.parameters.end(),
											[&](const auto &candidate) { return candidate.stableId == value.sourceParameterId; });
		const auto variable = std::find_if(asset.graphVariables.begin(), asset.graphVariables.end(),
										   [&](const auto &candidate) { return candidate.stableId == value.destinationVariableId; });
		// Bindings copy values without implicit conversion so native and managed
		// evaluators retain the same durable graph contract.
		if (value.stableId == 0U || parameter == asset.parameters.end() || variable == asset.graphVariables.end() ||
			parameter->type != variable->type || !ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	}
	ids.clear();
	for (const auto &value : asset.nodes)
		if (value.stableId == 0U || !KnownNode(value.kind) ||
			(RequiresExternalDependency(value.kind) && (!value.dependencyAssetId.IsValid() || value.dependencyGeneration == 0U ||
														value.dependencyFingerprint != asset.compatibilityFingerprint)) ||
			!ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	for (const auto &node : asset.nodes) {
		if (ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, node.blendSamples.size()) != AnimationStatus::Ok ||
			node.outOfDomainPolicy > AnimationControllerBlendOutOfDomainPolicy::Extrapolate)
			return AnimationStatus::HardLimitExceeded;
		std::set<std::uint64_t> sampleIds;
		for (const auto &sample : node.blendSamples)
			if (sample.stableId == 0U || sample.childNodeId == 0U || !std::isfinite(sample.positionX) || !std::isfinite(sample.positionY) ||
				!sampleIds.insert(sample.stableId).second)
				return AnimationStatus::InvalidArgument;
	}
	// Entry-state selection is semantic compiler work. Persist incomplete authored
	// graphs so T065 can issue diagnostics without losing last-known-good data.
	ids.clear();
	for (const auto &value : asset.states) {
		if (value.stableId == 0U || !ContainsId(asset.nodes, value.nodeId) ||
			(value.parentStateId != 0U && !ContainsId(asset.states, value.parentStateId)) || !ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	}
	ids.clear();
	for (const auto &value : asset.transitions) {
		if (value.stableId == 0U || !ContainsId(asset.states, value.sourceStateId) || !ContainsId(asset.states, value.destinationStateId) ||
			(value.conditionParameterId != 0U && !ContainsId(asset.parameters, value.conditionParameterId)) || value.durationTicks < 0 ||
			value.exitTimeTicks < 0 || !std::isfinite(value.destinationOffsetNormalized) ||
			value.destinationOffsetNormalized < 0.0F || value.destinationOffsetNormalized > 1.0F ||
			value.interruptionSource > AnimationControllerInterruptionSource::Next ||
			value.interruptionPolicy > AnimationControllerInterruptionPolicy::Replace ||
			value.triggerConsumption > AnimationControllerTriggerConsumption::MultiConsumer ||
			value.triggerReset > AnimationControllerTriggerReset::Manual || value.triggerFailure > AnimationControllerTriggerFailure::Reset ||
			!ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	}
	for (const auto &state : asset.states) {
		const auto count = static_cast<std::size_t>(std::count_if(asset.transitions.begin(), asset.transitions.end(),
																  [&](const auto &t) { return t.sourceStateId == state.stableId; }));
		if (ValidateAnimationHardLimit(AnimationLimit::TransitionsPerState, count) != AnimationStatus::Ok)
			return AnimationStatus::HardLimitExceeded;
	}
	ids.clear();
	for (const auto &value : asset.layers) {
		const bool hasMaskId = value.maskAssetId.IsValid();
		const bool hasMaskMetadata = value.maskGeneration != 0U || value.maskFingerprint != 0U;
		if (value.stableId == 0U || !ContainsId(asset.states, value.rootStateId) ||
			(hasMaskId ? (value.maskGeneration == 0U || value.maskFingerprint != asset.compatibilityFingerprint) : hasMaskMetadata) ||
			!std::isfinite(value.defaultWeight) || value.defaultWeight < 0.0F || value.defaultWeight > 1.0F ||
			!ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	}
	std::size_t markerTotal = 0U;
	ids.clear();
	for (const auto &value : asset.syncGroups) {
		if (!CheckedAddCount(markerTotal, value.markers.size()) || CheckNodeCount(value.markers.size()) != AnimationStatus::Ok ||
			value.stableId == 0U || value.name.empty() || value.name.size() > kMaximumAuthoredStringBytes ||
			!ContainsId(asset.states, value.leaderStateId) || !ids.insert(value.stableId).second)
			return markerTotal > GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum ? AnimationStatus::HardLimitExceeded
																									   : AnimationStatus::InvalidArgument;
		std::set<std::uint64_t> markerIds;
		for (const auto &marker : value.markers)
			if (marker.stableId == 0U || marker.name.empty() || marker.name.size() > kMaximumAuthoredStringBytes ||
				marker.clipEventId == 0U || !ContainsId(asset.states, marker.stateId) || !markerIds.insert(marker.stableId).second)
				return AnimationStatus::InvalidArgument;
	}
	if (CheckNodeCount(markerTotal) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	ids.clear();
	for (const auto &value : asset.subgraphDependencies)
		if (value.stableId == 0U || !value.controllerAssetId.IsValid() || value.generation == 0U ||
			value.compatibilityFingerprint != asset.compatibilityFingerprint || !ids.insert(value.stableId).second)
			return AnimationStatus::InvalidArgument;
	return AnimationStatus::Ok;
}

AnimationStatus SerializeAnimationControllerAsset(const AnimationControllerAsset &asset, std::vector<std::uint8_t> &output) {
	if (const AnimationStatus status = ValidateAnimationControllerAsset(asset); status != AnimationStatus::Ok)
		return status;
	AnimationControllerAsset canonical = asset;
	Canonicalize(canonical);
	std::vector<std::uint8_t> payload;
	const std::uint16_t version = kAnimationControllerAssetCurrentVersion;
	const std::uint16_t reserved = 0U;
	if (!Put(payload, kAnimationControllerPayloadMagic) || !Put(payload, version) || !Put(payload, reserved) ||
		!Put(payload, canonical.skeletonAssetId) || !Put(payload, canonical.compatibilityFingerprint))
		return AnimationStatus::RangeOverflow;
	const std::uint32_t counts[] = {static_cast<std::uint32_t>(canonical.parameters.size()),
									static_cast<std::uint32_t>(canonical.graphVariables.size()),
									static_cast<std::uint32_t>(canonical.reusableBindings.size()),
									static_cast<std::uint32_t>(canonical.nodes.size()),
									static_cast<std::uint32_t>(canonical.states.size()),
									static_cast<std::uint32_t>(canonical.transitions.size()),
									static_cast<std::uint32_t>(canonical.layers.size()),
									static_cast<std::uint32_t>(canonical.syncGroups.size()),
									static_cast<std::uint32_t>(canonical.subgraphDependencies.size())};
	for (auto c : counts)
		if (!Put(payload, c))
			return AnimationStatus::RangeOverflow;
	auto writeParameter = [&](const auto &value) {
		return Put(payload, value.stableId) && PutString(payload, value.name) && Put(payload, static_cast<std::uint8_t>(value.type)) &&
			   PutValue(payload, value.type, value.defaultValue);
	};
	for (const auto &v : canonical.parameters)
		if (!writeParameter(v))
			return AnimationStatus::RangeOverflow;
	for (const auto &v : canonical.graphVariables)
		if (!writeParameter(v))
			return AnimationStatus::RangeOverflow;
	for (const auto &v : canonical.reusableBindings)
		if (!Put(payload, v.stableId) || !Put(payload, v.sourceParameterId) || !Put(payload, v.destinationVariableId))
			return AnimationStatus::RangeOverflow;
	for (const auto &v : canonical.nodes)
		if (!Put(payload, v.stableId) || !Put(payload, static_cast<std::uint8_t>(v.kind)) || !Put(payload, v.flags) ||
			!Put(payload, v.dependencyAssetId) || !Put(payload, v.dependencyGeneration) || !Put(payload, v.dependencyFingerprint))
			return AnimationStatus::RangeOverflow;
	for (const auto &v : canonical.states) {
		const std::uint8_t flags = (v.entry ? 1U : 0U) | (v.exit ? 2U : 0U) | (v.anyState ? 4U : 0U);
		if (!Put(payload, v.stableId) || !Put(payload, v.nodeId) || !Put(payload, v.parentStateId) || !Put(payload, flags))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.transitions) {
		if (!Put(payload, v.stableId) || !Put(payload, v.sourceStateId) || !Put(payload, v.destinationStateId) ||
			!Put(payload, v.conditionParameterId) || !Put(payload, v.durationTicks) || !Put(payload, v.exitTimeTicks) ||
			!Put(payload, v.destinationOffsetNormalized) || !Put(payload, v.priority) ||
			!Put(payload, static_cast<std::uint8_t>(v.interruptionSource)) ||
			!Put(payload, static_cast<std::uint8_t>(v.interruptionPolicy)) ||
			!Put(payload, static_cast<std::uint8_t>(v.triggerConsumption)) ||
			!Put(payload, static_cast<std::uint8_t>(v.triggerReset)) ||
			!Put(payload, static_cast<std::uint8_t>(v.triggerFailure))) {
			return AnimationStatus::RangeOverflow;
		}
	}
	for (const auto &v : canonical.layers) {
		const std::uint8_t additive = v.additive ? 1U : 0U;
		if (!Put(payload, v.stableId) || !Put(payload, v.rootStateId) || !Put(payload, v.maskAssetId) || !Put(payload, v.maskGeneration) ||
			!Put(payload, v.maskFingerprint) || !Put(payload, additive) || !Put(payload, v.defaultWeight))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.syncGroups) {
		if (!Put(payload, v.stableId) || !PutString(payload, v.name) || !Put(payload, v.leaderStateId) ||
			!Put(payload, static_cast<std::uint32_t>(v.markers.size())))
			return AnimationStatus::RangeOverflow;
		for (const auto &marker : v.markers)
			if (!Put(payload, marker.stableId) || !PutString(payload, marker.name) || !Put(payload, marker.clipEventId) ||
				!Put(payload, marker.stateId))
				return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.subgraphDependencies)
		if (!Put(payload, v.stableId) || !Put(payload, v.controllerAssetId) || !Put(payload, v.generation) ||
			!Put(payload, v.compatibilityFingerprint))
			return AnimationStatus::RangeOverflow;
	// Optional trailing blend section keeps v1 payloads readable while preserving
	// durable samples for newer writers without changing legacy table offsets.
	const std::uint32_t blendNodeCount = static_cast<std::uint32_t>(canonical.nodes.size());
	if (!Put(payload, blendNodeCount)) return AnimationStatus::RangeOverflow;
	for (const auto &node : canonical.nodes) {
		if (!Put(payload,node.stableId) || !Put(payload,static_cast<std::uint8_t>(node.outOfDomainPolicy)) ||
			!Put(payload,node.outputCompatibilityFingerprint) || !Put(payload,static_cast<std::uint32_t>(node.blendSamples.size()))) return AnimationStatus::RangeOverflow;
		for (const auto &sample : node.blendSamples)
			if (!Put(payload,sample.stableId)||!Put(payload,sample.childNodeId)||!Put(payload,sample.positionX)||!Put(payload,sample.positionY)) return AnimationStatus::RangeOverflow;
	}
	AnimationAssetWriter writer;
	writer.AddDependency({canonical.skeletonAssetId, kSkeletonDependencyType, 1U});
	for (const auto &v : canonical.nodes)
		if (RequiresExternalDependency(v.kind))
			writer.AddDependency({v.dependencyAssetId,
								  v.kind == AnimationControllerNodeKind::Subgraph ? kControllerDependencyType : kClipDependencyType,
								  v.dependencyGeneration});
	for (const auto &v : canonical.layers)
		if (v.maskAssetId.IsValid())
			writer.AddDependency({v.maskAssetId, kMaskDependencyType, v.maskGeneration});
	for (const auto &v : canonical.subgraphDependencies)
		writer.AddDependency({v.controllerAssetId, kControllerDependencyType, v.generation});
	if (!writer.AddChunk(kAnimationChunkPayload, true, payload))
		return AnimationStatus::RangeOverflow;
	AnimationAssetEnvelope envelope{};
	envelope.assetId = canonical.assetId;
	envelope.compatibilityFingerprint = canonical.compatibilityFingerprint;
	return writer.Finish(envelope, output);
}

AnimationStatus DeserializeAnimationControllerAsset(std::span<const std::uint8_t> input, AnimationCompatibilityFingerprint expected,
													AnimationControllerAsset &output) {
	AnimationAssetReader reader;
	if (const AnimationStatus status = reader.Parse(input); status != AnimationStatus::Ok)
		return status;
	if (expected != 0U && reader.Envelope().compatibilityFingerprint != expected)
		return AnimationStatus::IncompatibleAsset;
	const auto bytes = reader.FindChunk(kAnimationChunkPayload);
	std::size_t cursor = 0U;
	std::uint32_t magic = 0U;
	std::uint16_t version = 0U, reserved = 0U;
	AnimationControllerAsset staged;
	if (!Get(bytes, cursor, magic) || !Get(bytes, cursor, version) || !Get(bytes, cursor, reserved) ||
		!Get(bytes, cursor, staged.skeletonAssetId) || !Get(bytes, cursor, staged.compatibilityFingerprint) ||
		magic != kAnimationControllerPayloadMagic || version != kAnimationControllerAssetCurrentVersion ||
		staged.compatibilityFingerprint != reader.Envelope().compatibilityFingerprint)
		return AnimationStatus::InvalidEnvelope;
	staged.assetId = reader.Envelope().assetId;
	staged.formatVersion = version;
	std::uint32_t counts[9]{};
	for (auto &count : counts)
		if (!Get(bytes, cursor, count))
			return AnimationStatus::TruncatedInput;
	if (const AnimationStatus status = ValidateControllerCounts(counts); status != AnimationStatus::Ok)
		return status;
	auto readParameter = [&](auto &values, std::uint32_t count) {
		values.resize(count);
		for (auto &value : values) {
			std::uint8_t type = 0U;
			if (!Get(bytes, cursor, value.stableId) || !GetString(bytes, cursor, value.name) || !Get(bytes, cursor, type)) {
				return false;
			}
			value.type = static_cast<AnimationControllerParameterType>(type);
			if (!GetValue(bytes, cursor, value.type, value.defaultValue))
				return false;
		}
		return true;
	};
	if (!readParameter(staged.parameters, counts[0]) || !readParameter(staged.graphVariables, counts[1]))
		return AnimationStatus::TruncatedInput;
	staged.reusableBindings.resize(counts[2]);
	for (auto &v : staged.reusableBindings)
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, v.sourceParameterId) || !Get(bytes, cursor, v.destinationVariableId))
			return AnimationStatus::TruncatedInput;
	staged.nodes.resize(counts[3]);
	for (auto &v : staged.nodes) {
		std::uint8_t kind = 0U;
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, kind) || !Get(bytes, cursor, v.flags) ||
			!Get(bytes, cursor, v.dependencyAssetId) || !Get(bytes, cursor, v.dependencyGeneration) ||
			!Get(bytes, cursor, v.dependencyFingerprint))
			return AnimationStatus::TruncatedInput;
		v.kind = static_cast<AnimationControllerNodeKind>(kind);
	}
	staged.states.resize(counts[4]);
	for (auto &v : staged.states) {
		std::uint8_t flags = 0U;
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, v.nodeId) || !Get(bytes, cursor, v.parentStateId) ||
			!Get(bytes, cursor, flags) || flags > 7U)
			return AnimationStatus::TruncatedInput;
		v.entry = (flags & 1U) != 0U;
		v.exit = (flags & 2U) != 0U;
		v.anyState = (flags & 4U) != 0U;
	}
	staged.transitions.resize(counts[5]);
	for (auto &v : staged.transitions) {
		std::uint8_t source = 0U;
		std::uint8_t policy = 0U;
		std::uint8_t consumption = 0U;
		std::uint8_t reset = 0U;
		std::uint8_t failure = 0U;
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, v.sourceStateId) || !Get(bytes, cursor, v.destinationStateId) ||
			!Get(bytes, cursor, v.conditionParameterId) || !Get(bytes, cursor, v.durationTicks) || !Get(bytes, cursor, v.exitTimeTicks) ||
			!Get(bytes, cursor, v.destinationOffsetNormalized) || !Get(bytes, cursor, v.priority) || !Get(bytes, cursor, source) ||
			!Get(bytes, cursor, policy) || !Get(bytes, cursor, consumption) || !Get(bytes, cursor, reset) || !Get(bytes, cursor, failure) ||
			source > 2U || policy > 1U || consumption > 1U || reset > 1U || failure > 1U) {
			return AnimationStatus::TruncatedInput;
		}
		v.interruptionSource = static_cast<AnimationControllerInterruptionSource>(source);
		v.interruptionPolicy = static_cast<AnimationControllerInterruptionPolicy>(policy);
		v.triggerConsumption = static_cast<AnimationControllerTriggerConsumption>(consumption);
		v.triggerReset = static_cast<AnimationControllerTriggerReset>(reset);
		v.triggerFailure = static_cast<AnimationControllerTriggerFailure>(failure);
	}
	staged.layers.resize(counts[6]);
	for (auto &v : staged.layers) {
		std::uint8_t additive = 0U;
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, v.rootStateId) || !Get(bytes, cursor, v.maskAssetId) ||
			!Get(bytes, cursor, v.maskGeneration) || !Get(bytes, cursor, v.maskFingerprint) || !Get(bytes, cursor, additive) ||
			!Get(bytes, cursor, v.defaultWeight) || additive > 1U)
			return AnimationStatus::TruncatedInput;
		v.additive = additive != 0U;
	}
	std::size_t markerTotal = 0U;
	staged.syncGroups.resize(counts[7]);
	for (auto &v : staged.syncGroups) {
		std::uint32_t markerCount = 0U;
		if (!Get(bytes, cursor, v.stableId) || !GetString(bytes, cursor, v.name) || !Get(bytes, cursor, v.leaderStateId) ||
			!Get(bytes, cursor, markerCount))
			return AnimationStatus::TruncatedInput;
		if (CheckNodeCount(markerCount) != AnimationStatus::Ok || !CheckedAddCount(markerTotal, markerCount) ||
			CheckNodeCount(markerTotal) != AnimationStatus::Ok)
			return AnimationStatus::HardLimitExceeded;
		v.markers.resize(markerCount);
		for (auto &marker : v.markers)
			if (!Get(bytes, cursor, marker.stableId) || !GetString(bytes, cursor, marker.name) || !Get(bytes, cursor, marker.clipEventId) ||
				!Get(bytes, cursor, marker.stateId))
				return AnimationStatus::TruncatedInput;
	}
	staged.subgraphDependencies.resize(counts[8]);
	for (auto &v : staged.subgraphDependencies)
		if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, v.controllerAssetId) || !Get(bytes, cursor, v.generation) ||
			!Get(bytes, cursor, v.compatibilityFingerprint))
			return AnimationStatus::TruncatedInput;
	if (cursor != bytes.size()) {
		std::uint32_t blendNodeCount = 0U;
		if (!Get(bytes,cursor,blendNodeCount) || blendNodeCount != staged.nodes.size() || CheckNodeCount(blendNodeCount) != AnimationStatus::Ok)
			return AnimationStatus::InvalidEnvelope;
		for (auto &node : staged.nodes) {
			std::uint8_t policy = 0U; std::uint32_t sampleCount = 0U;
			if (!Get(bytes,cursor,node.stableId) || !Get(bytes,cursor,policy) || !Get(bytes,cursor,node.outputCompatibilityFingerprint) ||
				!Get(bytes,cursor,sampleCount) || policy > 1U || CheckNodeCount(sampleCount) != AnimationStatus::Ok) return AnimationStatus::TruncatedInput;
			node.outOfDomainPolicy = static_cast<AnimationControllerBlendOutOfDomainPolicy>(policy); node.blendSamples.resize(sampleCount);
			for (auto &sample : node.blendSamples) if (!Get(bytes,cursor,sample.stableId)||!Get(bytes,cursor,sample.childNodeId)||!Get(bytes,cursor,sample.positionX)||!Get(bytes,cursor,sample.positionY)) return AnimationStatus::TruncatedInput;
		}
	}
	if (cursor != bytes.size()) return AnimationStatus::InvalidEnvelope;
	if (const AnimationStatus status = ValidateAnimationControllerAsset(staged); status != AnimationStatus::Ok)
		return status;
	Canonicalize(staged);
	output = std::move(staged);
	return AnimationStatus::Ok;
}
} // namespace TerraEngine::Animation
