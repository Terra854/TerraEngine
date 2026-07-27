// AnimationMaskAsset.cpp - Checked deterministic persistence for reusable masks.

#include "Animation/Assets/AnimationMaskAsset.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>

namespace TerraEngine::Animation {
namespace {
// Append fixed-width selector fields only after checking the output size arithmetic.
template <typename T> bool Put(std::vector<std::uint8_t> &bytes, const T &value) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (bytes.size() > std::numeric_limits<std::size_t>::max() - sizeof(T))
		return false;
	const auto old = bytes.size();
	bytes.resize(old + sizeof(T));
	std::memcpy(bytes.data() + old, &value, sizeof(T));
	return true;
}
// Keep a malformed mask payload from reading outside its chunk boundary.
template <typename T> bool Get(std::span<const std::uint8_t> bytes, std::size_t &cursor, T &value) {
	static_assert(std::is_trivially_copyable_v<T>);
	if (cursor > bytes.size() || bytes.size() - cursor < sizeof(T))
		return false;
	std::memcpy(&value, bytes.data() + cursor, sizeof(T));
	cursor += sizeof(T);
	return true;
}
bool FiniteWeight(float value) noexcept {
	return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}
bool CheckedAddCount(std::size_t &total, std::size_t value) noexcept {
	if (total > std::numeric_limits<std::size_t>::max() - value)
		return false;
	total += value;
	return true;
}
// Every declared count is checked before resize; the sum also prevents selector-count overflow.
AnimationStatus ValidateMaskCounts(const std::uint32_t counts[7]) noexcept {
	std::size_t selectorTotal = 0U;
	if (!CheckedAddCount(selectorTotal, counts[0]) || !CheckedAddCount(selectorTotal, counts[1]) ||
		!CheckedAddCount(selectorTotal, counts[2]) || !CheckedAddCount(selectorTotal, counts[3]))
		return AnimationStatus::HardLimitExceeded;
	if (ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, counts[0]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, counts[1]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, counts[2]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ClipChannels, counts[3]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, counts[4]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, counts[5]) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ClipChannels, counts[6]) != AnimationStatus::Ok ||
		selectorTotal > GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum)
		return AnimationStatus::HardLimitExceeded;
	return AnimationStatus::Ok;
}

// Check allocator-sized collections before narrowing them to the uint32 fields
// stored in the mask payload. This keeps the writer's checked-count promise on
// platforms where size_t is wider than the file representation.
AnimationStatus ValidateMaskCollectionSizes(const AnimationMaskAsset &asset) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, asset.jointSelectors.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, asset.branchSelectors.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, asset.morphSelectors.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ClipChannels, asset.curveSelectors.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, asset.compiledJointWeights.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, asset.compiledMorphIds.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ClipChannels, asset.compiledCurveIds.size()) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	std::size_t selectorTotal = 0U;
	if (!CheckedAddCount(selectorTotal, asset.jointSelectors.size()) || !CheckedAddCount(selectorTotal, asset.branchSelectors.size()) ||
		!CheckedAddCount(selectorTotal, asset.morphSelectors.size()) || !CheckedAddCount(selectorTotal, asset.curveSelectors.size()) ||
		selectorTotal > GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum)
		return AnimationStatus::HardLimitExceeded;
	return AnimationStatus::Ok;
}
template <typename T> bool OrderedUnique(const std::vector<T> &values, auto id) {
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (id(values[i]) == 0U || (i > 0U && id(values[i - 1U]) >= id(values[i])))
			return false;
	}
	return true;
}
void Canonicalize(AnimationMaskAsset &asset) {
	std::sort(asset.jointSelectors.begin(), asset.jointSelectors.end(),
			  [](const auto &a, const auto &b) { return a.stableJointId < b.stableJointId; });
	std::sort(asset.branchSelectors.begin(), asset.branchSelectors.end(),
			  [](const auto &a, const auto &b) { return a.rootJointId < b.rootJointId; });
	std::sort(asset.morphSelectors.begin(), asset.morphSelectors.end(),
			  [](const auto &a, const auto &b) { return a.stableId < b.stableId; });
	std::sort(asset.curveSelectors.begin(), asset.curveSelectors.end(),
			  [](const auto &a, const auto &b) { return a.stableId < b.stableId; });
	std::sort(asset.compiledJointWeights.begin(), asset.compiledJointWeights.end(),
			  [](const auto &a, const auto &b) { return a.stableJointId < b.stableJointId; });
	std::sort(asset.compiledMorphIds.begin(), asset.compiledMorphIds.end());
	std::sort(asset.compiledCurveIds.begin(), asset.compiledCurveIds.end());
}
} // namespace

