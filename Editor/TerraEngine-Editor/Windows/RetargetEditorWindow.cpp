// RetargetEditorWindow.cpp - interactive, generation-safe retarget authoring.

#include "RetargetEditorWindow.h"

#include "AnimationAssetWindowSupport.h"
#include "../Animation/AnimationAssetAuthoringDecode.h"
#include "../Assets/AssetDatabase.h"
#include "Engine/Resource/ResourceManager.h"

#if !defined(TERRAENGINE_EDITOR_TESTS)
#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
	using namespace TerraEngine::Animation;
	using TerraEngine::Editor::Animation::DeserializeAnimationAssetForAuthoring;
	using TerraEngine::Editor::Animation::RetargetAuthoringGenerationSnapshot;

	std::atomic<std::uint64_t> g_RetargetStageSequence{1U};
	// Publication is an infrequent protected editor boundary. Serializing the
	// final exact-byte compare and filesystem replacement prevents two windows
	// that started from the same revision from both reporting success.
	std::mutex g_RetargetPublicationMutex;

	std::uint64_t HashBytes(std::span<const std::uint8_t> bytes) noexcept {
		// FNV-1a is used only as a change token. The checked animation container
		// still performs its own stronger payload integrity validation.
		std::uint64_t hash = 1469598103934665603ULL;
		for(const std::uint8_t byte : bytes) {
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	AnimationGeneration GenerationFromHash(std::uint64_t hash) noexcept {
		AnimationGeneration generation = static_cast<AnimationGeneration>(
			(hash >> 32U) ^ (hash & 0xffffffffULL));
		return generation == 0U ? 1U : generation;
	}

	std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if(!input) {
			return {};
		}
		const std::streamoff end = input.tellg();
		if(end <= 0 || static_cast<std::uint64_t>(end)
			> kRetargetProfileMaximumPayloadBytes + 1024U * 1024U) {
			return {};
		}
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char*>(bytes.data()), end);
		return input ? bytes : std::vector<std::uint8_t>{};
	}

	bool WriteStagedBytes(
		const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes,
		std::string& diagnostic) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) {
			diagnostic = "Could not create the retarget staging file.";
			return false;
		}
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if(!output) {
			diagnostic = "Could not completely flush the retarget staging file.";
			return false;
		}
		return true;
	}

	bool ReplaceFileAtomically(
		const std::filesystem::path& staged,
		const std::filesystem::path& destination,
		std::string& diagnostic) {
#ifdef _WIN32
		// ReplaceFileW leaves the old destination intact if replacement fails.
		// MOVEFILE_WRITE_THROUGH handles the first publication case consistently.
		std::error_code existenceError;
		const bool exists = std::filesystem::exists(destination, existenceError);
		if(existenceError) {
			diagnostic = "The retarget destination could not be inspected before replacement.";
			return false;
		}
		const BOOL replaced = exists
			? ::ReplaceFileW(destination.c_str(), staged.c_str(), nullptr,
				REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
			: ::MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
		if(replaced == FALSE) {
			diagnostic = "The operating system rejected the atomic retarget replacement.";
			return false;
		}
		return true;
#else
		std::error_code error;
		std::filesystem::rename(staged, destination, error);
		if(error) {
			diagnostic = "The filesystem rejected the atomic retarget replacement: "
				+ error.message();
			return false;
		}
		return true;
#endif
	}

	std::string NormalizeSemantic(std::string_view value) {
		std::string normalized;
		normalized.reserve(value.size());
		for(const unsigned char character : value) {
			if(std::isalnum(character) != 0) {
				normalized.push_back(static_cast<char>(std::tolower(character)));
			}
		}
		return normalized;
	}

	const SkeletonJoint* FindJoint(const SkeletonAsset& skeleton, std::uint64_t stableId) {
		const auto iterator = std::find_if(skeleton.joints.begin(), skeleton.joints.end(),
			[stableId](const SkeletonJoint& joint) { return joint.stableId == stableId; });
		return iterator == skeleton.joints.end() ? nullptr : &*iterator;
	}

	std::optional<std::size_t> FindJointIndex(
		const SkeletonAsset& skeleton,
		std::uint64_t stableId) {
		for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
			if(skeleton.joints[index].stableId == stableId) {
				return index;
			}
		}
		return std::nullopt;
	}

	AnimationFloat3 Add(AnimationFloat3 left, AnimationFloat3 right) noexcept {
		return {left.x + right.x, left.y + right.y, left.z + right.z};
	}

	AnimationFloat3 Subtract(AnimationFloat3 left, AnimationFloat3 right) noexcept {
		return {left.x - right.x, left.y - right.y, left.z - right.z};
	}

	AnimationFloat3 Multiply(AnimationFloat3 left, AnimationFloat3 right) noexcept {
		return {left.x * right.x, left.y * right.y, left.z * right.z};
	}

	AnimationFloat3 Scale(AnimationFloat3 value, float scale) noexcept {
		return {value.x * scale, value.y * scale, value.z * scale};
	}

	AnimationQuaternion Multiply(
		AnimationQuaternion left,
		AnimationQuaternion right) noexcept {
		return {
			left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
			left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
			left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
			left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z
		};
	}

	AnimationFloat3 Rotate(AnimationQuaternion rotation, AnimationFloat3 value) noexcept {
		const AnimationFloat3 quaternionVector{rotation.x, rotation.y, rotation.z};
		const AnimationFloat3 twiceCross{
			2.0F * (quaternionVector.y * value.z - quaternionVector.z * value.y),
			2.0F * (quaternionVector.z * value.x - quaternionVector.x * value.z),
			2.0F * (quaternionVector.x * value.y - quaternionVector.y * value.x)
		};
		return Add(value, Add(Scale(twiceCross, rotation.w), AnimationFloat3{
			quaternionVector.y * twiceCross.z - quaternionVector.z * twiceCross.y,
			quaternionVector.z * twiceCross.x - quaternionVector.x * twiceCross.z,
			quaternionVector.x * twiceCross.y - quaternionVector.y * twiceCross.x
		}));
	}

	float Length(AnimationFloat3 value) noexcept {
		return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
	}

	std::vector<AnimationTransform> BuildModelTransforms(const SkeletonAsset& skeleton) {
		std::vector<AnimationTransform> result;
		result.reserve(skeleton.joints.size());
		for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
			const SkeletonJoint& joint = skeleton.joints[index];
			AnimationTransform model = joint.localBindTransform;
			if(joint.parentIndex >= 0
				&& static_cast<std::size_t>(joint.parentIndex) < result.size()) {
				const AnimationTransform& parent = result[static_cast<std::size_t>(joint.parentIndex)];
				model.translation = Add(parent.translation,
					Rotate(parent.rotation, Multiply(parent.scale, model.translation)));
				model.rotation = Multiply(parent.rotation, model.rotation);
				model.scale = Multiply(parent.scale, model.scale);
			}
			result.push_back(model);
		}
		return result;
	}

	float QuaternionError(AnimationQuaternion left, AnimationQuaternion right) noexcept {
		const float dot = std::clamp(std::abs(left.x * right.x + left.y * right.y
			+ left.z * right.z + left.w * right.w), 0.0F, 1.0F);
		return 2.0F * std::acos(dot);
	}

	AnimationQuaternion Conjugate(AnimationQuaternion value) noexcept {
		return {-value.x, -value.y, -value.z, value.w};
	}

	const RetargetOrientationBasis* FindOrientationBasis(
		const RetargetProfileAsset& profile,
		std::uint64_t basisId) noexcept {
		if(basisId == 0U) {
			return nullptr;
		}
		const auto iterator = std::find_if(profile.orientationBases.begin(),
			profile.orientationBases.end(), [basisId](const RetargetOrientationBasis& basis) {
				return basis.basisId == basisId;
			});
		return iterator == profile.orientationBases.end() ? nullptr : &*iterator;
	}

	AnimationQuaternion MappingAlignment(
		const RetargetProfileAsset& profile,
		std::uint64_t basisId) noexcept {
		const RetargetOrientationBasis* basis = FindOrientationBasis(profile, basisId);
		return basis == nullptr ? AnimationQuaternion{}
			: Multiply(basis->targetBasis, Conjugate(basis->sourceBasis));
	}

	float GlobalTranslationScale(const RetargetProfileAsset& profile) noexcept {
		float scale = 1.0F;
		switch(profile.proportion.translationScaleMode) {
		case RetargetTranslationScaleMode::None:
			break;
		case RetargetTranslationScaleMode::UniformHeight:
		case RetargetTranslationScaleMode::PerChainLength:
			scale = profile.proportion.sourceReferenceHeightMeters > 1.0e-6F
				? profile.proportion.targetReferenceHeightMeters
					/ profile.proportion.sourceReferenceHeightMeters : 1.0F;
			break;
		case RetargetTranslationScaleMode::Authored:
			scale = profile.proportion.authoredTranslationScale;
			break;
		}
		return std::clamp(scale, profile.proportion.minimumScale,
			profile.proportion.maximumScale);
	}

	float ChainLength(
		std::span<const std::uint64_t> jointIds,
		const SkeletonAsset& skeleton,
		std::span<const AnimationTransform> model) noexcept {
		float length = 0.0F;
		for(std::size_t index = 1U; index < jointIds.size(); ++index) {
			const std::optional<std::size_t> prior = FindJointIndex(skeleton, jointIds[index - 1U]);
			const std::optional<std::size_t> current = FindJointIndex(skeleton, jointIds[index]);
			if(!prior.has_value() || !current.has_value()
				|| *prior >= model.size() || *current >= model.size()) {
				return 0.0F;
			}
			length += Length(Subtract(model[*current].translation, model[*prior].translation));
		}
		return length;
	}

	float MappingTranslationScale(
		const RetargetProfileAsset& profile,
		std::uint64_t sourceJointId,
		std::uint64_t targetJointId,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		std::span<const AnimationTransform> sourceModel,
		std::span<const AnimationTransform> targetModel) noexcept {
		float scale = GlobalTranslationScale(profile);
		for(const RetargetChainMapping& chain : profile.chainMappings) {
			if(std::find(chain.sourceJointIds.begin(), chain.sourceJointIds.end(), sourceJointId)
				== chain.sourceJointIds.end()
				|| std::find(chain.targetJointIds.begin(), chain.targetJointIds.end(), targetJointId)
					== chain.targetJointIds.end()) {
				continue;
			}
			switch(chain.translationScaleMode) {
			case RetargetTranslationScaleMode::None:
				scale = 1.0F;
				break;
			case RetargetTranslationScaleMode::UniformHeight:
				scale = GlobalTranslationScale(profile);
				break;
			case RetargetTranslationScaleMode::Authored:
				scale = chain.authoredTranslationScale;
				break;
			case RetargetTranslationScaleMode::PerChainLength: {
				const float sourceLength = ChainLength(chain.sourceJointIds,
					sourceSkeleton, sourceModel);
				const float targetLength = ChainLength(chain.targetJointIds,
					targetSkeleton, targetModel);
				if(sourceLength > 1.0e-6F && targetLength > 1.0e-6F) {
					scale = targetLength / sourceLength;
				}
				break;
			}
			}
			break;
		}
		return std::clamp(scale, profile.proportion.minimumScale,
			profile.proportion.maximumScale);
	}

	std::string BytesToString(std::span<const std::uint8_t> bytes) {
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	std::span<const std::uint8_t> StringBytes(const std::string& bytes) noexcept {
		return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
	}

	const char* QualityTierName(RetargetQualityTier tier) {
		switch(tier) {
		case RetargetQualityTier::Unrated: return "Unrated";
		case RetargetQualityTier::Reduced: return "Reduced fidelity";
		case RetargetQualityTier::Production: return "Production";
		case RetargetQualityTier::Verified: return "Verified";
		}
		return "Unknown";
	}

	const char* ScaleModeName(RetargetTranslationScaleMode mode) {
		switch(mode) {
		case RetargetTranslationScaleMode::None: return "None";
		case RetargetTranslationScaleMode::UniformHeight: return "Uniform height";
		case RetargetTranslationScaleMode::PerChainLength: return "Per-chain length";
		case RetargetTranslationScaleMode::Authored: return "Authored";
		}
		return "Unknown";
	}

	std::string JointLabel(const SkeletonJoint& joint) {
		std::string label = joint.displayName.empty()
			? std::to_string(joint.stableId) : joint.displayName;
		if(!joint.semanticRole.empty()) {
			label += "  [" + joint.semanticRole + "]";
		}
		return label;
	}

	AnimationGeneration CheckedGeneration(std::uint64_t value) noexcept {
		if(value == 0U || value > std::numeric_limits<AnimationGeneration>::max()) {
			return 0U;
		}
		return static_cast<AnimationGeneration>(value);
	}

	bool ResolveSkeleton(
		AnimationGuid assetId,
		SkeletonAsset& output,
		std::string& diagnostic) {
		AssetDatabase& database = AssetDatabase::Instance();
		std::filesystem::path compiledPath;
		AnimationGeneration generation = 0U;
		GeneratedModelPackageChildMetadata generated;
		if(database.TryGetGeneratedModelPackageChildMetadata(
			assetId.value, GeneratedModelPackageChildKind::Skeleton, generated)) {
			compiledPath = database.GetBuildRoot() / generated.compiledPath;
			generation = CheckedGeneration(generated.publishedGeneration);
		} else if(const AssetMetadata* metadata = database.FindAssetByGuid(assetId.value)) {
			compiledPath = metadata->compiledPath.empty()
				? metadata->sourcePath : database.GetBuildRoot() / metadata->compiledPath;
			if(compiledPath.is_relative()) {
				compiledPath = database.GetAssetRoot() / compiledPath;
			}
		}
		if(compiledPath.empty()) {
			diagnostic = "A referenced skeleton is not registered in AssetDatabase.";
			return false;
		}

		const std::vector<std::uint8_t> bytes = ResourceManager::Instance().ReadFileBytes(compiledPath);
		if(bytes.empty()) {
			diagnostic = "A referenced .teskel artifact could not be read.";
			return false;
		}
		SkeletonAsset candidate;
		const AnimationStatus status = DeserializeAnimationAssetForAuthoring(
			bytes, candidate, DeserializeSkeletonAsset);
		if(status != AnimationStatus::Ok || candidate.assetId != assetId) {
			diagnostic = "A referenced .teskel artifact failed checked identity decoding: ";
			diagnostic += ToString(status);
			return false;
		}
		if(generation == 0U) {
			generation = GenerationFromHash(HashBytes(bytes));
		}
		candidate.runtimeGeneration = generation;
		output = std::move(candidate);
		return true;
	}

	RetargetAuthoringGenerationSnapshot MakeSnapshot(
		AnimationGuid profileId,
		std::span<const std::uint8_t> profileBytes,
		const SkeletonAsset& source,
		const SkeletonAsset& target) {
		// This snapshot is deliberately built from the exact on-disk revision and
		// checked skeleton payloads. A starter profile can therefore be bound in a
		// detached edit without making either selected skeleton live prematurely.
		RetargetAuthoringGenerationSnapshot snapshot;
		snapshot.profile = {profileId, GenerationFromHash(HashBytes(profileBytes))};
		snapshot.sourceSkeleton = {source.assetId, source.runtimeGeneration};
		snapshot.targetSkeleton = {target.assetId, target.runtimeGeneration};
		snapshot.sourceFingerprint = source.compatibilityFingerprint;
		snapshot.targetFingerprint = target.compatibilityFingerprint;
		snapshot.sourceReferencePoseHash = source.referencePoseHash;
		snapshot.targetReferencePoseHash = target.referencePoseHash;
		snapshot.persistedBytesHash = HashBytes(profileBytes);
		return snapshot;
	}

	std::optional<RetargetAuthoringGenerationSnapshot> CaptureSnapshot(
		const std::filesystem::path& profilePath,
		AnimationGuid expectedProfileId,
		SkeletonAsset* sourceOutput = nullptr,
		SkeletonAsset* targetOutput = nullptr,
		std::string* diagnosticOutput = nullptr) {
		const std::vector<std::uint8_t> profileBytes = ReadBytes(profilePath);
		RetargetProfileAsset profile;
		if(profileBytes.empty() || DeserializeAnimationAssetForAuthoring(
			profileBytes, profile, DeserializeRetargetProfileAsset) != AnimationStatus::Ok
			|| profile.assetId != expectedProfileId) {
			if(diagnosticOutput != nullptr) {
				*diagnosticOutput = "The selected retarget profile changed or no longer decodes.";
			}
			return std::nullopt;
		}

		SkeletonAsset source;
		SkeletonAsset target;
		std::string diagnostic;
		if(!ResolveSkeleton(profile.sourceSkeletonAssetId, source, diagnostic)
			|| !ResolveSkeleton(profile.targetSkeletonAssetId, target, diagnostic)) {
			if(diagnosticOutput != nullptr) {
				*diagnosticOutput = std::move(diagnostic);
			}
			return std::nullopt;
		}

		const RetargetAuthoringGenerationSnapshot snapshot = MakeSnapshot(
			profile.assetId, profileBytes, source, target);
		if(sourceOutput != nullptr) {
			*sourceOutput = std::move(source);
		}
		if(targetOutput != nullptr) {
			*targetOutput = std::move(target);
		}
		return snapshot;
	}

	std::optional<RetargetAuthoringGenerationSnapshot> CaptureStarterBindingSnapshot(
		const std::filesystem::path& profilePath,
		AnimationGuid expectedProfileId,
		AnimationGuid selectedSourceId,
		AnimationGuid selectedTargetId,
		SkeletonAsset* sourceOutput = nullptr,
		SkeletonAsset* targetOutput = nullptr,
		std::string* diagnosticOutput = nullptr) {
		const std::vector<std::uint8_t> profileBytes = ReadBytes(profilePath);
		RetargetProfileAsset profile;
		if(profileBytes.empty() || DeserializeAnimationAssetForAuthoring(
			profileBytes, profile, DeserializeRetargetProfileAsset)
			!= AnimationStatus::Ok || profile.assetId != expectedProfileId
			|| !selectedSourceId.IsValid() || !selectedTargetId.IsValid()) {
			if(diagnosticOutput != nullptr) {
				*diagnosticOutput = "The selected retarget profile or skeleton GUIDs are invalid.";
			}
			return std::nullopt;
		}

		// A profile which already resolves to another complete rig is not a blank
		// starter and must not be silently rebound. Generated starter placeholder
		// GUIDs intentionally fail this checked resolution and may be replaced.
		for(const auto [existingId, selectedId, side] : {
			std::tuple{profile.sourceSkeletonAssetId, selectedSourceId, "source"},
			std::tuple{profile.targetSkeletonAssetId, selectedTargetId, "target"}}) {
			if(!existingId.IsValid() || existingId == selectedId) {
				continue;
			}
			SkeletonAsset existing;
			std::string ignored;
			if(ResolveSkeleton(existingId, existing, ignored)) {
				if(diagnosticOutput != nullptr) {
					*diagnosticOutput = std::string("The profile already has a valid ") + side
						+ " skeleton; reload it instead of rebinding it.";
				}
				return std::nullopt;
			}
		}

		SkeletonAsset source;
		SkeletonAsset target;
		std::string diagnostic;
		if(!ResolveSkeleton(selectedSourceId, source, diagnostic)
			|| !ResolveSkeleton(selectedTargetId, target, diagnostic)) {
			if(diagnosticOutput != nullptr) {
				*diagnosticOutput = std::move(diagnostic);
			}
			return std::nullopt;
		}
		const RetargetAuthoringGenerationSnapshot snapshot = MakeSnapshot(
			profile.assetId, profileBytes, source, target);
		if(sourceOutput != nullptr) {
			*sourceOutput = std::move(source);
		}
		if(targetOutput != nullptr) {
			*targetOutput = std::move(target);
		}
		return snapshot;
	}

	std::vector<std::uint64_t> RootToEffectorPath(
		const SkeletonAsset& skeleton,
		std::uint64_t effectorJointId) {
		const std::optional<std::size_t> effector = FindJointIndex(skeleton, effectorJointId);
		if(!effector.has_value() || skeleton.rootJointIndex >= skeleton.joints.size()) {
			return {};
		}
		std::vector<std::uint64_t> reversed;
		for(std::int32_t index = static_cast<std::int32_t>(*effector);
			index >= 0 && reversed.size() <= skeleton.joints.size();
			index = skeleton.joints[static_cast<std::size_t>(index)].parentIndex) {
			reversed.push_back(skeleton.joints[static_cast<std::size_t>(index)].stableId);
		}
		if(reversed.empty() || reversed.back()
			!= skeleton.joints[skeleton.rootJointIndex].stableId) {
			return {};
		}
		std::reverse(reversed.begin(), reversed.end());
		return reversed;
	}

#if !defined(TERRAENGINE_EDITOR_TESTS)
	struct RetargetSkeletonCandidate {
		AnimationGuid assetId{};
		std::string label;
	};

	std::vector<RetargetSkeletonCandidate> DiscoverRetargetSkeletonCandidates() {
		AssetDatabase& database = AssetDatabase::Instance();
		std::vector<RetargetSkeletonCandidate> candidates;
		for(const auto& [path, metadata] : database.GetAssets()) {
			std::vector<TerraEngine::UUID> childIds;
			if(database.TryGetGeneratedModelPackageChildIds(metadata.guid,
				GeneratedModelPackageChildKind::Skeleton, childIds)) {
				for(const TerraEngine::UUID& childId : childIds) {
					GeneratedModelPackageChildMetadata child;
					if(!database.TryGetGeneratedModelPackageChildMetadata(childId,
						GeneratedModelPackageChildKind::Skeleton, child)) {
						continue;
					}
					const std::string name = child.displayName.empty()
						? path.filename().string() : child.displayName;
					candidates.push_back({{childId}, name + "  [" + ToHexString(childId) + "]"});
				}
			}
			if(metadata.importerName == "SkeletonAssetImporter") {
				candidates.push_back({{metadata.guid}, path.filename().string()
					+ "  [" + ToHexString(metadata.guid) + "]"});
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const RetargetSkeletonCandidate& left,
			const RetargetSkeletonCandidate& right) { return left.assetId.value < right.assetId.value; });
		candidates.erase(std::unique(candidates.begin(), candidates.end(),
			[](const RetargetSkeletonCandidate& left, const RetargetSkeletonCandidate& right) {
				return left.assetId == right.assetId;
			}), candidates.end());
		return candidates;
	}

	void CopyGuidText(std::array<char, 33U>& output, AnimationGuid assetId) {
		const std::string value = ToHexString(assetId.value);
		std::snprintf(output.data(), output.size(), "%s", value.c_str());
	}

	bool DrawSkeletonBindingPicker(
		const char* label,
		std::array<char, 33U>& guidText,
		const std::vector<RetargetSkeletonCandidate>& candidates) {
		bool changed = ImGui::InputText(label, guidText.data(), guidText.size());
		const std::string preview = guidText[0] == '\0' ? "Select a generated skeleton"
			: guidText.data();
		if(ImGui::BeginCombo((std::string(label) + " picker").c_str(), preview.c_str())) {
			for(const RetargetSkeletonCandidate& candidate : candidates) {
				const std::string guid = ToHexString(candidate.assetId.value);
				const bool selected = guid == guidText.data();
				if(ImGui::Selectable(candidate.label.c_str(), selected)) {
					CopyGuidText(guidText, candidate.assetId);
					changed = true;
				}
				if(selected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void DrawSkeletonColumn(
		const char* childId,
		const SkeletonAsset& skeleton,
		std::uint64_t& selectedJoint) {
		if(!ImGui::BeginChild(childId, ImVec2(0.0F, 210.0F), true)) {
			ImGui::EndChild();
			return;
		}
		for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
			const SkeletonJoint& joint = skeleton.joints[index];
			int depth = 0;
			std::int32_t parent = joint.parentIndex;
			while(parent >= 0 && static_cast<std::size_t>(parent) < skeleton.joints.size()
				&& depth < 32) {
				++depth;
				parent = skeleton.joints[static_cast<std::size_t>(parent)].parentIndex;
			}
			ImGui::Indent(static_cast<float>(depth) * 12.0F);
			ImGui::PushID(static_cast<int>(index));
			const std::string label = JointLabel(joint);
			if(ImGui::Selectable(label.c_str(), selectedJoint == joint.stableId)) {
				selectedJoint = joint.stableId;
			}
			ImGui::PopID();
			ImGui::Unindent(static_cast<float>(depth) * 12.0F);
		}
		ImGui::EndChild();
	}

	bool DrawTargetJointCombo(
		const char* id,
		const SkeletonAsset& targetSkeleton,
		std::uint64_t current,
		std::uint64_t& selected) {
		const SkeletonJoint* currentJoint = FindJoint(targetSkeleton, current);
		const std::string preview = currentJoint == nullptr ? "(missing)" : JointLabel(*currentJoint);
		bool changed = false;
		if(ImGui::BeginCombo(id, preview.c_str())) {
			for(const SkeletonJoint& joint : targetSkeleton.joints) {
				const bool isSelected = joint.stableId == current;
				const std::string label = JointLabel(joint);
				if(ImGui::Selectable(label.c_str(), isSelected)) {
					selected = joint.stableId;
					changed = true;
				}
				if(isSelected) {
					ImGui::SetItemDefaultFocus();
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void DrawRetargetPosePreview(
		const TerraEngine::Editor::Animation::RetargetPosePreviewPacket& packet,
		std::uint64_t selectedSourceJoint,
		std::uint64_t selectedTargetJoint) {
		const ImVec2 canvasSize{ImGui::GetContentRegionAvail().x, 280.0F};
		ImGui::InvisibleButton("Retarget geometric pose preview", canvasSize);
		const ImVec2 minimum = ImGui::GetItemRectMin();
		const ImVec2 maximum = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const float middle = (minimum.x + maximum.x) * 0.5F;
		drawList->AddRectFilled(minimum, maximum, IM_COL32(24, 28, 34, 255));
		drawList->AddLine({middle, minimum.y}, {middle, maximum.y},
			IM_COL32(90, 96, 108, 255), 1.0F);
		drawList->AddText({minimum.x + 8.0F, minimum.y + 6.0F},
			IM_COL32(210, 220, 235, 255), "Source reference pose");
		drawList->AddText({middle + 8.0F, minimum.y + 6.0F},
			IM_COL32(210, 220, 235, 255), "Target reference pose");

		struct Projector {
			float minimumX = 0.0F;
			float maximumX = 1.0F;
			float minimumY = 0.0F;
			float maximumY = 1.0F;
			ImVec2 panelMinimum{};
			ImVec2 panelMaximum{};

			[[nodiscard]] ImVec2 Project(AnimationFloat3 point) const noexcept {
				const float width = (std::max)(maximumX - minimumX, 1.0e-4F);
				const float height = (std::max)(maximumY - minimumY, 1.0e-4F);
				const float scale = (std::min)((panelMaximum.x - panelMinimum.x) / width,
					(panelMaximum.y - panelMinimum.y) / height);
				const float centerX = (minimumX + maximumX) * 0.5F;
				const float centerY = (minimumY + maximumY) * 0.5F;
				return {(panelMinimum.x + panelMaximum.x) * 0.5F
					+ (point.x - centerX) * scale,
					(panelMinimum.y + panelMaximum.y) * 0.5F
					- (point.y - centerY) * scale};
			}
		};

		auto makeProjector = [&](const std::vector<TerraEngine::Editor::Animation::RetargetPreviewJoint>& joints,
			float panelLeft, float panelRight) {
			Projector projector;
			projector.minimumX = std::numeric_limits<float>::max();
			projector.maximumX = std::numeric_limits<float>::lowest();
			projector.minimumY = std::numeric_limits<float>::max();
			projector.maximumY = std::numeric_limits<float>::lowest();
			for(const auto& joint : joints) {
				projector.minimumX = (std::min)(projector.minimumX, joint.position.x);
				projector.maximumX = (std::max)(projector.maximumX, joint.position.x);
				projector.minimumY = (std::min)(projector.minimumY, joint.position.y);
				projector.maximumY = (std::max)(projector.maximumY, joint.position.y);
			}
			if(joints.empty()) {
				projector.minimumX = projector.minimumY = -0.5F;
				projector.maximumX = projector.maximumY = 0.5F;
			}
			projector.panelMinimum = {panelLeft + 16.0F, minimum.y + 30.0F};
			projector.panelMaximum = {panelRight - 16.0F, maximum.y - 18.0F};
			return projector;
		};
		const Projector sourceProjector = makeProjector(packet.sourceJoints, minimum.x, middle);
		const Projector targetProjector = makeProjector(packet.targetJoints, middle, maximum.x);

		auto drawPose = [&](const std::vector<TerraEngine::Editor::Animation::RetargetPreviewJoint>& joints,
			const Projector& projector, std::uint64_t selectedJoint, ImU32 color) {
			std::unordered_map<std::uint64_t, AnimationFloat3> positions;
			for(const auto& joint : joints) {
				positions.emplace(joint.jointId, joint.position);
			}
			for(const auto& joint : joints) {
				const auto parent = positions.find(joint.parentJointId);
				if(parent != positions.end()) {
					drawList->AddLine(projector.Project(parent->second),
						projector.Project(joint.position), color, 2.0F);
				}
			}
			for(const auto& joint : joints) {
				const bool selected = joint.jointId == selectedJoint;
				drawList->AddCircleFilled(projector.Project(joint.position),
					selected ? 5.0F : 3.0F, selected
						? IM_COL32(255, 210, 70, 255) : color);
			}
		};
		drawPose(packet.sourceJoints, sourceProjector, selectedSourceJoint,
			IM_COL32(90, 175, 245, 255));
		drawPose(packet.targetJoints, targetProjector, selectedTargetJoint,
			IM_COL32(175, 125, 245, 255));

		for(const auto& contact : packet.contacts) {
			const ImU32 color = contact.withinTolerance
				? IM_COL32(75, 220, 120, 255) : IM_COL32(245, 85, 75, 255);
			const ImVec2 sourcePoint = sourceProjector.Project(contact.alignedSourcePosition);
			const ImVec2 targetPoint = targetProjector.Project(contact.targetPosition);
			drawList->AddCircle(sourcePoint, 6.0F, color, 16, 2.0F);
			drawList->AddCircle(targetPoint, 6.0F, color, 16, 2.0F);
			drawList->AddLine(sourcePoint, targetPoint, color, 1.5F);
			const std::string label = "C" + std::to_string(contact.contactId)
				+ (contact.withinTolerance ? " OK" : " ERROR");
			drawList->AddText({(sourcePoint.x + targetPoint.x) * 0.5F + 4.0F,
				(sourcePoint.y + targetPoint.y) * 0.5F}, color, label.c_str());
		}
	}
#endif
}

namespace TerraEngine::Editor::Animation {
	bool RetargetAuthoringGenerationSnapshot::IsValid() const noexcept {
		return profile.IsValid() && sourceSkeleton.IsValid() && targetSkeleton.IsValid()
			&& sourceFingerprint != 0U && targetFingerprint != 0U
			&& sourceReferencePoseHash != 0U && targetReferencePoseHash != 0U
			&& persistedBytesHash != 0U;
	}

	bool RetargetEditorModel::Begin(
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		const RetargetAuthoringGenerationSnapshot& generation,
		bool bindSelectedSkeletons) {
		if(!generation.IsValid() || profile.assetId != generation.profile.assetId
			|| sourceSkeleton.joints.empty() || targetSkeleton.joints.empty()
			|| sourceSkeleton.rootJointIndex >= sourceSkeleton.joints.size()
			|| targetSkeleton.rootJointIndex >= targetSkeleton.joints.size()
			|| sourceSkeleton.assetId != generation.sourceSkeleton.assetId
			|| targetSkeleton.assetId != generation.targetSkeleton.assetId
			|| sourceSkeleton.runtimeGeneration != generation.sourceSkeleton.generation
			|| targetSkeleton.runtimeGeneration != generation.targetSkeleton.generation
			|| sourceSkeleton.compatibilityFingerprint != generation.sourceFingerprint
			|| targetSkeleton.compatibilityFingerprint != generation.targetFingerprint
			|| sourceSkeleton.referencePoseHash != generation.sourceReferencePoseHash
			|| targetSkeleton.referencePoseHash != generation.targetReferencePoseHash) {
			return false;
		}
		if(!bindSelectedSkeletons
			&& (profile.sourceSkeletonAssetId != sourceSkeleton.assetId
				|| profile.targetSkeletonAssetId != targetSkeleton.assetId
				|| profile.sourceSkeletonFingerprint != sourceSkeleton.compatibilityFingerprint
				|| profile.targetSkeletonFingerprint != targetSkeleton.compatibilityFingerprint
				|| profile.sourceReferencePoseHash != sourceSkeleton.referencePoseHash
				|| profile.targetReferencePoseHash != targetSkeleton.referencePoseHash)) {
			return false;
		}
		candidate_ = profile;
		// Starter documents contain deliberately non-resolving placeholder GUIDs.
		// Binding is explicit, checked by the window, and only changes this
		// detached copy until the normal atomic publisher validates and saves it.
		candidate_.sourceSkeletonAssetId = sourceSkeleton.assetId;
		candidate_.targetSkeletonAssetId = targetSkeleton.assetId;
		candidate_.sourceSkeletonFingerprint = sourceSkeleton.compatibilityFingerprint;
		candidate_.targetSkeletonFingerprint = targetSkeleton.compatibilityFingerprint;
		candidate_.sourceReferencePoseHash = sourceSkeleton.referencePoseHash;
		candidate_.targetReferencePoseHash = targetSkeleton.referencePoseHash;
		if(bindSelectedSkeletons) {
			// A new binding has no meaningful root rule yet, so start at the two
			// checked skeleton roots. Existing profiles retain authored root choices.
			candidate_.root.sourceRootJointId = sourceSkeleton.joints[sourceSkeleton.rootJointIndex].stableId;
			candidate_.root.targetRootJointId = targetSkeleton.joints[targetSkeleton.rootJointIndex].stableId;
		}
		sourceSkeleton_ = sourceSkeleton;
		targetSkeleton_ = targetSkeleton;
		generation_ = generation;
		loaded_ = true;
		const AnimationStatus refreshStatus = RefreshErrors();
		// Loading computes a fresh report for display, but it is not itself an
		// authored user edit and must not enable Save.
		dirty_ = false;
		return refreshStatus != AnimationStatus::InternalError;
	}

	AnimationStatus RetargetEditorModel::AutoMap() {
		if(!loaded_) {
			return AnimationStatus::NotReady;
		}
		autoMapDiagnostics_.clear();
		const RetargetProfileAsset before = candidate_;

		std::map<std::string, std::vector<const SkeletonJoint*>> sourceBySemantic;
		for(const SkeletonJoint& joint : sourceSkeleton_.joints) {
			const std::string semantic = NormalizeSemantic(joint.semanticRole);
			if(!semantic.empty()) {
				sourceBySemantic[semantic].push_back(&joint);
			}
		}
		std::map<std::string, std::vector<const SkeletonJoint*>> targetBySemantic;
		for(const SkeletonJoint& joint : targetSkeleton_.joints) {
			const std::string semantic = NormalizeSemantic(joint.semanticRole);
			if(!semantic.empty()) {
				targetBySemantic[semantic].push_back(&joint);
			}
		}
		auto sortJoints = [](auto& groups) {
			for(auto& [semantic, joints] : groups) {
				static_cast<void>(semantic);
				std::sort(joints.begin(), joints.end(), [](const SkeletonJoint* left,
					const SkeletonJoint* right) { return left->stableId < right->stableId; });
			}
		};
		sortJoints(sourceBySemantic);
		sortJoints(targetBySemantic);

		// Existing semantic corrections and explicit rows are authored intent. They
		// participate in collision avoidance but are never deleted or overwritten by
		// assistance, which also preserves their basis/fallback/contact references.
		std::unordered_set<std::uint64_t> usedTargets;
		std::unordered_map<std::uint64_t, std::uint64_t> mappingBySourceJoint;
		std::uint64_t nextMappingId = 1U;
		for(const RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			usedTargets.insert(mapping.targetJointId);
			mappingBySourceJoint.emplace(mapping.sourceJointId, mapping.mappingId);
			nextMappingId = (std::max)(nextMappingId, mapping.mappingId + 1U);
		}
		for(const RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			usedTargets.insert(mapping.targetJointId);
			mappingBySourceJoint.emplace(mapping.sourceJointId, mapping.mappingId);
			nextMappingId = (std::max)(nextMappingId, mapping.mappingId + 1U);
		}

		std::vector<std::uint64_t> addedMappingIds;
		for(const auto& [semantic, sources] : sourceBySemantic) {
			const auto targetGroup = targetBySemantic.find(semantic);
			if(targetGroup == targetBySemantic.end()) {
				continue;
			}

			std::vector<const SkeletonJoint*> unmappedSources;
			std::copy_if(sources.begin(), sources.end(), std::back_inserter(unmappedSources),
				[&mappingBySourceJoint](const SkeletonJoint* joint) {
					return !mappingBySourceJoint.contains(joint->stableId);
				});
			std::vector<const SkeletonJoint*> availableTargets;
			std::copy_if(targetGroup->second.begin(), targetGroup->second.end(),
				std::back_inserter(availableTargets), [&usedTargets](const SkeletonJoint* joint) {
					return !usedTargets.contains(joint->stableId);
				});
			if(unmappedSources.empty()) {
				continue;
			}
			if(sources.size() != 1U || targetGroup->second.size() != 1U
				|| unmappedSources.size() != 1U || availableTargets.size() != 1U) {
				RetargetAutoMapDiagnostic diagnostic;
				diagnostic.semantic = semantic;
				for(const SkeletonJoint* source : sources) {
					diagnostic.sourceJointIds.push_back(source->stableId);
				}
				for(const SkeletonJoint* target : targetGroup->second) {
					diagnostic.targetJointIds.push_back(target->stableId);
				}
				diagnostic.message = "Semantic '" + semantic
					+ "' is ambiguous; existing manual rows were preserved and no guess was added.";
				autoMapDiagnostics_.push_back(std::move(diagnostic));
				continue;
			}

			const SkeletonJoint* source = unmappedSources.front();
			const SkeletonJoint* target = availableTargets.front();
			RetargetSemanticMapping mapping;
			mapping.mappingId = nextMappingId++;
			mapping.sourceJointId = source->stableId;
			mapping.targetJointId = target->stableId;
			mapping.sourceSemantic = source->semanticRole;
			mapping.targetSemantic = target->semanticRole;
			mapping.required = true;
			mappingBySourceJoint[source->stableId] = mapping.mappingId;
			usedTargets.insert(target->stableId);
			addedMappingIds.push_back(mapping.mappingId);
			candidate_.semanticMappings.push_back(std::move(mapping));
		}

		for(RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			if(std::find(addedMappingIds.begin(), addedMappingIds.end(), mapping.mappingId)
				== addedMappingIds.end()) {
				continue;
			}
			const std::optional<std::size_t> index = FindJointIndex(
				sourceSkeleton_, mapping.sourceJointId);
			if(!index.has_value()) {
				continue;
			}
			std::int32_t parent = sourceSkeleton_.joints[*index].parentIndex;
			while(parent >= 0 && static_cast<std::size_t>(parent) < sourceSkeleton_.joints.size()) {
				const auto found = mappingBySourceJoint.find(
					sourceSkeleton_.joints[static_cast<std::size_t>(parent)].stableId);
				if(found != mappingBySourceJoint.end()) {
					mapping.parentMappingId = found->second;
					break;
				}
				parent = sourceSkeleton_.joints[static_cast<std::size_t>(parent)].parentIndex;
			}
		}

		std::sort(candidate_.semanticMappings.begin(), candidate_.semanticMappings.end(),
			[](const RetargetSemanticMapping& left, const RetargetSemanticMapping& right) {
				return left.mappingId < right.mappingId;
			});
		candidate_.sourceSkeletonAssetId = sourceSkeleton_.assetId;
		candidate_.targetSkeletonAssetId = targetSkeleton_.assetId;
		candidate_.sourceSkeletonFingerprint = sourceSkeleton_.compatibilityFingerprint;
		candidate_.targetSkeletonFingerprint = targetSkeleton_.compatibilityFingerprint;
		candidate_.sourceReferencePoseHash = sourceSkeleton_.referencePoseHash;
		candidate_.targetReferencePoseHash = targetSkeleton_.referencePoseHash;
		candidate_.root.sourceRootJointId = sourceSkeleton_.joints[sourceSkeleton_.rootJointIndex].stableId;
		candidate_.root.targetRootJointId = targetSkeleton_.joints[targetSkeleton_.rootJointIndex].stableId;
		dirty_ = dirty_ || candidate_ != before;
		return RefreshErrors();
	}

	bool RetargetEditorModel::SetMappingTarget(
		std::uint64_t mappingId,
		std::uint64_t targetJointId) {
		const SkeletonJoint* target = FindJoint(targetSkeleton_, targetJointId);
		if(!loaded_ || target == nullptr) {
			return false;
		}
		for(RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			if(mapping.mappingId == mappingId) {
				mapping.targetJointId = targetJointId;
				mapping.targetSemantic = target->semanticRole;
				dirty_ = true;
				static_cast<void>(RefreshErrors());
				return true;
			}
		}
		for(RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			if(mapping.mappingId == mappingId) {
				mapping.targetJointId = targetJointId;
				dirty_ = true;
				static_cast<void>(RefreshErrors());
				return true;
			}
		}
		return false;
	}

	bool RetargetEditorModel::SetMappingRequired(std::uint64_t mappingId, bool required) {
		for(RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			if(mapping.mappingId == mappingId) {
				mapping.required = required;
				dirty_ = true;
				static_cast<void>(Validate());
				return true;
			}
		}
		for(RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			if(mapping.mappingId == mappingId) {
				mapping.required = required;
				dirty_ = true;
				static_cast<void>(Validate());
				return true;
			}
		}
		return false;
	}

	bool RetargetEditorModel::AddExplicitMapping(
		std::uint64_t sourceJointId,
		std::uint64_t targetJointId) {
		if(!loaded_ || FindJoint(sourceSkeleton_, sourceJointId) == nullptr
			|| FindJoint(targetSkeleton_, targetJointId) == nullptr) {
			return false;
		}
		std::uint64_t nextId = 1U;
		for(const RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			nextId = std::max(nextId, mapping.mappingId + 1U);
		}
		for(const RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			nextId = std::max(nextId, mapping.mappingId + 1U);
		}
		candidate_.explicitMappings.push_back({nextId, 0U, 0U, 0U,
			sourceJointId, targetJointId, true});
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	bool RetargetEditorModel::RemoveMapping(std::uint64_t mappingId) {
		const auto semanticEnd = std::remove_if(candidate_.semanticMappings.begin(),
			candidate_.semanticMappings.end(), [mappingId](const RetargetSemanticMapping& mapping) {
				return mapping.mappingId == mappingId;
			});
		const auto explicitEnd = std::remove_if(candidate_.explicitMappings.begin(),
			candidate_.explicitMappings.end(), [mappingId](const RetargetExplicitJointMapping& mapping) {
				return mapping.mappingId == mappingId;
			});
		const bool removed = semanticEnd != candidate_.semanticMappings.end()
			|| explicitEnd != candidate_.explicitMappings.end();
		candidate_.semanticMappings.erase(semanticEnd, candidate_.semanticMappings.end());
		candidate_.explicitMappings.erase(explicitEnd, candidate_.explicitMappings.end());
		if(!removed) {
			return false;
		}
		for(RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			if(mapping.parentMappingId == mappingId) mapping.parentMappingId = 0U;
		}
		for(RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			if(mapping.parentMappingId == mappingId) mapping.parentMappingId = 0U;
		}
		candidate_.fallbackRules.erase(std::remove_if(candidate_.fallbackRules.begin(),
			candidate_.fallbackRules.end(), [mappingId](const RetargetFallbackRule& fallback) {
				return fallback.sourceMappingId == mappingId;
			}), candidate_.fallbackRules.end());
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	bool RetargetEditorModel::AddContact(std::uint64_t mappingId) {
		std::uint64_t sourceJointId = 0U;
		std::uint64_t targetJointId = 0U;
		for(const RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			if(mapping.mappingId == mappingId) {
				sourceJointId = mapping.sourceJointId;
				targetJointId = mapping.targetJointId;
			}
		}
		for(const RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			if(mapping.mappingId == mappingId) {
				sourceJointId = mapping.sourceJointId;
				targetJointId = mapping.targetJointId;
			}
		}
		if(sourceJointId == 0U || targetJointId == 0U) {
			return false;
		}
		std::uint64_t contactId = 1U;
		for(const RetargetContactRule& contact : candidate_.contacts) {
			contactId = std::max(contactId, contact.contactId + 1U);
		}
		candidate_.contacts.push_back({contactId, sourceJointId, targetJointId, 0U,
			kRetargetDefaultContactToleranceMeters, true, false, true});
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	bool RetargetEditorModel::SetContactTarget(
		std::uint64_t contactId,
		std::uint64_t targetJointId) {
		if(FindJoint(targetSkeleton_, targetJointId) == nullptr) {
			return false;
		}
		for(RetargetContactRule& contact : candidate_.contacts) {
			if(contact.contactId == contactId) {
				contact.targetJointId = targetJointId;
				contact.targetChainId = 0U;
				dirty_ = true;
				static_cast<void>(RefreshErrors());
				return true;
			}
		}
		return false;
	}

	bool RetargetEditorModel::SetContactTolerance(
		std::uint64_t contactId,
		float toleranceMeters) {
		if(!std::isfinite(toleranceMeters) || toleranceMeters <= 0.0F
			|| toleranceMeters > kRetargetDefaultContactToleranceMeters) {
			return false;
		}
		for(RetargetContactRule& contact : candidate_.contacts) {
			if(contact.contactId == contactId) {
				contact.toleranceMeters = toleranceMeters;
				dirty_ = true;
				static_cast<void>(RefreshErrors());
				return true;
			}
		}
		return false;
	}

	bool RetargetEditorModel::SetContactPolicy(
		std::uint64_t contactId,
		bool preservePosition,
		bool preserveOrientation,
		bool required) {
		for(RetargetContactRule& contact : candidate_.contacts) {
			if(contact.contactId == contactId) {
				contact.preservePosition = preservePosition;
				contact.preserveOrientation = preserveOrientation;
				contact.required = required;
				dirty_ = true;
				static_cast<void>(RefreshErrors());
				return true;
			}
		}
		return false;
	}

	bool RetargetEditorModel::AddChainToEffectors(
		std::uint64_t sourceEffectorJointId,
		std::uint64_t targetEffectorJointId) {
		if(!loaded_) {
			return false;
		}
		RetargetChainMapping chain;
		chain.sourceJointIds = RootToEffectorPath(sourceSkeleton_, sourceEffectorJointId);
		chain.targetJointIds = RootToEffectorPath(targetSkeleton_, targetEffectorJointId);
		if(chain.sourceJointIds.empty() || chain.targetJointIds.empty()) {
			return false;
		}
		for(const RetargetChainMapping& existing : candidate_.chainMappings) {
			if(existing.sourceJointIds == chain.sourceJointIds
				&& existing.targetJointIds == chain.targetJointIds) {
				return false;
			}
			chain.chainId = (std::max)(chain.chainId, existing.chainId);
		}
		++chain.chainId;
		chain.semantic = "Root-to-effector chain "
			+ std::to_string(sourceEffectorJointId) + " -> "
			+ std::to_string(targetEffectorJointId);
		chain.translationScaleMode = RetargetTranslationScaleMode::PerChainLength;
		chain.required = true;
		candidate_.chainMappings.push_back(std::move(chain));
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	bool RetargetEditorModel::SetContactTargetChain(
		std::uint64_t contactId,
		std::uint64_t targetChainId) {
		const auto chain = std::find_if(candidate_.chainMappings.begin(),
			candidate_.chainMappings.end(), [targetChainId](const RetargetChainMapping& candidate) {
				return candidate.chainId == targetChainId;
			});
		if(chain == candidate_.chainMappings.end()) {
			return false;
		}
		for(RetargetContactRule& contact : candidate_.contacts) {
			if(contact.contactId != contactId) {
				continue;
			}
			if(std::find(chain->targetJointIds.begin(), chain->targetJointIds.end(),
				contact.targetJointId) == chain->targetJointIds.end()) {
				return false;
			}
			contact.targetChainId = targetChainId;
			dirty_ = true;
			static_cast<void>(RefreshErrors());
			return true;
		}
		return false;
	}

	bool RetargetEditorModel::RemoveChain(std::uint64_t chainId) {
		const auto end = std::remove_if(candidate_.chainMappings.begin(),
			candidate_.chainMappings.end(), [chainId](const RetargetChainMapping& chain) {
				return chain.chainId == chainId;
			});
		if(end == candidate_.chainMappings.end()) {
			return false;
		}
		candidate_.chainMappings.erase(end, candidate_.chainMappings.end());
		// Contacts stay valid when their optional chain anchor is removed instead
		// of retaining a dangling ID which would make the entire profile invalid.
		for(RetargetContactRule& contact : candidate_.contacts) {
			if(contact.targetChainId == chainId) {
				contact.targetChainId = 0U;
			}
		}
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	bool RetargetEditorModel::RemoveContact(std::uint64_t contactId) {
		const auto end = std::remove_if(candidate_.contacts.begin(), candidate_.contacts.end(),
			[contactId](const RetargetContactRule& contact) {
				return contact.contactId == contactId;
			});
		if(end == candidate_.contacts.end()) {
			return false;
		}
		candidate_.contacts.erase(end, candidate_.contacts.end());
		dirty_ = true;
		static_cast<void>(RefreshErrors());
		return true;
	}

	AnimationStatus RetargetEditorModel::RefreshErrors() {
		if(!loaded_) {
			return AnimationStatus::NotReady;
		}
		RetargetQuantitativeErrorReport report;
		RetargetPosePreviewPacket preview;
		const std::vector<AnimationTransform> sourceModel = BuildModelTransforms(sourceSkeleton_);
		const std::vector<AnimationTransform> targetModel = BuildModelTransforms(targetSkeleton_);
		if(sourceModel.size() != sourceSkeleton_.joints.size()
			|| targetModel.size() != targetSkeleton_.joints.size()) {
			return AnimationStatus::InvalidArgument;
		}

		const AnimationFloat3 sourceRoot = sourceModel[sourceSkeleton_.rootJointIndex].translation;
		const AnimationFloat3 targetRoot = targetModel[targetSkeleton_.rootJointIndex].translation;
		preview.sourcePoseHash = sourceSkeleton_.referencePoseHash;
		preview.targetPoseHash = targetSkeleton_.referencePoseHash;
		for(std::size_t index = 0U; index < sourceSkeleton_.joints.size(); ++index) {
			const SkeletonJoint& joint = sourceSkeleton_.joints[index];
			const std::uint64_t parentId = joint.parentIndex >= 0
				&& static_cast<std::size_t>(joint.parentIndex) < sourceSkeleton_.joints.size()
				? sourceSkeleton_.joints[static_cast<std::size_t>(joint.parentIndex)].stableId : 0U;
			preview.sourceJoints.push_back({joint.stableId, parentId,
				Subtract(sourceModel[index].translation, sourceRoot)});
		}
		for(std::size_t index = 0U; index < targetSkeleton_.joints.size(); ++index) {
			const SkeletonJoint& joint = targetSkeleton_.joints[index];
			const std::uint64_t parentId = joint.parentIndex >= 0
				&& static_cast<std::size_t>(joint.parentIndex) < targetSkeleton_.joints.size()
				? targetSkeleton_.joints[static_cast<std::size_t>(joint.parentIndex)].stableId : 0U;
			preview.targetJoints.push_back({joint.stableId, parentId,
				Subtract(targetModel[index].translation, targetRoot)});
		}

		auto measure = [&](std::uint64_t mappingId, std::uint64_t sourceJointId,
			std::uint64_t targetJointId, std::uint64_t basisId) {
			const std::optional<std::size_t> sourceIndex = FindJointIndex(sourceSkeleton_, sourceJointId);
			const std::optional<std::size_t> targetIndex = FindJointIndex(targetSkeleton_, targetJointId);
			if(!sourceIndex.has_value() || !targetIndex.has_value()) {
				return;
			}
			RetargetMappingError error;
			error.mappingId = mappingId;
			error.sourceJointId = sourceJointId;
			error.targetJointId = targetJointId;
			error.appliedTranslationScale = MappingTranslationScale(candidate_,
				sourceJointId, targetJointId, sourceSkeleton_, targetSkeleton_,
				sourceModel, targetModel);
			const AnimationQuaternion alignment = MappingAlignment(candidate_, basisId);
			const AnimationFloat3 normalizedSource = Scale(Rotate(alignment,
				Subtract(sourceModel[*sourceIndex].translation, sourceRoot)),
				error.appliedTranslationScale);
			const AnimationFloat3 normalizedTarget =
				Subtract(targetModel[*targetIndex].translation, targetRoot);
			error.positionErrorMeters = Length(Subtract(normalizedSource, normalizedTarget));
			const AnimationQuaternion alignedSourceRotation = Multiply(
				Multiply(alignment, sourceModel[*sourceIndex].rotation), Conjugate(alignment));
			error.rotationErrorRadians = QuaternionError(alignedSourceRotation,
				targetModel[*targetIndex].rotation);
			error.usesAuthoredOrientationBasis = basisId != 0U;
			report.meanPositionErrorMeters += error.positionErrorMeters;
			report.maximumPositionErrorMeters = std::max(
				report.maximumPositionErrorMeters, error.positionErrorMeters);
			report.maximumRotationErrorRadians = std::max(
				report.maximumRotationErrorRadians, error.rotationErrorRadians);
			report.mappings.push_back(error);
		};
		for(const RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
			measure(mapping.mappingId, mapping.sourceJointId, mapping.targetJointId,
				mapping.basisId);
		}
		for(const RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
			measure(mapping.mappingId, mapping.sourceJointId, mapping.targetJointId,
				mapping.basisId);
		}

		for(const RetargetContactRule& contact : candidate_.contacts) {
			const std::optional<std::size_t> sourceIndex = FindJointIndex(
				sourceSkeleton_, contact.sourceJointId);
			const std::optional<std::size_t> targetIndex = FindJointIndex(
				targetSkeleton_, contact.targetJointId);
			if(!sourceIndex.has_value() || !targetIndex.has_value()) {
				continue;
			}
			std::uint64_t basisId = 0U;
			for(const RetargetSemanticMapping& mapping : candidate_.semanticMappings) {
				if(mapping.sourceJointId == contact.sourceJointId
					&& mapping.targetJointId == contact.targetJointId) {
					basisId = mapping.basisId;
					break;
				}
			}
			if(basisId == 0U) {
				for(const RetargetExplicitJointMapping& mapping : candidate_.explicitMappings) {
					if(mapping.sourceJointId == contact.sourceJointId
						&& mapping.targetJointId == contact.targetJointId) {
						basisId = mapping.basisId;
						break;
					}
				}
			}

			RetargetContactError error;
			error.contactId = contact.contactId;
			error.sourceJointId = contact.sourceJointId;
			error.targetJointId = contact.targetJointId;
			error.toleranceMeters = contact.toleranceMeters;
			error.preservePosition = contact.preservePosition;
			error.preserveOrientation = contact.preserveOrientation;
			error.required = contact.required;
			const AnimationQuaternion alignment = MappingAlignment(candidate_, basisId);
			const float scale = MappingTranslationScale(candidate_, contact.sourceJointId,
				contact.targetJointId, sourceSkeleton_, targetSkeleton_, sourceModel, targetModel);
			error.alignedSourcePosition = Scale(Rotate(alignment,
				Subtract(sourceModel[*sourceIndex].translation, sourceRoot)), scale);
			error.targetPosition = Subtract(targetModel[*targetIndex].translation, targetRoot);
			error.positionErrorMeters = Length(Subtract(
				error.alignedSourcePosition, error.targetPosition));
			const AnimationQuaternion alignedSourceRotation = Multiply(
				Multiply(alignment, sourceModel[*sourceIndex].rotation), Conjugate(alignment));
			error.orientationErrorRadians = QuaternionError(alignedSourceRotation,
				targetModel[*targetIndex].rotation);
			const bool positionAccepted = !contact.preservePosition
				|| error.positionErrorMeters <= contact.toleranceMeters;
			const bool orientationAccepted = !contact.preserveOrientation
				|| error.orientationErrorRadians <= candidate_.quality.facingToleranceRadians;
			error.withinTolerance = positionAccepted && orientationAccepted;
			if(error.withinTolerance) {
				++report.contactsWithinTolerance;
			}
			if(contact.preservePosition) {
				report.maximumContactPositionErrorMeters = (std::max)(
					report.maximumContactPositionErrorMeters, error.positionErrorMeters);
			}
			if(contact.preserveOrientation) {
				report.maximumContactOrientationErrorRadians = (std::max)(
					report.maximumContactOrientationErrorRadians,
					error.orientationErrorRadians);
			}
			report.contacts.push_back(error);
			preview.contacts.push_back(std::move(error));
		}
		std::size_t semanticSourceCount = 0U;
		for(const SkeletonJoint& joint : sourceSkeleton_.joints) {
			if(!NormalizeSemantic(joint.semanticRole).empty()) {
				++semanticSourceCount;
			}
		}
		const std::size_t authoredCount = candidate_.semanticMappings.size()
			+ candidate_.explicitMappings.size();
		const std::size_t coverageDenominator = semanticSourceCount == 0U
			? authoredCount : semanticSourceCount;
		report.coverage = coverageDenominator == 0U ? 0.0F
			: std::clamp(static_cast<float>(report.mappings.size())
				/ static_cast<float>(coverageDenominator), 0.0F, 1.0F);
		if(!report.mappings.empty()) {
			report.meanPositionErrorMeters /= static_cast<float>(report.mappings.size());
		}
		std::sort(report.mappings.begin(), report.mappings.end(),
			[](const RetargetMappingError& left, const RetargetMappingError& right) {
				return left.mappingId < right.mappingId;
			});
		errors_ = std::move(report);
		preview_ = std::move(preview);
		candidate_.quality.authoredCoverage = errors_.coverage;
		candidate_.quality.expectedMeanPositionErrorMeters = errors_.meanPositionErrorMeters;
		candidate_.quality.expectedMaximumPositionErrorMeters = (std::max)(
			errors_.maximumPositionErrorMeters, errors_.maximumContactPositionErrorMeters);
		candidate_.quality.expectedMaximumRotationErrorRadians = (std::max)(
			errors_.maximumRotationErrorRadians,
			errors_.maximumContactOrientationErrorRadians);
		const bool requiredContactsAccepted = std::all_of(errors_.contacts.begin(),
			errors_.contacts.end(), [](const RetargetContactError& contact) {
				return !contact.required || contact.withinTolerance;
			});
		candidate_.quality.tier = errors_.coverage >= 1.0F
			&& errors_.maximumPositionErrorMeters
				<= candidate_.quality.reachableEndEffectorToleranceMeters
			&& requiredContactsAccepted
			? RetargetQualityTier::Verified
			: (errors_.coverage >= 1.0F ? RetargetQualityTier::Production
				: RetargetQualityTier::Reduced);
		candidate_.quality.reviewReason =
			"Reference-pose error metrics refreshed by Retarget Editor.";
		dirty_ = true;
		return Validate();
	}

	AnimationStatus RetargetEditorModel::Validate() {
		if(!loaded_) {
			return AnimationStatus::NotReady;
		}
		RetargetValidationResult staged;
		const AnimationStatus status = ValidateRetargetProfile(
			candidate_, sourceSkeleton_, targetSkeleton_, staged);
		validation_ = std::move(staged);
		return status;
	}

	RetargetAuthoringService::RetargetAuthoringService()
		: transaction_("Grouped retarget profile edit") {}

	AnimationAuthoringStatus RetargetAuthoringService::Open(
		const std::filesystem::path& destination,
		const RetargetProfileAsset& profile,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		const RetargetAuthoringGenerationSnapshot& generation,
		std::span<const std::uint8_t> persistedBytes,
		bool bindSelectedSkeletons) noexcept {
		try {
			if(destination.extension() != ".teretarget" || persistedBytes.empty()
				|| !generation.IsValid()) {
				return AnimationAuthoringStatus::InvalidArgument;
			}
			RetargetProfileAsset decoded;
			if(DeserializeAnimationAssetForAuthoring(
				persistedBytes, decoded, DeserializeRetargetProfileAsset) != AnimationStatus::Ok
				|| decoded.assetId != profile.assetId || decoded.assetId != generation.profile.assetId) {
				return AnimationAuthoringStatus::ValidationFailed;
			}
			RetargetEditorModel stagedModel;
			if(!stagedModel.Begin(decoded, sourceSkeleton, targetSkeleton, generation,
				bindSelectedSkeletons)) {
				return AnimationAuthoringStatus::ValidationFailed;
			}
			destination_ = destination;
			model_ = std::move(stagedModel);
			published_ = {decoded.assetId, generation.profile.generation,
				BytesToString(persistedBytes)};
			generation_ = generation;
			transaction_ = AnimationAuthoringTransaction("Grouped retarget profile edit");
			history_.Clear();
			editing_ = false;
			diagnostic_ = "Retarget profile opened as a detached transaction candidate.";
			return AnimationAuthoringStatus::Ok;
		} catch(...) {
			diagnostic_ = "Retarget authoring could not allocate or decode the selected profile.";
			return AnimationAuthoringStatus::ValidationFailed;
		}
	}

	AnimationAuthoringStatus RetargetAuthoringService::BeginEdit() noexcept {
		if(!HasOpenDocument()) {
			return AnimationAuthoringStatus::Rejected;
		}
		if(editing_) {
			return AnimationAuthoringStatus::Ok;
		}
		const AnimationAuthoringStatus status = transaction_.Begin(
			published_, published_.assetId, published_.generation);
		if(status == AnimationAuthoringStatus::Ok) {
			editing_ = true;
			diagnostic_ = "Grouped retarget edit started from the exact published revision.";
		}
		return status;
	}

	RetargetEditorModel* RetargetAuthoringService::EditableModel() noexcept {
		return editing_ ? &model_ : nullptr;
	}

	AnimationAuthoringStatus RetargetAuthoringService::PrepareCommit(
		AnimationAuthoringCompileRequest& outRequest) noexcept {
		try {
			if(!editing_) {
				return AnimationAuthoringStatus::Rejected;
			}
			std::vector<std::uint8_t> bytes;
			if(SerializeRetargetProfileAsset(model_.Candidate(), bytes) != AnimationStatus::Ok) {
				diagnostic_ = "The complete retarget candidate could not be serialized.";
				return AnimationAuthoringStatus::ValidationFailed;
			}
			const std::string candidateBytes = BytesToString(bytes);
			if(candidateBytes == published_.bytes) {
				diagnostic_ = "The grouped retarget edit contains no durable changes.";
				return AnimationAuthoringStatus::Rejected;
			}
			const AnimationAuthoringStatus replaced = transaction_.Replace(candidateBytes);
			if(replaced != AnimationAuthoringStatus::Ok) {
				return replaced;
			}
			diagnostic_ = "Retarget candidate copied for worker-safe validation.";
			return transaction_.PrepareCompile(outRequest);
		} catch(...) {
			diagnostic_ = "The retarget candidate exceeded available authoring memory.";
			return AnimationAuthoringStatus::CapacityExceeded;
		}
	}

	RetargetAuthoringCompileProduct RetargetAuthoringService::CompileCandidate(
		const AnimationAuthoringCompileRequest& request,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton) noexcept {
		RetargetAuthoringCompileProduct product;
		product.authoring.assetId = request.assetId;
		product.authoring.expectedGeneration = request.expectedGeneration;
		product.authoring.requestId = request.requestId;
		const auto canceled = [&request]() {
			return request.cancellationRequested
				&& request.cancellationRequested->load(std::memory_order_acquire);
		};
		try {
			if(canceled()) {
				product.authoring.canceled = true;
				product.authoring.diagnostic = "Retarget validation was canceled before decoding.";
				return product;
			}
			RetargetProfileAsset candidate;
			if(DeserializeAnimationAssetForAuthoring(
				StringBytes(request.candidateBytes), candidate, DeserializeRetargetProfileAsset)
				!= AnimationStatus::Ok || candidate.assetId != request.assetId) {
				product.authoring.diagnostic = "Retarget candidate failed checked round-trip decoding.";
				return product;
			}
			if(canceled()) {
				product.authoring.canceled = true;
				product.authoring.diagnostic = "Retarget validation was canceled before cross-rig compilation.";
				return product;
			}
			RetargetCompiledProfile compiled;
			const AnimationStatus status = CompileRetargetProfile(candidate, sourceSkeleton,
				targetSkeleton, product.validation, compiled);
			if(status != AnimationStatus::Ok || !product.validation.publishable) {
				product.authoring.diagnostic = "Retarget candidate failed complete cross-rig validation: "
					+ std::string(ToString(status)) + ".";
				return product;
			}
			if(canceled()) {
				product.authoring.canceled = true;
				product.authoring.diagnostic = "Retarget validation was canceled before publication handoff.";
				return product;
			}
			product.authoring.validatedBytes = request.candidateBytes;
			product.authoring.succeeded = true;
			product.authoring.diagnostic = "Retarget serializer, decoder, and cross-rig compiler accepted the candidate.";
		} catch(...) {
			product.authoring.diagnostic = "Retarget validation failed without publishing after an internal allocation error.";
		}
		return product;
	}

	bool RetargetAuthoringService::CurrentRevisionMatches(
		const SnapshotProvider& currentSnapshot,
		AnimationAuthoringRevision& outCurrent) const noexcept {
		outCurrent = published_;
		try {
			const std::optional<RetargetAuthoringGenerationSnapshot> snapshot = currentSnapshot
				? currentSnapshot() : std::nullopt;
			const std::vector<std::uint8_t> bytes = ReadBytes(destination_);
			if(snapshot.has_value() && *snapshot == generation_
				&& !bytes.empty() && BytesToString(bytes) == published_.bytes) {
				return true;
			}
			// CompleteCompile interprets either a byte or generation mismatch as stale
			// and therefore never invokes the publisher for this copied worker result.
			outCurrent.bytes = BytesToString(bytes);
			++outCurrent.generation;
		} catch(...) {
			++outCurrent.generation;
		}
		return false;
	}

	AnimationAuthoringStatus RetargetAuthoringService::CompleteCommit(
		const RetargetAuthoringCompileProduct& product,
		const SnapshotProvider& currentSnapshot) noexcept {
		AnimationAuthoringRevision current;
		const bool currentMatches = CurrentRevisionMatches(currentSnapshot, current);
		const AnimationAuthoringRevision before = published_;
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		AnimationStatus publicationStatus = AnimationStatus::NotReady;
		const AnimationAuthoringStatus status = transaction_.CompleteCompile(
			product.authoring, current,
			[this, &currentSnapshot, &publicationStatus](const auto& expected,
				const auto& replacement, std::string& diagnostic) {
				const RetargetAuthoringPublishResult result = Publish(destination_, expected,
					replacement, model_.SourceSkeleton(), model_.TargetSkeleton(),
					generation_, currentSnapshot);
				publicationStatus = result.status;
				diagnostic = result.diagnostic;
				return result.published;
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic.empty()
				? product.authoring.diagnostic : publicationDiagnostic;
			if(!currentMatches || publicationStatus == AnimationStatus::StaleHandle) {
				return AnimationAuthoringStatus::Stale;
			}
			return status;
		}
		const AnimationAuthoringStatus historyStatus = history_.Commit(before, published);
		if(historyStatus != AnimationAuthoringStatus::Ok) {
			diagnostic_ = "Retarget bytes published, but the grouped history entry was rejected.";
			return historyStatus;
		}
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) {
			diagnostic_ = product.authoring.diagnostic;
		}
		return adopted;
	}

	AnimationAuthoringStatus RetargetAuthoringService::Cancel() noexcept {
		const AnimationAuthoringStatus status = transaction_.Cancel();
		if(status != AnimationAuthoringStatus::Canceled) {
			return status;
		}
		RetargetProfileAsset decoded;
		if(DeserializeAnimationAssetForAuthoring(
			StringBytes(published_.bytes), decoded, DeserializeRetargetProfileAsset)
			!= AnimationStatus::Ok
			|| !model_.Begin(decoded, model_.SourceSkeleton(), model_.TargetSkeleton(), generation_)) {
			return AnimationAuthoringStatus::ValidationFailed;
		}
		editing_ = false;
		diagnostic_ = "Grouped retarget edits canceled; the exact published revision was restored.";
		return status;
	}

	AnimationAuthoringStatus RetargetAuthoringService::Undo(
		const SnapshotProvider& currentSnapshot) noexcept {
		if(editing_) {
			return AnimationAuthoringStatus::Rejected;
		}
		AnimationAuthoringRevision current;
		if(!CurrentRevisionMatches(currentSnapshot, current)) {
			diagnostic_ = "Undo rejected because the retarget profile or a skeleton generation changed.";
			return AnimationAuthoringStatus::Stale;
		}
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		AnimationStatus publicationStatus = AnimationStatus::NotReady;
		const AnimationAuthoringStatus status = history_.Undo(published_,
			[this, &currentSnapshot, &publicationStatus](const auto& expected,
				const auto& replacement, std::string& diagnostic) {
				const RetargetAuthoringPublishResult result = Publish(destination_, expected,
					replacement, model_.SourceSkeleton(), model_.TargetSkeleton(),
					generation_, currentSnapshot);
				publicationStatus = result.status;
				diagnostic = result.diagnostic;
				return result.published;
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic;
			return publicationStatus == AnimationStatus::StaleHandle
				? AnimationAuthoringStatus::Stale : status;
		}
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) {
			diagnostic_ = "Undo atomically published the complete prior retarget revision.";
		}
		return adopted;
	}

	AnimationAuthoringStatus RetargetAuthoringService::Redo(
		const SnapshotProvider& currentSnapshot) noexcept {
		if(editing_) {
			return AnimationAuthoringStatus::Rejected;
		}
		AnimationAuthoringRevision current;
		if(!CurrentRevisionMatches(currentSnapshot, current)) {
			diagnostic_ = "Redo rejected because the retarget profile or a skeleton generation changed.";
			return AnimationAuthoringStatus::Stale;
		}
		AnimationAuthoringRevision published;
		std::string publicationDiagnostic;
		AnimationStatus publicationStatus = AnimationStatus::NotReady;
		const AnimationAuthoringStatus status = history_.Redo(published_,
			[this, &currentSnapshot, &publicationStatus](const auto& expected,
				const auto& replacement, std::string& diagnostic) {
				const RetargetAuthoringPublishResult result = Publish(destination_, expected,
					replacement, model_.SourceSkeleton(), model_.TargetSkeleton(),
					generation_, currentSnapshot);
				publicationStatus = result.status;
				diagnostic = result.diagnostic;
				return result.published;
			}, published, publicationDiagnostic);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = publicationDiagnostic;
			return publicationStatus == AnimationStatus::StaleHandle
				? AnimationAuthoringStatus::Stale : status;
		}
		const AnimationAuthoringStatus adopted = AdoptPublishedRevision(std::move(published));
		if(adopted == AnimationAuthoringStatus::Ok) {
			diagnostic_ = "Redo atomically published the complete later retarget revision.";
		}
		return adopted;
	}

	AnimationAuthoringStatus RetargetAuthoringService::AdoptPublishedRevision(
		AnimationAuthoringRevision revision) noexcept {
		try {
			RetargetProfileAsset decoded;
			if(DeserializeAnimationAssetForAuthoring(
				StringBytes(revision.bytes), decoded, DeserializeRetargetProfileAsset)
				!= AnimationStatus::Ok || decoded.assetId != revision.assetId) {
				return AnimationAuthoringStatus::ValidationFailed;
			}
			RetargetAuthoringGenerationSnapshot nextGeneration = generation_;
			const std::uint64_t bytesHash = HashBytes(StringBytes(revision.bytes));
			nextGeneration.profile = {revision.assetId, GenerationFromHash(bytesHash)};
			nextGeneration.persistedBytesHash = bytesHash;
			const SkeletonAsset sourceSkeleton = model_.SourceSkeleton();
			const SkeletonAsset targetSkeleton = model_.TargetSkeleton();
			if(!model_.Begin(decoded, sourceSkeleton, targetSkeleton, nextGeneration)) {
				return AnimationAuthoringStatus::ValidationFailed;
			}
			published_ = std::move(revision);
			generation_ = nextGeneration;
			editing_ = false;
			return AnimationAuthoringStatus::Ok;
		} catch(...) {
			return AnimationAuthoringStatus::ValidationFailed;
		}
	}

	bool RetargetAuthoringService::PublishRevision(
		const AnimationAuthoringRevision& expectedCurrent,
		const AnimationAuthoringRevision& replacement,
		const SnapshotProvider& currentSnapshot,
		std::string& outDiagnostic) noexcept {
		const RetargetAuthoringPublishResult result = Publish(destination_, expectedCurrent,
			replacement, model_.SourceSkeleton(), model_.TargetSkeleton(), generation_,
			currentSnapshot);
		outDiagnostic = result.diagnostic;
		return result.published;
	}

	RetargetAuthoringPublishResult RetargetAuthoringService::Publish(
		const std::filesystem::path& destination,
		const AnimationAuthoringRevision& expectedCurrent,
		const AnimationAuthoringRevision& replacement,
		const SkeletonAsset& sourceSkeleton,
		const SkeletonAsset& targetSkeleton,
		const RetargetAuthoringGenerationSnapshot& expected,
		const SnapshotProvider& currentSnapshot) try {
		RetargetAuthoringPublishResult result;
		if(destination.extension() != ".teretarget" || !expected.IsValid()
			|| !currentSnapshot || !expectedCurrent.assetId.IsValid()
			|| expectedCurrent.assetId != expected.profile.assetId
			|| replacement.assetId != expectedCurrent.assetId
			|| replacement.generation != expectedCurrent.generation + 1U
			|| expectedCurrent.bytes.empty() || replacement.bytes.empty()) {
			result.status = AnimationStatus::InvalidArgument;
			result.diagnostic = "Retarget publication requires complete exact before/after revisions and dependency generations.";
			return result;
		}

		RetargetProfileAsset candidate;
		if(DeserializeAnimationAssetForAuthoring(
			StringBytes(replacement.bytes), candidate, DeserializeRetargetProfileAsset)
			!= AnimationStatus::Ok || candidate.assetId != expectedCurrent.assetId) {
			result.status = AnimationStatus::InvalidArgument;
			result.diagnostic = "The replacement retarget revision failed checked GUID decoding.";
			return result;
		}
		if(sourceSkeleton.assetId != expected.sourceSkeleton.assetId
			|| targetSkeleton.assetId != expected.targetSkeleton.assetId
			|| sourceSkeleton.runtimeGeneration != expected.sourceSkeleton.generation
			|| targetSkeleton.runtimeGeneration != expected.targetSkeleton.generation
			|| candidate.sourceSkeletonFingerprint != expected.sourceFingerprint
			|| candidate.targetSkeletonFingerprint != expected.targetFingerprint
			|| candidate.sourceReferencePoseHash != expected.sourceReferencePoseHash
			|| candidate.targetReferencePoseHash != expected.targetReferencePoseHash) {
			result.status = AnimationStatus::StaleHandle;
			result.diagnostic = "The candidate does not match the captured source/target identities and generations.";
			return result;
		}

		RetargetValidationResult validation;
		RetargetCompiledProfile compiled;
		result.status = CompileRetargetProfile(candidate, sourceSkeleton,
			targetSkeleton, validation, compiled);
		if(result.status != AnimationStatus::Ok || !validation.publishable) {
			result.diagnostic = "The edited retarget profile failed complete cross-skeleton validation.";
			return result;
		}
		std::vector<std::uint8_t> canonicalBytes;
		if(SerializeRetargetProfileAsset(compiled.profile, canonicalBytes) != AnimationStatus::Ok
			|| BytesToString(canonicalBytes) != replacement.bytes) {
			result.status = AnimationStatus::InvalidArgument;
			result.diagnostic = "The validated retarget packet was not the exact replacement revision.";
			return result;
		}

		const std::filesystem::path staged = destination.parent_path()
			/ (destination.filename().string() + ".retarget-"
				+ std::to_string(g_RetargetStageSequence.fetch_add(1U,
					std::memory_order_relaxed)) + ".tmp");
		std::error_code cleanupError;
		std::filesystem::remove(staged, cleanupError);
		if(!WriteStagedBytes(staged, StringBytes(replacement.bytes), result.diagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			result.status = AnimationStatus::InternalError;
			return result;
		}

		// Compilation and staging are parallel-friendly. Only this short durable
		// boundary is serialized, and it compares the complete original bytes while
		// holding the same lock that protects the atomic replacement.
		std::scoped_lock publicationLock(g_RetargetPublicationMutex);
		const std::optional<RetargetAuthoringGenerationSnapshot> current = currentSnapshot();
		const std::vector<std::uint8_t> currentBytes = ReadBytes(destination);
		if(!current.has_value() || *current != expected || currentBytes.empty()
			|| BytesToString(currentBytes) != expectedCurrent.bytes) {
			std::filesystem::remove(staged, cleanupError);
			result.status = AnimationStatus::StaleHandle;
			result.diagnostic = "Publication lost its exact generation/byte compare-and-swap; newer bytes remain untouched.";
			return result;
		}
		if(!ReplaceFileAtomically(staged, destination, result.diagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			result.status = AnimationStatus::InternalError;
			return result;
		}
		result.status = AnimationStatus::Ok;
		result.published = true;
		result.diagnostic = "Validated retarget profile published with an exact generation/byte compare-and-swap.";
		return result;
	} catch(...) {
		RetargetAuthoringPublishResult result;
		result.status = AnimationStatus::InternalError;
		result.diagnostic = "Retarget publication failed closed after an internal filesystem or allocation error.";
		return result;
	}
}

#if !defined(TERRAENGINE_EDITOR_TESTS)
struct RetargetEditorWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<
		TerraEngine::Animation::RetargetProfileAsset> selection;
	TerraEngine::Editor::Animation::RetargetAuthoringService authoring;
	std::vector<float> measuredErrors;
	std::filesystem::path modelPath;
	std::uint64_t selectedMapping = 0U;
	std::uint64_t selectedChain = 0U;
	std::uint64_t selectedSourceJoint = 0U;
	std::uint64_t selectedTargetJoint = 0U;
	std::array<char, 33U> sourceSkeletonGuid{};
	std::array<char, 33U> targetSkeletonGuid{};
	std::filesystem::path bindingPath;
	std::string message;
};

RetargetEditorWindow::RetargetEditorWindow(MainEditorSubsystem* owner)
	: EditorWindow("Retarget Editor", false, "Animation/Authoring"),
	  owner_(owner),
	  state_(std::make_unique<State>()) {
}

RetargetEditorWindow::~RetargetEditorWindow() {
	// Closing the window signals any copied validation packet and guarantees that
	// an unsaved detached candidate never leaks into durable authoring state.
	if(state_ && state_->authoring.HasCandidate()) {
		static_cast<void>(state_->authoring.Cancel());
	}
}

void RetargetEditorWindow::SetErrors(std::vector<float> errors) {
	state_->measuredErrors = std::move(errors);
}

void RetargetEditorWindow::OnGui() {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Editor::Animation;
	state_->selection.Synchronize(owner_, ".teretarget", &DeserializeRetargetProfileAsset);
	const RetargetProfileAsset* selectedProfile = state_->selection.Get();
	if(!state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("Select a .teretarget profile to auto-map semantic joints, correct mappings and contacts, refresh errors, and publish a validated candidate.");
		return;
	}
	ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
	if(selectedProfile == nullptr) {
		ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
			state_->selection.Message().c_str());
		return;
	}

	if(!state_->authoring.HasOpenDocument() || state_->modelPath != state_->selection.Path()) {
		SkeletonAsset source;
		SkeletonAsset target;
		std::string diagnostic;
		const std::vector<std::uint8_t> persistedBytes = ReadBytes(state_->selection.Path());
		const std::optional<RetargetAuthoringGenerationSnapshot> snapshot = CaptureSnapshot(
			state_->selection.Path(), selectedProfile->assetId, &source, &target, &diagnostic);
		if(!snapshot.has_value()) {
			if(state_->bindingPath != state_->selection.Path()) {
				CopyGuidText(state_->sourceSkeletonGuid, selectedProfile->sourceSkeletonAssetId);
				CopyGuidText(state_->targetSkeletonGuid, selectedProfile->targetSkeletonAssetId);
				state_->bindingPath = state_->selection.Path();
			}
			ImGui::TextWrapped("This starter profile has no checked skeleton bindings yet. Select generated package-child skeletons below. The profile on disk is not changed until you save a validated candidate.");
			const std::vector<RetargetSkeletonCandidate> candidates =
				DiscoverRetargetSkeletonCandidates();
			DrawSkeletonBindingPicker("Source skeleton GUID", state_->sourceSkeletonGuid, candidates);
			DrawSkeletonBindingPicker("Target skeleton GUID", state_->targetSkeletonGuid, candidates);
			if(candidates.empty()) {
				ImGui::TextDisabled("No generated skeleton package children are registered yet. Import a model with a skeleton first.");
			}
			if(ImGui::Button("Bind checked skeletons")) {
				const std::optional<TerraEngine::UUID> sourceId =
					TerraEngine::ParseUUID(state_->sourceSkeletonGuid.data());
				const std::optional<TerraEngine::UUID> targetId =
					TerraEngine::ParseUUID(state_->targetSkeletonGuid.data());
				if(!sourceId.has_value() || !targetId.has_value()) {
					state_->message = "Enter or select two 32-character skeleton GUIDs.";
				} else {
					const std::optional<RetargetAuthoringGenerationSnapshot> bindingSnapshot =
						CaptureStarterBindingSnapshot(state_->selection.Path(), selectedProfile->assetId,
							{*sourceId}, {*targetId}, &source, &target, &diagnostic);
					if(!bindingSnapshot.has_value()
						|| state_->authoring.Open(state_->selection.Path(), *selectedProfile,
							source, target, *bindingSnapshot, persistedBytes, true)
							!= AnimationAuthoringStatus::Ok) {
						state_->message = diagnostic.empty()
							? "The selected skeleton GUIDs were rejected; the last-good profile remains unchanged."
							: diagnostic;
					} else {
						state_->modelPath = state_->selection.Path();
						state_->selectedSourceJoint = source.joints.empty()
							? 0U : source.joints.front().stableId;
						state_->selectedTargetJoint = target.joints.empty()
							? 0U : target.joints.front().stableId;
						state_->message = "Checked skeletons are bound in a detached candidate. Auto-map or add mappings, then save.";
					}
				}
			}
			if(!state_->message.empty()) {
				ImGui::TextWrapped("%s", state_->message.c_str());
			}
			return;
		}
		if(state_->authoring.Open(state_->selection.Path(), *selectedProfile,
			source, target, *snapshot, persistedBytes) != AnimationAuthoringStatus::Ok) {
			ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
				"The profile could not be opened as a detached checked authoring candidate.");
			return;
		}
		state_->modelPath = state_->selection.Path();
		state_->selectedSourceJoint = source.joints.empty() ? 0U : source.joints.front().stableId;
		state_->selectedTargetJoint = target.joints.empty() ? 0U : target.joints.front().stableId;
		state_->message = "Detached authoring candidate loaded.";
	}

	const RetargetEditorModel& model = state_->authoring.Model();
	const RetargetProfileAsset& profile = model.Candidate();
	const RetargetValidationResult& validation = model.Validation();
	const std::filesystem::path path = state_->selection.Path();
	const AnimationGuid profileId = profile.assetId;
	const AnimationGuid sourceId = model.Generation().sourceSkeleton.assetId;
	const AnimationGuid targetId = model.Generation().targetSkeleton.assetId;
	const auto snapshotProvider = [path, profileId, sourceId, targetId]() {
		// The same checked source/target IDs protect a new starter binding until
		// it is saved, and protect ordinary profiles after they have been saved.
		return CaptureStarterBindingSnapshot(path, profileId, sourceId, targetId);
	};
	const auto beginEdit = [&]() -> RetargetEditorModel* {
		const AnimationAuthoringStatus status = state_->authoring.BeginEdit();
		if(status != AnimationAuthoringStatus::Ok) {
			state_->message = "Could not begin grouped retarget edit.";
			return nullptr;
		}
		return state_->authoring.EditableModel();
	};
	if(state_->selection.IsStale()) {
		ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
			"The latest file was rejected; Save is disabled until a complete current snapshot reloads.");
	}

	if(ImGui::Button("Auto-map semantics")) {
		if(RetargetEditorModel* editable = beginEdit()) {
			const AnimationStatus status = editable->AutoMap();
			state_->message = std::string("Auto-map: ") + std::string(ToString(status));
		}
	}
	ImGui::SameLine();
	if(ImGui::Button("Refresh quantitative errors")) {
		if(RetargetEditorModel* editable = beginEdit()) {
			const AnimationStatus status = editable->RefreshErrors();
			state_->message = std::string("Error refresh: ") + std::string(ToString(status));
		}
	}
	ImGui::SameLine();
	const bool canPublish = state_->authoring.HasCandidate() && model.IsDirty()
		&& validation.publishable
		&& !state_->selection.IsStale();
	ImGui::BeginDisabled(!canPublish);
	if(ImGui::Button("Save validated profile")) {
		AnimationAuthoringCompileRequest request;
		AnimationAuthoringStatus status = state_->authoring.PrepareCommit(request);
		if(status == AnimationAuthoringStatus::Ok) {
			const RetargetAuthoringCompileProduct product = RetargetAuthoringService::CompileCandidate(
				request, model.SourceSkeleton(), model.TargetSkeleton());
			status = state_->authoring.CompleteCommit(product, snapshotProvider);
		}
		state_->message = state_->authoring.Diagnostic();
		if(status == AnimationAuthoringStatus::Ok) {
			// AssetDatabase remains the owner of import registration and runtime
			// hot reload after the durable checked bytes have been committed.
			AssetDatabase::Instance().ImportAssetWithProgress(path, true);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!state_->authoring.HasCandidate());
	if(ImGui::Button("Cancel edit")) {
		static_cast<void>(state_->authoring.Cancel());
		state_->message = state_->authoring.Diagnostic();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(state_->authoring.HasCandidate()
		|| state_->authoring.UndoDepth() == 0U || state_->selection.IsStale());
	if(ImGui::Button("Undo")) {
		const AnimationAuthoringStatus status = state_->authoring.Undo(snapshotProvider);
		state_->message = state_->authoring.Diagnostic();
		if(status == AnimationAuthoringStatus::Ok) {
			AssetDatabase::Instance().ImportAssetWithProgress(path, true);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(state_->authoring.HasCandidate()
		|| state_->authoring.RedoDepth() == 0U || state_->selection.IsStale());
	if(ImGui::Button("Redo")) {
		const AnimationAuthoringStatus status = state_->authoring.Redo(snapshotProvider);
		state_->message = state_->authoring.Diagnostic();
		if(status == AnimationAuthoringStatus::Ok) {
			AssetDatabase::Instance().ImportAssetWithProgress(path, true);
		}
	}
	ImGui::EndDisabled();

	const ImVec4 statusColor = validation.publishable
		? ImVec4(0.35F, 0.85F, 0.45F, 1.0F) : ImVec4(1.0F, 0.45F, 0.35F, 1.0F);
	ImGui::TextColored(statusColor, "%s | %s | %s", validation.publishable
		? "Publishable" : "Invalid candidate",
		state_->authoring.HasCandidate() ? "grouped edit active" : "saved",
		state_->message.c_str());

	if(ImGui::BeginTable("RetargetSummary", 2,
		ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
		auto row = [](const char* name, const std::string& value) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
			ImGui::TableNextColumn(); ImGui::TextWrapped("%s", value.c_str());
		};
		row("Source Skeleton", TerraEngine::ToHexString(profile.sourceSkeletonAssetId.value));
		row("Target Skeleton", TerraEngine::ToHexString(profile.targetSkeletonAssetId.value));
		row("Source Generation", std::to_string(model.Generation().sourceSkeleton.generation));
		row("Target Generation", std::to_string(model.Generation().targetSkeleton.generation));
		row("Quality Tier", QualityTierName(profile.quality.tier));
		row("Validation", std::string(ToString(validation.status)));
		ImGui::EndTable();
	}

	if(!ImGui::BeginTabBar("RetargetEditorTabs")) {
		return;
	}
	if(ImGui::BeginTabItem("Side-by-side Mapping")) {
		DrawRetargetPosePreview(model.Preview(), state_->selectedSourceJoint,
			state_->selectedTargetJoint);
		for(const RetargetAutoMapDiagnostic& diagnostic : model.AutoMapDiagnostics()) {
			ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.2F, 1.0F),
				"Ambiguous semantic: %s", diagnostic.message.c_str());
		}
		if(ImGui::BeginTable("SkeletonPair", 2,
			ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
			ImGui::TableSetupColumn("Source skeleton");
			ImGui::TableSetupColumn("Target skeleton");
			ImGui::TableHeadersRow();
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			DrawSkeletonColumn("SourceRig", model.SourceSkeleton(), state_->selectedSourceJoint);
			ImGui::TableNextColumn();
			DrawSkeletonColumn("TargetRig", model.TargetSkeleton(), state_->selectedTargetJoint);
			ImGui::EndTable();
		}
		ImGui::BeginDisabled(state_->selectedSourceJoint == 0U
			|| state_->selectedTargetJoint == 0U);
		if(ImGui::Button("Add selected explicit mapping")) {
			RetargetEditorModel* editable = beginEdit();
			if(editable != nullptr && editable->AddExplicitMapping(
				state_->selectedSourceJoint, state_->selectedTargetJoint)) {
				state_->message = "Explicit mapping added; review validation below.";
			}
		}
		ImGui::EndDisabled();

		std::uint64_t removeMapping = 0U;
		if(ImGui::BeginTable("EditableRetargetMappings", 6,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
			ImVec2(0.0F, 300.0F))) {
			for(const char* heading : {"ID", "Source", "Semantic", "Target correction", "Required", "Remove"}) {
				ImGui::TableSetupColumn(heading);
			}
			ImGui::TableHeadersRow();
			auto drawMapping = [&](std::uint64_t mappingId, std::uint64_t sourceJointId,
				std::uint64_t targetJointId, const char* semantic, bool required) {
				ImGui::TableNextRow();
				ImGui::PushID(static_cast<int>(mappingId));
				ImGui::TableNextColumn();
				if(ImGui::Selectable(std::to_string(mappingId).c_str(),
					state_->selectedMapping == mappingId)) state_->selectedMapping = mappingId;
				ImGui::TableNextColumn();
				const SkeletonJoint* source = FindJoint(model.SourceSkeleton(), sourceJointId);
				ImGui::TextUnformatted(source == nullptr ? "(missing)" : JointLabel(*source).c_str());
				ImGui::TableNextColumn(); ImGui::TextUnformatted(semantic);
				ImGui::TableNextColumn();
				std::uint64_t selectedTarget = targetJointId;
				if(DrawTargetJointCombo("##Target", model.TargetSkeleton(), targetJointId,
					selectedTarget)) {
					if(RetargetEditorModel* editable = beginEdit()) {
						static_cast<void>(editable->SetMappingTarget(mappingId, selectedTarget));
					}
				}
				ImGui::TableNextColumn();
				bool editedRequired = required;
				if(ImGui::Checkbox("##Required", &editedRequired)) {
					if(RetargetEditorModel* editable = beginEdit()) {
						static_cast<void>(editable->SetMappingRequired(mappingId, editedRequired));
					}
				}
				ImGui::TableNextColumn();
				if(ImGui::SmallButton("Delete")) removeMapping = mappingId;
				ImGui::PopID();
			};
			for(const RetargetSemanticMapping& mapping : profile.semanticMappings) {
				drawMapping(mapping.mappingId, mapping.sourceJointId, mapping.targetJointId,
					mapping.sourceSemantic.c_str(), mapping.required);
			}
			for(const RetargetExplicitJointMapping& mapping : profile.explicitMappings) {
				drawMapping(mapping.mappingId, mapping.sourceJointId, mapping.targetJointId,
					"explicit", mapping.required);
			}
			ImGui::EndTable();
		}
		if(removeMapping != 0U) {
			if(RetargetEditorModel* editable = beginEdit()) {
				static_cast<void>(editable->RemoveMapping(removeMapping));
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Contacts / Root")) {
		ImGui::Text("Root: %llu -> %llu   displacement scale %.4f",
			static_cast<unsigned long long>(profile.root.sourceRootJointId),
			static_cast<unsigned long long>(profile.root.targetRootJointId),
			profile.root.displacementScale);
		ImGui::BeginDisabled(state_->selectedMapping == 0U);
		if(ImGui::Button("Add contact from selected mapping")) {
			if(RetargetEditorModel* editable = beginEdit()) {
				if(editable->AddContact(state_->selectedMapping)) {
					const RetargetContactRule& added = editable->Candidate().contacts.back();
					if(state_->selectedChain != 0U) {
						static_cast<void>(editable->SetContactTargetChain(
							added.contactId, state_->selectedChain));
					}
					state_->message = "Contact rule added to the selected mapping.";
				}
			}
		}
		ImGui::EndDisabled();
		std::uint64_t removeContact = 0U;
		for(const RetargetContactRule& contact : profile.contacts) {
			ImGui::PushID(static_cast<int>(contact.contactId));
			if(ImGui::TreeNode("Contact", "Contact #%llu",
				static_cast<unsigned long long>(contact.contactId))) {
				const auto measured = std::find_if(model.Errors().contacts.begin(),
					model.Errors().contacts.end(), [&contact](const RetargetContactError& error) {
						return error.contactId == contact.contactId;
					});
				if(measured != model.Errors().contacts.end()) {
					const ImVec4 color = measured->withinTolerance
						? ImVec4(0.35F, 0.85F, 0.45F, 1.0F)
						: ImVec4(1.0F, 0.45F, 0.35F, 1.0F);
					ImGui::TextColored(color, "%s | position %.5f / %.5f m | orientation %.5f rad",
						measured->withinTolerance ? "Within tolerance" : "Contact error",
						measured->positionErrorMeters, measured->toleranceMeters,
						measured->orientationErrorRadians);
				}
				std::uint64_t selectedTarget = contact.targetJointId;
				if(DrawTargetJointCombo("Target joint", model.TargetSkeleton(),
					contact.targetJointId, selectedTarget)) {
					if(RetargetEditorModel* editable = beginEdit()) {
						static_cast<void>(editable->SetContactTarget(contact.contactId, selectedTarget));
					}
				}
				const RetargetChainMapping* selectedChain = nullptr;
				for(const RetargetChainMapping& chain : profile.chainMappings) {
					if(chain.chainId == contact.targetChainId) {
						selectedChain = &chain;
						break;
					}
				}
				const std::string chainPreview = selectedChain == nullptr
					? "No target chain" : selectedChain->semantic;
				if(ImGui::BeginCombo("Target chain", chainPreview.c_str())) {
					for(const RetargetChainMapping& chain : profile.chainMappings) {
						if(std::find(chain.targetJointIds.begin(), chain.targetJointIds.end(),
							contact.targetJointId) == chain.targetJointIds.end()) {
							continue;
						}
						const bool active = chain.chainId == contact.targetChainId;
						if(ImGui::Selectable(chain.semantic.c_str(), active)) {
							if(RetargetEditorModel* editable = beginEdit()) {
								static_cast<void>(editable->SetContactTargetChain(
									contact.contactId, chain.chainId));
							}
						}
						if(active) {
							ImGui::SetItemDefaultFocus();
						}
					}
					ImGui::EndCombo();
				}
				float tolerance = contact.toleranceMeters;
				if(ImGui::SliderFloat("Tolerance (m)", &tolerance, 0.0001F,
					kRetargetDefaultContactToleranceMeters, "%.4f")) {
					if(RetargetEditorModel* editable = beginEdit()) {
						static_cast<void>(editable->SetContactTolerance(contact.contactId, tolerance));
					}
				}
				bool position = contact.preservePosition;
				bool orientation = contact.preserveOrientation;
				bool required = contact.required;
				bool changed = ImGui::Checkbox("Preserve position", &position);
				changed = ImGui::Checkbox("Preserve orientation", &orientation) || changed;
				changed = ImGui::Checkbox("Required", &required) || changed;
				if(changed) {
					if(RetargetEditorModel* editable = beginEdit()) {
						static_cast<void>(editable->SetContactPolicy(contact.contactId,
							position, orientation, required));
					}
				}
				if(ImGui::Button("Remove contact")) removeContact = contact.contactId;
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		if(removeContact != 0U) {
			if(RetargetEditorModel* editable = beginEdit()) {
				static_cast<void>(editable->RemoveContact(removeContact));
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Chains / Proportions")) {
		ImGui::Text("Default translation scale: %s", ScaleModeName(profile.proportion.translationScaleMode));
		ImGui::Text("Reference heights: %.3f m -> %.3f m",
			profile.proportion.sourceReferenceHeightMeters,
			profile.proportion.targetReferenceHeightMeters);
		ImGui::TextWrapped("Select a source and target joint in the mapping tab, then create a root-to-effector chain. A chain can be assigned to a contact only when it reaches that contact's target joint.");
		ImGui::BeginDisabled(state_->selectedSourceJoint == 0U
			|| state_->selectedTargetJoint == 0U);
		if(ImGui::Button("Create chain from selected joints")) {
			if(RetargetEditorModel* editable = beginEdit()) {
				if(editable->AddChainToEffectors(state_->selectedSourceJoint,
					state_->selectedTargetJoint)) {
					state_->selectedChain = editable->Candidate().chainMappings.back().chainId;
					state_->message = "Root-to-effector chain added; it may now anchor a reachable contact.";
				} else {
					state_->message = "The selected joints do not form a new valid root-to-effector chain.";
				}
			}
		}
		ImGui::EndDisabled();
		std::uint64_t removeChain = 0U;
		for(const RetargetChainMapping& chain : profile.chainMappings) {
			ImGui::PushID(static_cast<int>(chain.chainId));
			if(ImGui::Selectable(chain.semantic.c_str(), state_->selectedChain == chain.chainId)) {
				state_->selectedChain = chain.chainId;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("#%llu  %zu -> %zu joints",
				static_cast<unsigned long long>(chain.chainId), chain.sourceJointIds.size(),
				chain.targetJointIds.size());
			ImGui::SameLine();
			if(ImGui::SmallButton("Remove")) {
				removeChain = chain.chainId;
			}
			ImGui::PopID();
		}
		if(removeChain != 0U) {
			if(RetargetEditorModel* editable = beginEdit()) {
				if(editable->RemoveChain(removeChain)) {
					if(state_->selectedChain == removeChain) {
						state_->selectedChain = 0U;
					}
					state_->message = "Chain removed and any dependent contacts were safely unbound.";
				}
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Quality / Errors")) {
		const RetargetQuantitativeErrorReport& errors = model.Errors();
		ImGui::Text("Coverage: %.2f%%", errors.coverage * 100.0F);
		ImGui::Text("Mean / maximum position error: %.6f / %.6f m",
			errors.meanPositionErrorMeters, errors.maximumPositionErrorMeters);
		ImGui::Text("Maximum rotation error: %.6f rad", errors.maximumRotationErrorRadians);
		ImGui::Text("Contacts within tolerance: %u / %zu | max position %.6f m | max orientation %.6f rad",
			errors.contactsWithinTolerance, errors.contacts.size(),
			errors.maximumContactPositionErrorMeters,
			errors.maximumContactOrientationErrorRadians);
		if(ImGui::BeginTable("MappingErrors", 6,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
			ImVec2(0.0F, 250.0F))) {
			for(const char* heading : {"Mapping", "Joint pair", "Scale", "Basis",
				"Position (m)", "Rotation (rad)"}) {
				ImGui::TableSetupColumn(heading);
			}
			ImGui::TableHeadersRow();
			for(const RetargetMappingError& error : errors.mappings) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(error.mappingId));
				ImGui::TableNextColumn(); ImGui::Text("%llu -> %llu",
					static_cast<unsigned long long>(error.sourceJointId),
					static_cast<unsigned long long>(error.targetJointId));
				ImGui::TableNextColumn(); ImGui::Text("%.4f", error.appliedTranslationScale);
				ImGui::TableNextColumn(); ImGui::TextUnformatted(
					error.usesAuthoredOrientationBasis ? "authored" : "identity");
				ImGui::TableNextColumn(); ImGui::Text("%.6f", error.positionErrorMeters);
				ImGui::TableNextColumn(); ImGui::Text("%.6f", error.rotationErrorRadians);
			}
			ImGui::EndTable();
		}
		for(const RetargetValidationDiagnostic& diagnostic : validation.diagnostics) {
			const ImVec4 color = diagnostic.severity == RetargetDiagnosticSeverity::Error
				? ImVec4(1.0F, 0.45F, 0.35F, 1.0F) : ImVec4(1.0F, 0.8F, 0.3F, 1.0F);
			ImGui::TextColored(color, "[%u] %s",
				static_cast<unsigned int>(diagnostic.code), diagnostic.message.c_str());
		}
		for(std::size_t index = 0U; index < state_->measuredErrors.size(); ++index) {
			ImGui::BulletText("Preview joint %zu error: %.6f m", index,
				state_->measuredErrors[index]);
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}
#endif