AnimationStatus ValidateAnimationMaskAsset(const AnimationMaskAsset &asset) noexcept {
	if (!asset.assetId.IsValid() || !asset.skeletonAssetId.IsValid() || asset.compatibilityFingerprint == 0U ||
		asset.formatVersion != kAnimationMaskAssetCurrentVersion || !FiniteWeight(asset.rootMotionWeight))
		return AnimationStatus::InvalidArgument;
	if (ValidateMaskCollectionSizes(asset) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	const std::uint32_t counts[] = {
		static_cast<std::uint32_t>(asset.jointSelectors.size()),	   static_cast<std::uint32_t>(asset.branchSelectors.size()),
		static_cast<std::uint32_t>(asset.morphSelectors.size()),	   static_cast<std::uint32_t>(asset.curveSelectors.size()),
		static_cast<std::uint32_t>(asset.compiledJointWeights.size()), static_cast<std::uint32_t>(asset.compiledMorphIds.size()),
		static_cast<std::uint32_t>(asset.compiledCurveIds.size())};
	if (ValidateMaskCounts(counts) != AnimationStatus::Ok)
		return AnimationStatus::HardLimitExceeded;
	AnimationMaskAsset canonical = asset;
	Canonicalize(canonical);
	if (!OrderedUnique(canonical.jointSelectors, [](const auto &v) { return v.stableJointId; }) ||
		!OrderedUnique(canonical.branchSelectors, [](const auto &v) { return v.rootJointId; }) ||
		!OrderedUnique(canonical.morphSelectors, [](const auto &v) { return v.stableId; }) ||
		!OrderedUnique(canonical.curveSelectors, [](const auto &v) { return v.stableId; }) ||
		!OrderedUnique(canonical.compiledJointWeights, [](const auto &v) { return v.stableJointId; }))
		return AnimationStatus::InvalidArgument;
	if (std::adjacent_find(canonical.compiledMorphIds.begin(), canonical.compiledMorphIds.end()) != canonical.compiledMorphIds.end() ||
		std::adjacent_find(canonical.compiledCurveIds.begin(), canonical.compiledCurveIds.end()) != canonical.compiledCurveIds.end() ||
		std::any_of(canonical.compiledMorphIds.begin(), canonical.compiledMorphIds.end(), [](auto id) { return id == 0U; }) ||
		std::any_of(canonical.compiledCurveIds.begin(), canonical.compiledCurveIds.end(), [](auto id) { return id == 0U; }))
		return AnimationStatus::InvalidArgument;
	for (const auto &value : canonical.jointSelectors)
		if (!FiniteWeight(value.weight))
			return AnimationStatus::InvalidArgument;
	for (const auto &value : canonical.branchSelectors)
		if (!FiniteWeight(value.weight))
			return AnimationStatus::InvalidArgument;
	for (const auto &value : canonical.compiledJointWeights)
		if (!FiniteWeight(value.weight))
			return AnimationStatus::InvalidArgument;
	return AnimationStatus::Ok;
}

AnimationStatus SerializeAnimationMaskAsset(const AnimationMaskAsset &asset, std::vector<std::uint8_t> &output) {
	if (const auto status = ValidateAnimationMaskAsset(asset); status != AnimationStatus::Ok)
		return status;
	AnimationMaskAsset canonical = asset;
	Canonicalize(canonical);
	std::vector<std::uint8_t> bytes;
	const std::uint16_t version = kAnimationMaskAssetCurrentVersion, reserved = 0U;
	if (!Put(bytes, kAnimationMaskPayloadMagic) || !Put(bytes, version) || !Put(bytes, reserved) ||
		!Put(bytes, canonical.skeletonAssetId) || !Put(bytes, canonical.compatibilityFingerprint) ||
		!Put(bytes, canonical.rootMotionWeight))
		return AnimationStatus::RangeOverflow;
	const std::uint32_t counts[] = {
		static_cast<std::uint32_t>(canonical.jointSelectors.size()),	   static_cast<std::uint32_t>(canonical.branchSelectors.size()),
		static_cast<std::uint32_t>(canonical.morphSelectors.size()),	   static_cast<std::uint32_t>(canonical.curveSelectors.size()),
		static_cast<std::uint32_t>(canonical.compiledJointWeights.size()), static_cast<std::uint32_t>(canonical.compiledMorphIds.size()),
		static_cast<std::uint32_t>(canonical.compiledCurveIds.size())};
	for (auto count : counts)
		if (!Put(bytes, count))
			return AnimationStatus::RangeOverflow;
	for (const auto &v : canonical.jointSelectors) {
		const std::uint8_t descendants = v.includeDescendants ? 1U : 0U;
		if (!Put(bytes, v.stableJointId) || !Put(bytes, v.weight) || !Put(bytes, descendants))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.branchSelectors) {
		const std::uint8_t descendants = v.includeDescendants ? 1U : 0U;
		if (!Put(bytes, v.rootJointId) || !Put(bytes, v.weight) || !Put(bytes, descendants))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.morphSelectors) {
		const std::uint8_t included = v.include ? 1U : 0U;
		if (!Put(bytes, v.stableId) || !Put(bytes, included))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.curveSelectors) {
		const std::uint8_t included = v.include ? 1U : 0U;
		if (!Put(bytes, v.stableId) || !Put(bytes, included))
			return AnimationStatus::RangeOverflow;
	}
	for (const auto &v : canonical.compiledJointWeights)
		if (!Put(bytes, v.stableJointId) || !Put(bytes, v.weight))
			return AnimationStatus::RangeOverflow;
	for (auto id : canonical.compiledMorphIds)
		if (!Put(bytes, id))
			return AnimationStatus::RangeOverflow;
	for (auto id : canonical.compiledCurveIds)
		if (!Put(bytes, id))
			return AnimationStatus::RangeOverflow;
	AnimationAssetWriter writer;
	writer.AddDependency({canonical.skeletonAssetId, 6U, 1U});
	if (!writer.AddChunk(kAnimationChunkPayload, true, bytes))
		return AnimationStatus::RangeOverflow;
	AnimationAssetEnvelope envelope{};
	envelope.assetId = canonical.assetId;
	envelope.compatibilityFingerprint = canonical.compatibilityFingerprint;
	return writer.Finish(envelope, output);
}

AnimationStatus DeserializeAnimationMaskAsset(std::span<const std::uint8_t> input, AnimationCompatibilityFingerprint expected,
											  AnimationMaskAsset &output) {
	AnimationAssetReader reader;
	if (const auto status = reader.Parse(input); status != AnimationStatus::Ok)
		return status;
	if (expected != 0U && reader.Envelope().compatibilityFingerprint != expected)
		return AnimationStatus::IncompatibleAsset;
	const auto bytes = reader.FindChunk(kAnimationChunkPayload);
	std::size_t cursor = 0U;
	std::uint32_t magic = 0U;
	std::uint16_t version = 0U, reserved = 0U;
	AnimationMaskAsset staged;
	if (!Get(bytes, cursor, magic) || !Get(bytes, cursor, version) || !Get(bytes, cursor, reserved) ||
		!Get(bytes, cursor, staged.skeletonAssetId) || !Get(bytes, cursor, staged.compatibilityFingerprint) ||
		!Get(bytes, cursor, staged.rootMotionWeight) || magic != kAnimationMaskPayloadMagic ||
		version != kAnimationMaskAssetCurrentVersion || staged.compatibilityFingerprint != reader.Envelope().compatibilityFingerprint)
		return AnimationStatus::InvalidEnvelope;
	staged.assetId = reader.Envelope().assetId;
	staged.formatVersion = version;
	std::uint32_t counts[7]{};
	for (auto &count : counts)
		if (!Get(bytes, cursor, count))
			return AnimationStatus::TruncatedInput;
	if (const auto status = ValidateMaskCounts(counts); status != AnimationStatus::Ok)
		return status;
	staged.jointSelectors.resize(counts[0]);
	for (auto &v : staged.jointSelectors) {
		std::uint8_t flag = 0U;
		if (!Get(bytes, cursor, v.stableJointId) || !Get(bytes, cursor, v.weight) || !Get(bytes, cursor, flag) || flag > 1U)
			return AnimationStatus::TruncatedInput;
		v.includeDescendants = flag != 0U;
	}
	staged.branchSelectors.resize(counts[1]);
	for (auto &v : staged.branchSelectors) {
		std::uint8_t flag = 0U;
		if (!Get(bytes, cursor, v.rootJointId) || !Get(bytes, cursor, v.weight) || !Get(bytes, cursor, flag) || flag > 1U)
			return AnimationStatus::TruncatedInput;
		v.includeDescendants = flag != 0U;
	}
	auto readNamed = [&](auto &values, std::uint32_t count) {
		values.resize(count);
		for (auto &v : values) {
			std::uint8_t flag = 0U;
			if (!Get(bytes, cursor, v.stableId) || !Get(bytes, cursor, flag) || flag > 1U)
				return false;
			v.include = flag != 0U;
		}
		return true;
	};
	if (!readNamed(staged.morphSelectors, counts[2]) || !readNamed(staged.curveSelectors, counts[3]))
		return AnimationStatus::TruncatedInput;
	staged.compiledJointWeights.resize(counts[4]);
	for (auto &v : staged.compiledJointWeights)
		if (!Get(bytes, cursor, v.stableJointId) || !Get(bytes, cursor, v.weight))
			return AnimationStatus::TruncatedInput;
	staged.compiledMorphIds.resize(counts[5]);
	for (auto &id : staged.compiledMorphIds)
		if (!Get(bytes, cursor, id))
			return AnimationStatus::TruncatedInput;
	staged.compiledCurveIds.resize(counts[6]);
	for (auto &id : staged.compiledCurveIds)
		if (!Get(bytes, cursor, id))
			return AnimationStatus::TruncatedInput;
	if (cursor != bytes.size())
		return AnimationStatus::InvalidEnvelope;
	if (const auto status = ValidateAnimationMaskAsset(staged); status != AnimationStatus::Ok)
		return status;
	Canonicalize(staged);
	output = std::move(staged);
	return AnimationStatus::Ok;
}
} // namespace TerraEngine::Animation
