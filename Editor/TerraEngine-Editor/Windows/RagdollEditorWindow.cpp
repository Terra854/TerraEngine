// RagdollEditorWindow.cpp - persisted provider-neutral ragdoll authoring.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "RagdollEditorWindow.h"

#include "../Animation/AnimationAssetAuthoringDecode.h"
#include "Engine/Animation/Assets/AnimationClipAsset.h"

#ifndef TERRAENGINE_EDITOR_TESTS
#include "AnimationAssetWindowSupport.h"
#include "../Assets/AssetDatabase.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <imgui.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
	using namespace TerraEngine::Animation;
	using TerraEngine::Editor::Animation::DeserializeAnimationAssetForAuthoring;
	using TerraEngine::Editor::Animation::RagdollCollisionVisualizationEntry;
	using TerraEngine::Editor::Animation::RagdollRecoveryClipDependency;
	using TerraEngine::Editor::Animation::RagdollAuthoringGenerationSnapshot;
	using TerraEngine::Editor::Animation::RagdollVisualizationLine;

	std::atomic<std::uint64_t> g_RagdollStageSequence{1U};
	std::mutex g_RagdollPublicationRegistryMutex;
	std::map<std::string, std::weak_ptr<std::mutex>> g_RagdollPublicationMutexes;

	std::uint64_t HashBytes(std::span<const std::uint8_t> bytes) noexcept {
		std::uint64_t hash = 1469598103934665603ULL;
		for(const std::uint8_t byte : bytes) {
			hash ^= byte;
			hash *= 1099511628211ULL;
		}
		return hash;
	}

	AnimationGeneration GenerationFromHash(std::uint64_t hash) noexcept {
		AnimationGeneration result = static_cast<AnimationGeneration>(hash ^ (hash >> 32U));
		return result == 0U ? 1U : result;
	}

	std::uint64_t StablePairId(std::uint64_t left, std::uint64_t right) noexcept {
		std::uint64_t hash = 1469598103934665603ULL;
		for(const std::uint64_t value : {left, right}) {
			for(std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
				hash ^= static_cast<std::uint8_t>(value >> shift);
				hash *= 1099511628211ULL;
			}
		}
		return hash == 0U ? 1U : hash;
	}

	float Length(AnimationFloat3 value) noexcept {
		return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
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

	AnimationQuaternion Normalize(AnimationQuaternion value) noexcept {
		const float lengthSquared = value.x * value.x + value.y * value.y
			+ value.z * value.z + value.w * value.w;
		if(!std::isfinite(lengthSquared) || lengthSquared <= 0.000001F) return {};
		const float inverseLength = 1.0F / std::sqrt(lengthSquared);
		return {value.x * inverseLength, value.y * inverseLength,
			value.z * inverseLength, value.w * inverseLength};
	}

	AnimationQuaternion Multiply(AnimationQuaternion left, AnimationQuaternion right) noexcept {
		return Normalize({
			left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
			left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
			left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
			left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z});
	}

	AnimationFloat3 Rotate(AnimationQuaternion rotation, AnimationFloat3 value) noexcept {
		rotation = Normalize(rotation);
		const AnimationFloat3 axis{rotation.x, rotation.y, rotation.z};
		const AnimationFloat3 twiceCross{
			2.0F * (axis.y * value.z - axis.z * value.y),
			2.0F * (axis.z * value.x - axis.x * value.z),
			2.0F * (axis.x * value.y - axis.y * value.x)};
		return Add(value, Add({rotation.w * twiceCross.x, rotation.w * twiceCross.y,
			rotation.w * twiceCross.z}, {
			axis.y * twiceCross.z - axis.z * twiceCross.y,
			axis.z * twiceCross.x - axis.x * twiceCross.z,
			axis.x * twiceCross.y - axis.y * twiceCross.x}));
	}

	AnimationFloat3 InverseRotate(AnimationQuaternion rotation, AnimationFloat3 value) noexcept {
		rotation = Normalize(rotation);
		return Rotate({-rotation.x, -rotation.y, -rotation.z, rotation.w}, value);
	}

	AnimationTransform Compose(
		const AnimationTransform& parent,
		const AnimationTransform& local) noexcept {
		AnimationTransform result;
		result.translation = Add(parent.translation,
			Rotate(parent.rotation, Multiply(parent.scale, local.translation)));
		result.rotation = Multiply(parent.rotation, local.rotation);
		result.scale = Multiply(parent.scale, local.scale);
		return result;
	}

	AnimationTransform BodyPose(
		const AnimationTransform& jointPose,
		const RagdollBodyRecord& body) noexcept {
		AnimationTransform result;
		result.translation = Add(jointPose.translation,
			Rotate(jointPose.rotation, body.jointToBodyOffset));
		result.rotation = Multiply(jointPose.rotation, body.jointToBodyOrientation);
		return result;
	}

	std::vector<AnimationTransform> BuildModelBindPoses(const SkeletonAsset& skeleton) {
		std::vector<AnimationTransform> result(skeleton.joints.size());
		for(std::size_t jointIndex = 0U; jointIndex < skeleton.joints.size(); ++jointIndex) {
			const SkeletonJoint& joint = skeleton.joints[jointIndex];
			if(joint.parentIndex < 0) result[jointIndex] = joint.localBindTransform;
			else if(static_cast<std::size_t>(joint.parentIndex) < jointIndex) {
				result[jointIndex] = Compose(result[static_cast<std::size_t>(joint.parentIndex)],
					joint.localBindTransform);
			}
		}
		return result;
	}

	AnimationTransform ColliderPose(
		const AnimationTransform& bodyPose,
		const RagdollColliderRecord& collider) noexcept {
		AnimationTransform result;
		result.translation = Add(bodyPose.translation,
			Rotate(bodyPose.rotation, collider.localCenter));
		result.rotation = Multiply(bodyPose.rotation, collider.localOrientation);
		return result;
	}

	AnimationFloat3 TransformPoint(
		const AnimationTransform& transform,
		AnimationFloat3 local) noexcept {
		return Add(transform.translation, Rotate(transform.rotation, local));
	}

	void AddLine(
		std::vector<RagdollVisualizationLine>& lines,
		const AnimationTransform& pose,
		AnimationFloat3 localStart,
		AnimationFloat3 localEnd,
		std::uint64_t bodyId,
		std::uint64_t colliderId,
		bool selected) {
		lines.push_back({TransformPoint(pose, localStart), TransformPoint(pose, localEnd),
			bodyId, colliderId, selected});
	}

	template<typename PointFactory>
	void AddRing(
		std::vector<RagdollVisualizationLine>& lines,
		const AnimationTransform& pose,
		PointFactory&& point,
		std::uint64_t bodyId,
		std::uint64_t colliderId,
		bool selected) {
		constexpr std::size_t kSegments = 16U;
		constexpr float kTwoPi = 6.2831853071795864769F;
		for(std::size_t segment = 0U; segment < kSegments; ++segment) {
			const float first = kTwoPi * static_cast<float>(segment) / static_cast<float>(kSegments);
			const float second = kTwoPi * static_cast<float>(segment + 1U) / static_cast<float>(kSegments);
			AddLine(lines, pose, point(first), point(second), bodyId, colliderId, selected);
		}
	}

	void AddColliderGeometry(
		std::vector<RagdollVisualizationLine>& lines,
		const RagdollColliderRecord& collider,
		const AnimationTransform& pose,
		std::uint64_t bodyId,
		bool selected) {
		const float x = (std::max)(collider.dimensions.x, 0.001F);
		const float y = (std::max)(collider.dimensions.y, 0.001F);
		const float z = (std::max)(collider.dimensions.z, 0.001F);
		if(collider.shape == RagdollColliderShape::Sphere) {
			AddRing(lines, pose, [x](float angle) { return AnimationFloat3{
				x * std::cos(angle), x * std::sin(angle), 0.0F}; }, bodyId, collider.colliderId, selected);
			AddRing(lines, pose, [x](float angle) { return AnimationFloat3{
				x * std::cos(angle), 0.0F, x * std::sin(angle)}; }, bodyId, collider.colliderId, selected);
			AddRing(lines, pose, [x](float angle) { return AnimationFloat3{
				0.0F, x * std::cos(angle), x * std::sin(angle)}; }, bodyId, collider.colliderId, selected);
		} else if(collider.shape == RagdollColliderShape::Capsule) {
			AddRing(lines, pose, [x, y](float angle) { return AnimationFloat3{
				x * std::cos(angle), y, x * std::sin(angle)}; }, bodyId, collider.colliderId, selected);
			AddRing(lines, pose, [x, y](float angle) { return AnimationFloat3{
				x * std::cos(angle), -y, x * std::sin(angle)}; }, bodyId, collider.colliderId, selected);
			for(const AnimationFloat3 side : std::array<AnimationFloat3, 4U>{
				AnimationFloat3{x, 0.0F, 0.0F}, AnimationFloat3{-x, 0.0F, 0.0F},
				AnimationFloat3{0.0F, 0.0F, x}, AnimationFloat3{0.0F, 0.0F, -x}}) {
				AddLine(lines, pose, {side.x, -y, side.z}, {side.x, y, side.z},
					bodyId, collider.colliderId, selected);
			}
			// Two great-circle outlines make the hemispherical ends visible instead
			// of showing a misleading open cylinder.
			AddRing(lines, pose, [x, y](float angle) { return AnimationFloat3{
				x * std::cos(angle), (angle < 3.14159265F ? y : -y) + x * std::sin(angle), 0.0F}; },
				bodyId, collider.colliderId, selected);
		} else if(collider.shape == RagdollColliderShape::Box) {
			const std::array<AnimationFloat3, 8U> corners{{
				{-x, -y, -z}, {x, -y, -z}, {x, y, -z}, {-x, y, -z},
				{-x, -y, z}, {x, -y, z}, {x, y, z}, {-x, y, z}}};
			constexpr std::array<std::array<std::size_t, 2U>, 12U> edges{{
				{{0U, 1U}}, {{1U, 2U}}, {{2U, 3U}}, {{3U, 0U}},
				{{4U, 5U}}, {{5U, 6U}}, {{6U, 7U}}, {{7U, 4U}},
				{{0U, 4U}}, {{1U, 5U}}, {{2U, 6U}}, {{3U, 7U}}}};
			for(const auto& edge : edges) AddLine(lines, pose, corners[edge[0U]], corners[edge[1U]],
				bodyId, collider.colliderId, selected);
		} else if(collider.convexHullVertices.size() > 1U) {
			for(std::size_t index = 0U; index < collider.convexHullVertices.size(); ++index) {
				AddLine(lines, pose, collider.convexHullVertices[index],
					collider.convexHullVertices[(index + 1U) % collider.convexHullVertices.size()],
					bodyId, collider.colliderId, selected);
			}
		}
	}

	std::vector<std::uint8_t> ReadBytes(const std::filesystem::path& path) {
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		if(!input) return {};
		const std::streamoff size = input.tellg();
		if(size <= 0 || static_cast<std::uint64_t>(size)
			> kRagdollProfileMaximumPayloadBytes + 1024U * 1024U) return {};
		std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
		input.seekg(0, std::ios::beg);
		input.read(reinterpret_cast<char*>(bytes.data()), size);
		return input ? bytes : std::vector<std::uint8_t>{};
	}

	bool WriteStagedBytes(
		const std::filesystem::path& path,
		std::span<const std::uint8_t> bytes,
		std::string& diagnostic) {
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if(!output) {
			diagnostic = "Could not create the ragdoll staging file.";
			return false;
		}
		output.write(reinterpret_cast<const char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if(!output) {
			diagnostic = "Could not completely flush the ragdoll staging file.";
			return false;
		}
		return true;
	}

	bool ReplaceFileAtomically(
		const std::filesystem::path& staged,
		const std::filesystem::path& destination,
		std::string& diagnostic) {
#ifdef _WIN32
		std::error_code error;
		const bool exists = std::filesystem::exists(destination, error);
		if(error) {
			diagnostic = "Could not inspect the ragdoll destination before replacement.";
			return false;
		}
		const BOOL replaced = exists
			? ::ReplaceFileW(destination.c_str(), staged.c_str(), nullptr,
				REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)
			: ::MoveFileExW(staged.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH);
		if(replaced == FALSE) {
			diagnostic = "The operating system rejected the atomic ragdoll replacement.";
			return false;
		}
		return true;
#else
		std::error_code error;
		std::filesystem::rename(staged, destination, error);
		if(error) {
			diagnostic = "The filesystem rejected the atomic ragdoll replacement: "
				+ error.message();
			return false;
		}
		return true;
#endif
	}

	AnimationGeneration CheckedGeneration(std::uint64_t value) noexcept {
		return value == 0U || value > std::numeric_limits<AnimationGeneration>::max()
			? 0U : static_cast<AnimationGeneration>(value);
	}

	std::shared_ptr<std::mutex> PublicationMutexFor(const std::filesystem::path& path) {
		std::error_code error;
		std::filesystem::path keyPath = std::filesystem::absolute(path, error);
		if(error) keyPath = path;
		const std::string key = keyPath.lexically_normal().generic_string();
		std::scoped_lock registryLock(g_RagdollPublicationRegistryMutex);
		std::shared_ptr<std::mutex> result = g_RagdollPublicationMutexes[key].lock();
		if(!result) {
			result = std::make_shared<std::mutex>();
			g_RagdollPublicationMutexes[key] = result;
		}
		return result;
	}

	AnimationStatus ValidateRecoveryDependencies(
		const RagdollProfileAsset& profile,
		const SkeletonAsset& skeleton,
		std::span<const RagdollRecoveryClipDependency> dependencies,
		std::vector<std::string>& diagnostics) {
		diagnostics.clear();
		for(const RagdollRecoveryRecord& recovery : profile.recovery) {
			const auto dependency = std::find_if(dependencies.begin(), dependencies.end(),
				[&recovery](const RagdollRecoveryClipDependency& current) {
					return current.assetId == recovery.clipAssetId;
				});
			if(dependency == dependencies.end() || !dependency->IsValid()) {
				diagnostics.push_back("Recovery candidate " + std::to_string(recovery.candidateId)
					+ " references a clip that is missing from the current asset database.");
				continue;
			}
			if(dependency->generation != recovery.clipGeneration) {
				diagnostics.push_back("Recovery candidate " + std::to_string(recovery.candidateId)
					+ " has a stale clip generation.");
			}
			if(dependency->skeletonFingerprint != recovery.clipSkeletonFingerprint
				|| dependency->skeletonFingerprint != skeleton.compatibilityFingerprint) {
				diagnostics.push_back("Recovery candidate " + std::to_string(recovery.candidateId)
					+ " has an incompatible clip skeleton fingerprint.");
			}
			if(recovery.requiredReferencePoseHash != skeleton.referencePoseHash) {
				diagnostics.push_back("Recovery candidate " + std::to_string(recovery.candidateId)
					+ " has a stale reference-pose identity.");
			}
		}
		if(diagnostics.empty()) return AnimationStatus::Ok;
		return std::any_of(diagnostics.begin(), diagnostics.end(), [](const std::string& diagnostic) {
			return diagnostic.find("missing") != std::string::npos;
		}) ? AnimationStatus::MissingAsset : AnimationStatus::StaleHandle;
	}

	std::vector<RagdollRecoveryClipDependency> ResolveRecoveryDependencies(
		const RagdollProfileAsset& profile,
		const TerraEngine::Editor::Animation::RagdollRecoveryClipProvider& provider) {
		std::vector<RagdollRecoveryClipDependency> result;
		if(!profile.recovery.empty() && !provider) return result;
		for(const RagdollRecoveryRecord& recovery : profile.recovery) {
			if(std::any_of(result.begin(), result.end(), [&recovery](const auto& current) {
				return current.assetId == recovery.clipAssetId;
			})) continue;
			const auto dependency = provider(recovery.clipAssetId);
			if(dependency) result.push_back(*dependency);
		}
		return result;
	}

#ifndef TERRAENGINE_EDITOR_TESTS
	bool ResolveSkeleton(AnimationGuid assetId, SkeletonAsset& output, std::string& diagnostic) {
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
			if(compiledPath.is_relative()) compiledPath = database.GetAssetRoot() / compiledPath;
		}
		if(compiledPath.empty()) {
			diagnostic = "The profile skeleton is not registered in AssetDatabase.";
			return false;
		}
		const std::vector<std::uint8_t> bytes = ResourceManager::Instance().ReadFileBytes(compiledPath);
		SkeletonAsset skeleton;
		const AnimationStatus status = bytes.empty() ? AnimationStatus::MissingAsset
			: DeserializeAnimationAssetForAuthoring(bytes, skeleton, DeserializeSkeletonAsset);
		if(status != AnimationStatus::Ok || skeleton.assetId != assetId) {
			diagnostic = "The profile skeleton failed checked identity decoding: ";
			diagnostic += ToString(status);
			return false;
		}
		if(generation == 0U) generation = GenerationFromHash(HashBytes(bytes));
		skeleton.runtimeGeneration = generation;
		output = std::move(skeleton);
		return true;
	}

	std::optional<RagdollRecoveryClipDependency> ResolveRecoveryClip(AnimationGuid assetId) {
		AssetDatabase& database = AssetDatabase::Instance();
		std::filesystem::path compiledPath;
		AnimationGeneration generation = 0U;
		std::string displayName;
		GeneratedModelPackageChildMetadata generated;
		if(database.TryGetGeneratedModelPackageChildMetadata(
			assetId.value, GeneratedModelPackageChildKind::AnimationClip, generated)) {
			compiledPath = database.GetBuildRoot() / generated.compiledPath;
			generation = CheckedGeneration(generated.publishedGeneration);
			displayName = generated.displayName;
		} else if(const AssetMetadata* metadata = database.FindAssetByGuid(assetId.value)) {
			compiledPath = metadata->compiledPath.empty()
				? metadata->sourcePath : database.GetBuildRoot() / metadata->compiledPath;
			if(compiledPath.is_relative()) compiledPath = database.GetAssetRoot() / compiledPath;
			displayName = metadata->sourcePath.filename().string();
		}
		if(compiledPath.empty()) return std::nullopt;
		const std::vector<std::uint8_t> bytes = ResourceManager::Instance().ReadFileBytes(compiledPath);
		AnimationClipAsset clip;
		if(bytes.empty() || DeserializeAnimationAssetForAuthoring(
			bytes, clip, DeserializeAnimationClipAsset) != AnimationStatus::Ok
			|| clip.assetId != assetId) return std::nullopt;
		if(generation == 0U) generation = GenerationFromHash(HashBytes(bytes));
		return RagdollRecoveryClipDependency{
			clip.assetId, generation, clip.compatibilityFingerprint, std::move(displayName)};
	}

	std::vector<RagdollRecoveryClipDependency> DiscoverRecoveryClips(
		AnimationCompatibilityFingerprint requiredFingerprint) {
		AssetDatabase& database = AssetDatabase::Instance();
		std::vector<RagdollRecoveryClipDependency> result;
		const auto append = [&](AnimationGuid id) {
			const auto dependency = ResolveRecoveryClip(id);
			if(dependency && dependency->skeletonFingerprint == requiredFingerprint
				&& std::none_of(result.begin(), result.end(), [&](const auto& current) {
					return current.assetId == dependency->assetId;
				})) result.push_back(*dependency);
		};
		for(const auto& [path, metadata] : database.GetAssets()) {
			if(path.extension() == ".teanim" || metadata.sourcePath.extension() == ".teanim") {
				append(AnimationGuid{metadata.guid});
			}
			std::vector<TerraEngine::UUID> generatedIds;
			if(database.TryGetGeneratedModelPackageChildIds(metadata.guid,
				GeneratedModelPackageChildKind::AnimationClip, generatedIds)) {
				for(const TerraEngine::UUID& id : generatedIds) append(AnimationGuid{id});
			}
		}
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
			return left.displayName < right.displayName;
		});
		return result;
	}

	// A generated skeleton is a package child, not a normal visible source file.
	// Enumerating both kinds here keeps the picker useful for imported Fox GLB/FBX
	// packages as well as standalone cooked skeleton documents.
	struct RagdollSkeletonBindingOption {
		AnimationGuid assetId{};
		std::string displayName;
	};

	std::vector<RagdollSkeletonBindingOption> DiscoverRagdollSkeletonBindings() {
		AssetDatabase& database = AssetDatabase::Instance();
		std::vector<RagdollSkeletonBindingOption> result;
		const auto append = [&result](AnimationGuid assetId, std::string displayName) {
			if(!assetId.IsValid() || std::any_of(result.begin(), result.end(),
				[assetId](const RagdollSkeletonBindingOption& current) {
					return current.assetId == assetId;
				})) return;
			result.push_back({assetId, std::move(displayName)});
		};
		for(const auto& [path, metadata] : database.GetAssets()) {
			if(path.extension() == ".teskel" || metadata.sourcePath.extension() == ".teskel"
				|| metadata.compiledPath.extension() == ".teskel") {
				append(AnimationGuid{metadata.guid}, metadata.sourcePath.filename().string());
			}
			std::vector<TerraEngine::UUID> generatedIds;
			if(!database.TryGetGeneratedModelPackageChildIds(metadata.guid,
				GeneratedModelPackageChildKind::Skeleton, generatedIds)) continue;
			for(const TerraEngine::UUID& id : generatedIds) {
				GeneratedModelPackageChildMetadata generated;
				const bool found = database.TryGetGeneratedModelPackageChildMetadata(
					id, GeneratedModelPackageChildKind::Skeleton, generated);
				append(AnimationGuid{id}, found && !generated.displayName.empty()
					? generated.displayName : metadata.sourcePath.filename().string());
			}
		}
		std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
			return left.displayName < right.displayName;
		});
		return result;
	}

	std::vector<RagdollCollisionLayerRule> CurrentProjectCollisionLayers() {
		std::vector<RagdollCollisionLayerRule> result;
		const TerraEngine::Physics::PhysicsSettings settings =
			TerraEngine::Physics::PhysicsSystem::Instance().ConfiguredSettings();
		result.reserve(settings.layers.size());
		for(const TerraEngine::Physics::CollisionLayer& layer : settings.layers) {
			result.push_back({layer.id, layer.collisionMask, layer.enabled});
		}
		return result;
	}

	std::optional<RagdollAuthoringGenerationSnapshot> CaptureSnapshot(
		const std::filesystem::path& profilePath,
		AnimationGuid expectedProfileId,
		SkeletonAsset* skeletonOutput = nullptr,
		std::string* diagnosticOutput = nullptr,
		std::optional<AnimationGuid> skeletonOverride = std::nullopt) {
		const std::vector<std::uint8_t> bytes = ReadBytes(profilePath);
		RagdollProfileAsset profile;
		if(bytes.empty() || DeserializeAnimationAssetForAuthoring(
			bytes, profile, DeserializeRagdollProfileAsset)
			!= AnimationStatus::Ok || profile.assetId != expectedProfileId) {
			if(diagnosticOutput != nullptr) {
				*diagnosticOutput = "The selected ragdoll profile changed or no longer decodes.";
			}
			return std::nullopt;
		}
		SkeletonAsset skeleton;
		std::string diagnostic;
		// A newly-created profile deliberately has no trustworthy skeleton identity.
		// The authoring window may therefore supply a checked selection while keeping
		// the persisted starter bytes untouched until Validate & Save succeeds.
		const AnimationGuid skeletonId = skeletonOverride.value_or(profile.skeletonAssetId);
		if(!ResolveSkeleton(skeletonId, skeleton, diagnostic)) {
			if(diagnosticOutput != nullptr) *diagnosticOutput = std::move(diagnostic);
			return std::nullopt;
		}
		RagdollAuthoringGenerationSnapshot snapshot;
		snapshot.profile = {profile.assetId, GenerationFromHash(HashBytes(bytes))};
		snapshot.skeleton = {skeleton.assetId, skeleton.runtimeGeneration};
		snapshot.skeletonFingerprint = skeleton.compatibilityFingerprint;
		snapshot.referencePoseHash = skeleton.referencePoseHash;
		snapshot.persistedBytesHash = HashBytes(bytes);
		if(skeletonOutput != nullptr) *skeletonOutput = std::move(skeleton);
		return snapshot;
	}
#endif

#ifndef TERRAENGINE_EDITOR_TESTS
	const char* ShapeName(RagdollColliderShape shape) {
		switch(shape) {
		case RagdollColliderShape::Sphere: return "Sphere";
		case RagdollColliderShape::Capsule: return "Capsule";
		case RagdollColliderShape::Box: return "Box";
		case RagdollColliderShape::ConvexHull: return "Convex Hull";
		}
		return "Unknown";
	}

	const char* MotionTypeName(RagdollMotionType type) {
		switch(type) {
		case RagdollMotionType::Dynamic: return "Dynamic";
		case RagdollMotionType::Kinematic: return "Kinematic";
		case RagdollMotionType::AnimationDriven: return "Animation Driven";
		}
		return "Unknown";
	}

	const char* ConstraintKindName(RagdollConstraintKind kind) {
		switch(kind) {
		case RagdollConstraintKind::Fixed: return "Fixed";
		case RagdollConstraintKind::Hinge: return "Hinge";
		case RagdollConstraintKind::Cone: return "Cone";
		case RagdollConstraintKind::SwingTwist: return "Swing / Twist";
		case RagdollConstraintKind::SixDof: return "Six DoF";
		}
		return "Unknown";
	}

	const char* BoundaryPolicyName(RagdollBoundaryPolicy policy) {
		switch(policy) {
		case RagdollBoundaryPolicy::MatchAnimation: return "Match Animation";
		case RagdollBoundaryPolicy::KinematicAnchor: return "Kinematic Anchor";
		case RagdollBoundaryPolicy::MotorToAnimation: return "Motor to Animation";
		}
		return "Unknown";
	}

	const char* LimitModeName(RagdollLimitMode mode) {
		switch(mode) {
		case RagdollLimitMode::Free: return "Free";
		case RagdollLimitMode::Limited: return "Limited";
		case RagdollLimitMode::Locked: return "Locked";
		}
		return "Unknown";
	}

	const char* MotorModeName(RagdollMotorMode mode) {
		switch(mode) {
		case RagdollMotorMode::Disabled: return "Disabled";
		case RagdollMotorMode::Position: return "Position";
		case RagdollMotorMode::Velocity: return "Velocity";
		}
		return "Unknown";
	}

	bool EnumCombo(const char* label, int& value, const char* const* names, int count) {
		bool changed = false;
		if(ImGui::BeginCombo(label, value >= 0 && value < count ? names[value] : "Unknown")) {
			for(int index = 0; index < count; ++index) {
				if(ImGui::Selectable(names[index], value == index)) {
					value = index;
					changed = true;
				}
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	void DrawWirePacket(const char* id, std::span<const RagdollVisualizationLine> lines) {
		const ImVec2 size(ImGui::GetContentRegionAvail().x, 240.0F);
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(id, size);
		ImDrawList* draw = ImGui::GetWindowDrawList();
		draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
			IM_COL32(22, 25, 30, 255));
		if(lines.empty()) {
			draw->AddText(ImVec2(origin.x + 8.0F, origin.y + 8.0F),
				IM_COL32(170, 175, 185, 255), "No preview geometry");
			return;
		}
		float minimumX = std::numeric_limits<float>::max();
		float maximumX = std::numeric_limits<float>::lowest();
		float minimumY = std::numeric_limits<float>::max();
		float maximumY = std::numeric_limits<float>::lowest();
		for(const RagdollVisualizationLine& line : lines) {
			for(const AnimationFloat3 point : {line.start, line.end}) {
				minimumX = (std::min)(minimumX, point.x); maximumX = (std::max)(maximumX, point.x);
				minimumY = (std::min)(minimumY, point.y); maximumY = (std::max)(maximumY, point.y);
			}
		}
		const float rangeX = (std::max)(maximumX - minimumX, 0.1F);
		const float rangeY = (std::max)(maximumY - minimumY, 0.1F);
		const float scale = (std::min)((size.x - 20.0F) / rangeX, (size.y - 20.0F) / rangeY);
		const auto project = [&](AnimationFloat3 point) {
			return ImVec2(origin.x + 10.0F + (point.x - minimumX) * scale,
				origin.y + size.y - 10.0F - (point.y - minimumY) * scale);
		};
		for(const RagdollVisualizationLine& line : lines) {
			draw->AddLine(project(line.start), project(line.end), line.selected
				? IM_COL32(255, 195, 60, 255) : IM_COL32(95, 205, 235, 255),
				line.selected ? 2.5F : 1.5F);
		}
	}
#endif
}

namespace TerraEngine::Editor::Animation {
	bool RagdollAuthoringGenerationSnapshot::IsValid() const noexcept {
		return profile.IsValid() && skeleton.IsValid() && skeletonFingerprint != 0U
			&& referencePoseHash != 0U && persistedBytesHash != 0U;
	}

	bool RagdollRecoveryClipDependency::IsValid() const noexcept {
		return assetId.IsValid() && generation != 0U && skeletonFingerprint != 0U;
	}

	bool RagdollEditorModel::Begin(
		const RagdollProfileAsset& profile,
		const SkeletonAsset& skeleton,
		const RagdollAuthoringGenerationSnapshot& generation,
		RagdollCollisionLayerProvider collisionLayerProvider,
		RagdollRecoveryClipProvider recoveryClipProvider) {
		if(!generation.IsValid() || profile.assetId != generation.profile.assetId
			|| skeleton.assetId != generation.skeleton.assetId
			|| skeleton.runtimeGeneration != generation.skeleton.generation
			|| skeleton.compatibilityFingerprint != generation.skeletonFingerprint
			|| skeleton.referencePoseHash != generation.referencePoseHash
			|| skeleton.joints.empty() || skeleton.rootJointIndex >= skeleton.joints.size()) {
			return false;
		}
		std::vector<std::uint8_t> bytes;
		if(SerializeRagdollProfileAsset(profile, bytes) != AnimationStatus::Ok || bytes.empty()) {
			return false;
		}
		static_cast<void>(transaction_.Cancel());
		history_.Clear();
		publishedProfile_ = profile;
		candidate_ = profile;
		skeleton_ = skeleton;
		generation_ = generation;
		publishedRevision_ = {profile.assetId,
			(static_cast<std::uint64_t>(generation.profile.generation)),
			std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size())};
		collisionLayerProvider_ = std::move(collisionLayerProvider);
		recoveryClipProvider_ = std::move(recoveryClipProvider);
		loaded_ = true;
		editing_ = false;
		dirty_ = false;
		diagnostic_ = "Detached ragdoll document opened.";
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::RebindSkeleton(
		const SkeletonAsset& skeleton,
		const RagdollAuthoringGenerationSnapshot& generation) {
		if(!loaded_ || !generation.IsValid()
			|| generation.profile.assetId != publishedRevision_.assetId
			|| skeleton.assetId != generation.skeleton.assetId
			|| skeleton.runtimeGeneration != generation.skeleton.generation
			|| skeleton.compatibilityFingerprint != generation.skeletonFingerprint
			|| skeleton.referencePoseHash != generation.referencePoseHash
			|| skeleton.joints.empty() || skeleton.rootJointIndex >= skeleton.joints.size()
			|| !BeginMutation()) {
			diagnostic_ = "The selected skeleton could not be bound to this detached profile.";
			return false;
		}
		// Preserve the selected skeleton as one checked identity.  GenerateFromSkeleton
		// will replace the starter body data before the candidate can be published.
		skeleton_ = skeleton;
		generation_ = generation;
		candidate_.skeletonAssetId = skeleton.assetId;
		candidate_.skeletonFingerprint = skeleton.compatibilityFingerprint;
		candidate_.referencePoseHash = skeleton.referencePoseHash;
		dirty_ = true;
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		diagnostic_ = "Checked skeleton bound to the detached candidate; generate bodies before saving.";
		return true;
	}

	bool RagdollEditorModel::BeginMutation() noexcept {
		if(!loaded_) return false;
		if(editing_) return true;
		const AnimationAuthoringStatus status = transaction_.Begin(
			publishedRevision_, publishedRevision_.assetId, publishedRevision_.generation);
		if(status != AnimationAuthoringStatus::Ok) {
			diagnostic_ = "The detached ragdoll transaction could not begin.";
			return false;
		}
		candidate_ = publishedProfile_;
		editing_ = true;
		diagnostic_ = "Detached ragdoll edit started.";
		return true;
	}

	AnimationStatus RagdollEditorModel::GenerateFromSkeleton() {
		if(!BeginMutation()) return AnimationStatus::NotReady;
		RagdollProfileAsset generated = candidate_;
		generated.skeletonAssetId = skeleton_.assetId;
		generated.skeletonFingerprint = skeleton_.compatibilityFingerprint;
		generated.referencePoseHash = skeleton_.referencePoseHash;
		generated.physicsProviderFingerprint = 0x4a4f4c54U;
		generated.coveragePolicy = RagdollCoveragePolicy::AllDeformingJoints;
		generated.requiredBodyJointStableIds.clear();
		generated.bodies.clear();
		generated.constraints.clear();
		generated.blends.clear();
		generated.recovery.clear();
		generated.expectedTotalMassKilograms = 70.0F;
		generated.expectedMassToleranceFraction = 0.01F;
		generated.minimumBodyMassFraction = 0.0001F;
		generated.maximumBodyMassFraction = 1.0F;
		std::uint32_t generatedLayer = 0U;
		std::uint32_t generatedMask = 1U;
		try {
			const std::vector<RagdollCollisionLayerRule> layers = collisionLayerProvider_
				? collisionLayerProvider_() : std::vector<RagdollCollisionLayerRule>{};
			std::uint32_t enabledMask = 0U;
			for(const RagdollCollisionLayerRule& layer : layers) {
				if(layer.enabled && layer.layer < 32U) enabledMask |= 1U << layer.layer;
			}
			const auto selected = std::find_if(layers.begin(), layers.end(),
				[](const RagdollCollisionLayerRule& layer) {
					return layer.enabled && layer.layer < 32U;
				});
			if(selected != layers.end()) {
				generatedLayer = selected->layer;
				generatedMask = selected->allowedMask & enabledMask;
				if(generatedMask == 0U) generatedMask = 1U << generatedLayer;
			}
		} catch(...) {
			// Validation below reports the unavailable project policy without allowing
			// the provider exception to escape the editor boundary.
		}
		generated.allowedCollisionLayerMask = generatedMask;
		generated.allowDisconnectedBodyIslands = false;
		generated.requireSymmetricCollisionFilters = true;

		std::unordered_map<std::uint64_t, std::uint64_t> bodyByJoint;
		std::unordered_map<std::uint64_t, std::size_t> bodyIndexByJoint;
		const std::vector<AnimationTransform> modelBindPoses = BuildModelBindPoses(skeleton_);
		const float bodyMass = generated.expectedTotalMassKilograms
			/ static_cast<float>(skeleton_.joints.size());
		for(std::size_t jointIndex = 0U; jointIndex < skeleton_.joints.size(); ++jointIndex) {
			const SkeletonJoint& joint = skeleton_.joints[jointIndex];
			RagdollBodyRecord body;
			body.bodyId = joint.stableId;
			body.stableJointId = joint.stableId;
			body.massKilograms = bodyMass;
			body.motionType = RagdollMotionType::Dynamic;
			body.collisionLayer = generatedLayer;
			body.collisionMask = generatedMask;
			RagdollColliderRecord collider;
			collider.colliderId = StablePairId(joint.stableId, 0xc0111de5U);
			collider.shape = RagdollColliderShape::Capsule;
			collider.dimensions = {0.08F,
				std::max(0.05F, Length(joint.localBindTransform.translation) * 0.5F), 0.08F};
			body.colliders.push_back(collider);
			generated.bodies.push_back(body);
			bodyByJoint[joint.stableId] = body.bodyId;
			bodyIndexByJoint[joint.stableId] = generated.bodies.size() - 1U;
			if(joint.deformsMesh) generated.requiredBodyJointStableIds.push_back(joint.stableId);
			generated.blends.push_back({joint.stableId, 1.0F,
				RagdollBoundaryPolicy::MatchAnimation, 0.1F, 0.25F, 1.0F, false});
		}
		generated.rootBodyId = bodyByJoint[skeleton_.joints[skeleton_.rootJointIndex].stableId];
		for(std::size_t jointIndex = 0U; jointIndex < skeleton_.joints.size(); ++jointIndex) {
			const SkeletonJoint& joint = skeleton_.joints[jointIndex];
			if(joint.parentIndex < 0
				|| static_cast<std::size_t>(joint.parentIndex) >= skeleton_.joints.size()) continue;
			const std::size_t parentIndex = static_cast<std::size_t>(joint.parentIndex);
			const SkeletonJoint& parent = skeleton_.joints[parentIndex];
			const RagdollBodyRecord& parentBody = generated.bodies[bodyIndexByJoint[parent.stableId]];
			const RagdollBodyRecord& childBody = generated.bodies[bodyIndexByJoint[joint.stableId]];
			const AnimationTransform parentBodyPose = BodyPose(modelBindPoses[parentIndex], parentBody);
			const AnimationTransform childBodyPose = BodyPose(modelBindPoses[jointIndex], childBody);
			const AnimationTransform& anchorPose = modelBindPoses[jointIndex];
			const AnimationFloat3 worldPrimary = Rotate(anchorPose.rotation, {1.0F, 0.0F, 0.0F});
			const AnimationFloat3 worldSecondary = Rotate(anchorPose.rotation, {0.0F, 1.0F, 0.0F});
			RagdollConstraintRecord constraint;
			constraint.constraintId = StablePairId(parent.stableId, joint.stableId);
			constraint.parentBodyId = bodyByJoint[parent.stableId];
			constraint.childBodyId = bodyByJoint[joint.stableId];
			constraint.kind = RagdollConstraintKind::SwingTwist;
			// Both local frames describe one common bind-pose world anchor. A physics
			// provider can therefore activate the separate bodies without correcting an
			// artificial frame mismatch on the first solver step.
			constraint.parentFrame.localPosition = InverseRotate(parentBodyPose.rotation,
				Subtract(anchorPose.translation, parentBodyPose.translation));
			constraint.childFrame.localPosition = InverseRotate(childBodyPose.rotation,
				Subtract(anchorPose.translation, childBodyPose.translation));
			const AnimationQuaternion inverseParent{-parentBodyPose.rotation.x,
				-parentBodyPose.rotation.y, -parentBodyPose.rotation.z, parentBodyPose.rotation.w};
			const AnimationQuaternion inverseChild{-childBodyPose.rotation.x,
				-childBodyPose.rotation.y, -childBodyPose.rotation.z, childBodyPose.rotation.w};
			constraint.parentFrame.localOrientation = Multiply(inverseParent, anchorPose.rotation);
			constraint.childFrame.localOrientation = Multiply(inverseChild, anchorPose.rotation);
			constraint.parentFrame.primaryAxis = InverseRotate(parentBodyPose.rotation, worldPrimary);
			constraint.parentFrame.secondaryAxis = InverseRotate(parentBodyPose.rotation, worldSecondary);
			constraint.childFrame.primaryAxis = InverseRotate(childBodyPose.rotation, worldPrimary);
			constraint.childFrame.secondaryAxis = InverseRotate(childBodyPose.rotation, worldSecondary);
			for(std::size_t axis = static_cast<std::size_t>(RagdollConstraintAxis::AngularX);
				axis < constraint.limits.size(); ++axis) {
				constraint.limits[axis].mode = RagdollLimitMode::Limited;
				constraint.limits[axis].minimum = -0.75F;
				constraint.limits[axis].maximum = 0.75F;
			}
			generated.constraints.push_back(constraint);
		}
		candidate_ = std::move(generated);
		dirty_ = true;
		RefreshDerivedPreview();
		return Validate();
	}

	bool RagdollEditorModel::ReplaceBody(const RagdollBodyRecord& body) {
		if(!BeginMutation()) return false;
		const auto iterator = std::find_if(candidate_.bodies.begin(), candidate_.bodies.end(),
			[&body](const RagdollBodyRecord& current) { return current.bodyId == body.bodyId; });
		if(iterator == candidate_.bodies.end()) return false;
		*iterator = body;
		dirty_ = true;
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::ReplaceCollider(
		std::uint64_t bodyId,
		const RagdollColliderRecord& collider) {
		if(!BeginMutation()) return false;
		const auto body = std::find_if(candidate_.bodies.begin(), candidate_.bodies.end(),
			[bodyId](const RagdollBodyRecord& current) { return current.bodyId == bodyId; });
		if(body == candidate_.bodies.end() || collider.colliderId == 0U) return false;
		const auto existing = std::find_if(body->colliders.begin(), body->colliders.end(),
			[&collider](const RagdollColliderRecord& current) {
				return current.colliderId == collider.colliderId;
			});
		if(existing == body->colliders.end()) body->colliders.push_back(collider);
		else *existing = collider;
		dirty_ = true;
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::ReplaceConstraint(const RagdollConstraintRecord& constraint) {
		if(!BeginMutation()) return false;
		const auto existing = std::find_if(candidate_.constraints.begin(), candidate_.constraints.end(),
			[&constraint](const RagdollConstraintRecord& current) {
				return current.constraintId == constraint.constraintId;
			});
		if(existing == candidate_.constraints.end()) candidate_.constraints.push_back(constraint);
		else *existing = constraint;
		dirty_ = true;
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::ReplaceBlend(const RagdollBlendRecord& blend) {
		if(!BeginMutation()) return false;
		const auto existing = std::find_if(candidate_.blends.begin(), candidate_.blends.end(),
			[&blend](const RagdollBlendRecord& current) {
				return current.stableJointId == blend.stableJointId;
			});
		if(existing == candidate_.blends.end()) candidate_.blends.push_back(blend);
		else *existing = blend;
		dirty_ = true;
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::ReplaceRecovery(const RagdollRecoveryRecord& recovery) {
		if(recovery.candidateId == 0U || !BeginMutation()) return false;
		const auto existing = std::find_if(candidate_.recovery.begin(), candidate_.recovery.end(),
			[&recovery](const RagdollRecoveryRecord& current) {
				return current.candidateId == recovery.candidateId;
			});
		if(existing == candidate_.recovery.end()) candidate_.recovery.push_back(recovery);
		else *existing = recovery;
		dirty_ = true;
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::SelectRecoveryClip(
		std::uint64_t candidateId,
		AnimationGuid clipAssetId) {
		if(candidateId == 0U || !clipAssetId.IsValid() || !recoveryClipProvider_) return false;
		std::optional<RagdollRecoveryClipDependency> dependency;
		try {
			dependency = recoveryClipProvider_(clipAssetId);
		} catch(...) {
			return false;
		}
		if(!dependency || !dependency->IsValid()
			|| dependency->skeletonFingerprint != skeleton_.compatibilityFingerprint
			|| !BeginMutation()) return false;
		const auto recovery = std::find_if(candidate_.recovery.begin(), candidate_.recovery.end(),
			[candidateId](const RagdollRecoveryRecord& current) {
				return current.candidateId == candidateId;
			});
		if(recovery == candidate_.recovery.end()) return false;
		recovery->clipAssetId = dependency->assetId;
		recovery->clipGeneration = dependency->generation;
		recovery->clipSkeletonFingerprint = dependency->skeletonFingerprint;
		recovery->requiredReferencePoseHash = skeleton_.referencePoseHash;
		dirty_ = true;
		static_cast<void>(Validate());
		return true;
	}

	bool RagdollEditorModel::RemoveRecovery(std::uint64_t candidateId) {
		if(!BeginMutation()) return false;
		const auto end = std::remove_if(candidate_.recovery.begin(), candidate_.recovery.end(),
			[candidateId](const RagdollRecoveryRecord& recovery) {
				return recovery.candidateId == candidateId;
			});
		if(end == candidate_.recovery.end()) return false;
		candidate_.recovery.erase(end, candidate_.recovery.end());
		dirty_ = true;
		static_cast<void>(Validate());
		return true;
	}

	void RagdollEditorModel::SetCollisionVisualizationEnabled(bool enabled) noexcept {
		collisionVisualizationEnabled_ = enabled;
	}

	void RagdollEditorModel::SelectCollider(std::uint64_t colliderId) {
		selectedColliderId_ = colliderId;
		RefreshDerivedPreview();
	}

	void RagdollEditorModel::SetActivationPreview(float normalizedTime, bool activating) {
		activationPreview_.normalizedTime = std::clamp(normalizedTime, 0.0F, 1.0F);
		activationPreview_.activating = activating;
		RefreshDerivedPreview();
	}

	RagdollRecoveryTestResult RagdollEditorModel::TestRecovery(
		std::uint64_t candidateId,
		const RagdollRecoveryTestInput& input) const {
		RagdollRecoveryTestResult result;
		const auto recovery = std::find_if(candidate_.recovery.begin(), candidate_.recovery.end(),
			[candidateId](const RagdollRecoveryRecord& candidate) {
				return candidate.candidateId == candidateId;
			});
		if(recovery == candidate_.recovery.end()) {
			result.failures.push_back("Recovery candidate does not exist.");
			return result;
		}
		if(!std::isfinite(input.uprightDot) || !std::isfinite(input.poseErrorMeters)
			|| !std::isfinite(input.linearSpeed) || !std::isfinite(input.angularSpeed)
			|| !std::isfinite(input.availableClearanceRadiusMeters)
			|| !std::isfinite(input.availableClearanceHeightMeters)) {
			result.failures.push_back("Recovery test inputs must all be finite.");
			return result;
		}
		if(recovery->posture != RagdollRecoveryPosture::Any
			&& recovery->posture != input.posture) result.failures.push_back("Posture does not match.");
		if((input.activeContactMask & recovery->requiredContactMask)
			!= recovery->requiredContactMask) result.failures.push_back("Required contacts are missing.");
		if(input.uprightDot < recovery->minimumUprightDot) result.failures.push_back("Upright threshold is not met.");
		if(input.poseErrorMeters > recovery->maximumPoseErrorMeters) result.failures.push_back("Pose error is too high.");
		if(input.linearSpeed > recovery->maximumLinearSpeed) result.failures.push_back("Linear speed is too high.");
		if(input.angularSpeed > recovery->maximumAngularSpeed) result.failures.push_back("Angular speed is too high.");
		if(input.availableClearanceRadiusMeters < recovery->clearanceRadiusMeters
			|| input.availableClearanceHeightMeters < recovery->clearanceHeightMeters) {
			result.failures.push_back("Recovery clearance is insufficient.");
		}
		result.eligible = result.failures.empty();
		return result;
	}

	AnimationStatus RagdollEditorModel::Validate() {
		if(!loaded_) return AnimationStatus::NotReady;
		std::vector<RagdollCollisionLayerRule> layers;
		std::vector<RagdollRecoveryClipDependency> dependencies;
		try {
			if(collisionLayerProvider_) layers = collisionLayerProvider_();
			dependencies = ResolveRecoveryDependencies(candidate_, recoveryClipProvider_);
		} catch(...) {
			dependencyDiagnostics_ = {"Project validation providers were unavailable."};
			validation_ = {};
			validation_.status = AnimationStatus::NotReady;
			return validation_.status;
		}
		RagdollProfileValidationOptions options;
		options.collisionLayers = layers;
		validation_ = ValidateRagdollProfile(candidate_, skeleton_, options);
		const AnimationStatus dependencyStatus = ValidateRecoveryDependencies(
			candidate_, skeleton_, dependencies, dependencyDiagnostics_);
		return validation_.status != AnimationStatus::Ok ? validation_.status : dependencyStatus;
	}

	void RagdollEditorModel::RefreshDerivedPreview() {
		collisionVisualization_.clear();
		collisionGeometry_.clear();
		const std::vector<AnimationTransform> modelBindPoses = BuildModelBindPoses(skeleton_);
		std::unordered_map<std::uint64_t, std::size_t> jointIndices;
		for(std::size_t index = 0U; index < skeleton_.joints.size(); ++index) {
			jointIndices[skeleton_.joints[index].stableId] = index;
		}
		for(const RagdollBodyRecord& body : candidate_.bodies) {
			const auto joint = jointIndices.find(body.stableJointId);
			if(joint == jointIndices.end()) continue;
			const AnimationTransform bodyPose = BodyPose(modelBindPoses[joint->second], body);
			for(const RagdollColliderRecord& collider : body.colliders) {
				const bool selected = collider.colliderId == selectedColliderId_;
				const AnimationTransform colliderPose = ColliderPose(bodyPose, collider);
				collisionVisualization_.push_back({body.bodyId, collider.colliderId,
					collider.shape, bodyPose, colliderPose, collider.dimensions,
					collider.isSensor, selected});
				AddColliderGeometry(collisionGeometry_, collider, colliderPose, body.bodyId, selected);
			}
		}
		std::sort(collisionVisualization_.begin(), collisionVisualization_.end(),
			[](const RagdollCollisionVisualizationEntry& left,
				const RagdollCollisionVisualizationEntry& right) {
				return std::tie(left.bodyId, left.colliderId)
					< std::tie(right.bodyId, right.colliderId);
			});
		activationPreview_.joints.clear();
		activationPreview_.poseLines.clear();
		std::unordered_map<std::uint64_t, std::uint64_t> bodyIdsByJoint;
		std::unordered_map<std::uint64_t, AnimationTransform> bodyPosesByJoint;
		for(const RagdollBodyRecord& body : candidate_.bodies) {
			const auto joint = jointIndices.find(body.stableJointId);
			if(joint == jointIndices.end()) continue;
			bodyIdsByJoint[body.stableJointId] = body.bodyId;
			bodyPosesByJoint[body.stableJointId] = BodyPose(modelBindPoses[joint->second], body);
		}
		for(const RagdollBlendRecord& blend : candidate_.blends) {
			const float duration = activationPreview_.activating
				? blend.activationBlendSeconds : blend.recoveryBlendSeconds;
			const float elapsed = activationPreview_.normalizedTime * std::max(duration, 0.000001F);
			const float progress = std::clamp(elapsed / std::max(duration, 0.000001F), 0.0F, 1.0F);
			const auto joint = jointIndices.find(blend.stableJointId);
			if(joint == jointIndices.end()) continue;
			const auto bodyPose = bodyPosesByJoint.find(blend.stableJointId);
			activationPreview_.joints.push_back({blend.stableJointId,
				bodyIdsByJoint[blend.stableJointId],
				blend.defaultWeight * (activationPreview_.activating ? progress : 1.0F - progress),
				modelBindPoses[joint->second],
				bodyPose == bodyPosesByJoint.end() ? modelBindPoses[joint->second] : bodyPose->second});
		}
		std::sort(activationPreview_.joints.begin(), activationPreview_.joints.end(),
			[](const RagdollActivationPreviewJoint& left,
				const RagdollActivationPreviewJoint& right) {
				return left.stableJointId < right.stableJointId;
			});
		for(std::size_t jointIndex = 0U; jointIndex < skeleton_.joints.size(); ++jointIndex) {
			const SkeletonJoint& joint = skeleton_.joints[jointIndex];
			if(joint.parentIndex < 0 || static_cast<std::size_t>(joint.parentIndex) >= modelBindPoses.size()) {
				continue;
			}
			const auto bodyId = bodyIdsByJoint.find(joint.stableId);
			activationPreview_.poseLines.push_back({
				modelBindPoses[static_cast<std::size_t>(joint.parentIndex)].translation,
				modelBindPoses[jointIndex].translation,
				bodyId == bodyIdsByJoint.end() ? 0U : bodyId->second, 0U, false});
		}
	}

	AnimationAuthoringStatus RagdollEditorModel::PrepareCommit(
		RagdollAuthoringCompileRequest& outRequest) noexcept {
		if(!loaded_ || !editing_ || !dirty_) return AnimationAuthoringStatus::Rejected;
		if(Validate() != AnimationStatus::Ok || !validation_.IsValid()
			|| !dependencyDiagnostics_.empty()) {
			diagnostic_ = "The complete ragdoll candidate failed validation.";
			return AnimationAuthoringStatus::ValidationFailed;
		}
		std::vector<std::uint8_t> bytes;
		if(SerializeRagdollProfileAsset(candidate_, bytes) != AnimationStatus::Ok) {
			diagnostic_ = "The complete ragdoll candidate could not be serialized.";
			return AnimationAuthoringStatus::ValidationFailed;
		}
		const std::string candidateBytes(
			reinterpret_cast<const char*>(bytes.data()), bytes.size());
		if(candidateBytes == publishedRevision_.bytes) {
			static_cast<void>(transaction_.Cancel());
			candidate_ = publishedProfile_;
			editing_ = false;
			dirty_ = false;
			diagnostic_ = "The grouped edit has no durable changes to publish.";
			return AnimationAuthoringStatus::Rejected;
		}
		AnimationAuthoringStatus status = transaction_.Replace(
			candidateBytes);
		if(status != AnimationAuthoringStatus::Ok) return status;
		status = transaction_.PrepareCompile(outRequest.authoring);
		if(status != AnimationAuthoringStatus::Ok) return status;
		try {
			outRequest.skeleton = skeleton_;
			outRequest.sourceGeneration = generation_;
			outRequest.collisionLayers = collisionLayerProvider_
				? collisionLayerProvider_() : std::vector<RagdollCollisionLayerRule>{};
			outRequest.recoveryDependencies = ResolveRecoveryDependencies(
				candidate_, recoveryClipProvider_);
		} catch(...) {
			static_cast<void>(transaction_.Cancel());
			candidate_ = publishedProfile_;
			editing_ = false;
			dirty_ = false;
			diagnostic_ = "Project validation providers failed while preparing copied work.";
			return AnimationAuthoringStatus::ValidationFailed;
		}
		diagnostic_ = "Ragdoll candidate copied for worker validation.";
		return AnimationAuthoringStatus::Ok;
	}

	AnimationAuthoringStatus RagdollEditorModel::Cancel() noexcept {
		if(!editing_) return AnimationAuthoringStatus::Rejected;
		const AnimationAuthoringStatus status = transaction_.Cancel();
		if(status == AnimationAuthoringStatus::Canceled) {
			candidate_ = publishedProfile_;
			editing_ = false;
			dirty_ = false;
			diagnostic_ = "Detached ragdoll edit canceled; published bytes were unchanged.";
			RefreshDerivedPreview();
			static_cast<void>(Validate());
		}
		return status;
	}

	AnimationAuthoringStatus RagdollEditorModel::AdoptPublishedRevision(
		AnimationAuthoringRevision revision,
		RagdollAuthoringGenerationSnapshot generation) noexcept {
		RagdollProfileAsset decoded;
		const auto bytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(revision.bytes.data()), revision.bytes.size());
		if(DeserializeAnimationAssetForAuthoring(
			bytes, decoded, DeserializeRagdollProfileAsset) != AnimationStatus::Ok
			|| decoded.assetId != revision.assetId || !generation.IsValid()) {
			return AnimationAuthoringStatus::ValidationFailed;
		}
		decoded.runtimeGeneration = CheckedGeneration(revision.generation);
		publishedRevision_ = std::move(revision);
		publishedProfile_ = decoded;
		candidate_ = std::move(decoded);
		generation_ = generation;
		editing_ = false;
		dirty_ = false;
		RefreshDerivedPreview();
		static_cast<void>(Validate());
		return AnimationAuthoringStatus::Ok;
	}

	RagdollAuthoringCompileProduct RagdollAuthoringService::CompileCandidate(
		const RagdollAuthoringCompileRequest& request) noexcept {
		RagdollAuthoringCompileProduct product;
		product.authoring.assetId = request.authoring.assetId;
		product.authoring.expectedGeneration = request.authoring.expectedGeneration;
		product.authoring.requestId = request.authoring.requestId;
		const auto canceled = [&request]() {
			return request.authoring.cancellationRequested
				&& request.authoring.cancellationRequested->load(std::memory_order_acquire);
		};
		if(canceled()) {
			product.authoring.canceled = true;
			product.authoring.diagnostic = "Ragdoll validation was canceled before decoding.";
			product.validationStatus = AnimationStatus::Cancelled;
			return product;
		}
		if(!request.sourceGeneration.IsValid()
			|| request.sourceGeneration.profile.assetId != request.authoring.assetId
			|| request.sourceGeneration.skeleton.assetId != request.skeleton.assetId
			|| request.sourceGeneration.skeleton.generation != request.skeleton.runtimeGeneration
			|| request.sourceGeneration.skeletonFingerprint
				!= request.skeleton.compatibilityFingerprint
			|| request.sourceGeneration.referencePoseHash != request.skeleton.referencePoseHash) {
			product.validationStatus = AnimationStatus::StaleHandle;
			product.authoring.diagnostic = "Copied ragdoll validation context is stale.";
			return product;
		}
		RagdollProfileAsset candidate;
		const auto bytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(request.authoring.candidateBytes.data()),
			request.authoring.candidateBytes.size());
		const AnimationStatus decoded = DeserializeRagdollProfileAsset(
			bytes, request.skeleton.compatibilityFingerprint, candidate);
		if(decoded != AnimationStatus::Ok || candidate.assetId != request.authoring.assetId) {
			product.validationStatus = decoded == AnimationStatus::Ok
				? AnimationStatus::IncompatibleAsset : decoded;
			product.authoring.diagnostic = "Ragdoll candidate failed checked round-trip decoding.";
			return product;
		}
		if(canceled()) {
			product.authoring.canceled = true;
			product.validationStatus = AnimationStatus::Cancelled;
			product.authoring.diagnostic = "Ragdoll validation was canceled before policy checks.";
			return product;
		}
		RagdollProfileValidationOptions options;
		options.collisionLayers = request.collisionLayers;
		product.validation = ValidateRagdollProfile(candidate, request.skeleton, options);
		product.validationStatus = product.validation.status;
		if(product.validationStatus == AnimationStatus::Ok) {
			product.validationStatus = ValidateRecoveryDependencies(candidate, request.skeleton,
				request.recoveryDependencies, product.dependencyDiagnostics);
		}
		if(product.validationStatus != AnimationStatus::Ok) {
			product.authoring.diagnostic = "Copied ragdoll validation rejected the complete candidate.";
			return product;
		}
		if(canceled()) {
			product.authoring.canceled = true;
			product.validationStatus = AnimationStatus::Cancelled;
			product.authoring.diagnostic = "Ragdoll validation was canceled before publication handoff.";
			return product;
		}
		product.authoring.validatedBytes = request.authoring.candidateBytes;
		product.authoring.succeeded = true;
		product.authoring.diagnostic = "Serializer, decoder, skeleton, layer, and clip checks accepted the candidate.";
		return product;
	}

	bool RagdollAuthoringService::PublishRevision(
		const std::filesystem::path& destination,
		RagdollEditorModel& model,
		const SnapshotProvider& currentSnapshot,
		const AnimationAuthoringRevision& expectedCurrent,
		const AnimationAuthoringRevision& replacement,
		RagdollAuthoringGenerationSnapshot& outGeneration,
		std::string& outDiagnostic) noexcept {
		if(destination.extension() != ".teragdoll" || !currentSnapshot
			|| !expectedCurrent.assetId.IsValid()
			|| replacement.assetId != expectedCurrent.assetId
			|| replacement.generation != expectedCurrent.generation + 1U
			|| replacement.bytes.empty()) {
			outDiagnostic = "Ragdoll publication received an invalid complete revision.";
			return false;
		}
		const std::shared_ptr<std::mutex> destinationMutex = PublicationMutexFor(destination);
		// The snapshot comparison, disk-byte comparison, final dependency validation,
		// and replacement share this boundary. Two publishers from one old revision
		// therefore cannot both pass their stale checks and overwrite each other.
		std::scoped_lock publicationLock(*destinationMutex);
		const std::optional<RagdollAuthoringGenerationSnapshot> current = currentSnapshot();
		const std::vector<std::uint8_t> persisted = ReadBytes(destination);
		if(!current || *current != model.generation_ || persisted.empty()
			|| HashBytes(persisted) != model.generation_.persistedBytesHash) {
			outDiagnostic = "The profile or skeleton changed after editing began.";
			return false;
		}
		RagdollProfileAsset candidate;
		const auto replacementBytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(replacement.bytes.data()), replacement.bytes.size());
		if(DeserializeRagdollProfileAsset(replacementBytes,
			model.skeleton_.compatibilityFingerprint, candidate) != AnimationStatus::Ok
			|| candidate.assetId != expectedCurrent.assetId
			|| candidate.skeletonAssetId != model.skeleton_.assetId
			|| candidate.skeletonFingerprint != current->skeletonFingerprint
			|| candidate.referencePoseHash != current->referencePoseHash) {
			outDiagnostic = "The replacement failed final profile and skeleton identity checks.";
			return false;
		}
		std::vector<RagdollCollisionLayerRule> layers;
		std::vector<RagdollRecoveryClipDependency> dependencies;
		try {
			if(model.collisionLayerProvider_) layers = model.collisionLayerProvider_();
			dependencies = ResolveRecoveryDependencies(candidate, model.recoveryClipProvider_);
		} catch(...) {
			outDiagnostic = "Project validation providers failed at the publication boundary.";
			return false;
		}
		RagdollProfileValidationOptions options;
		options.collisionLayers = layers;
		const RagdollProfileValidationReport validation =
			ValidateRagdollProfile(candidate, model.skeleton_, options);
		std::vector<std::string> dependencyDiagnostics;
		if(!validation.IsValid() || ValidateRecoveryDependencies(candidate, model.skeleton_,
			dependencies, dependencyDiagnostics) != AnimationStatus::Ok) {
			outDiagnostic = "Final project-layer or recovery dependency validation rejected publication.";
			return false;
		}
		const std::filesystem::path staged = destination.parent_path()
			/ (destination.filename().string() + ".ragdoll-"
				+ std::to_string(g_RagdollStageSequence.fetch_add(1U,
					std::memory_order_relaxed)) + ".tmp");
		std::error_code cleanupError;
		std::filesystem::remove(staged, cleanupError);
		if(!WriteStagedBytes(staged, replacementBytes, outDiagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			return false;
		}
		if(!ReplaceFileAtomically(staged, destination, outDiagnostic)) {
			std::filesystem::remove(staged, cleanupError);
			return false;
		}
		outGeneration = model.generation_;
		outGeneration.persistedBytesHash = HashBytes(replacementBytes);
		outGeneration.profile.generation = GenerationFromHash(outGeneration.persistedBytesHash);
		outDiagnostic = "Validated ragdoll revision published atomically.";
		return true;
	}

	RagdollAuthoringPublishResult RagdollAuthoringService::CompletePublish(
		const std::filesystem::path& destination,
		RagdollEditorModel& model,
		const RagdollAuthoringCompileProduct& product,
		const SnapshotProvider& currentSnapshot) {
		RagdollAuthoringPublishResult result;
		const AnimationAuthoringRevision before = model.publishedRevision_;
		AnimationAuthoringRevision published;
		RagdollAuthoringGenerationSnapshot generation;
		std::string diagnostic;
		const AnimationAuthoringStatus status = model.transaction_.CompleteCompile(
			product.authoring, model.publishedRevision_,
			[&](const auto& expected, const auto& replacement, std::string& publisherDiagnostic) {
				return PublishRevision(destination, model, currentSnapshot, expected,
					replacement, generation, publisherDiagnostic);
			}, published, diagnostic);
		result.authoringStatus = status;
		if(status != AnimationAuthoringStatus::Ok) {
			if(status == AnimationAuthoringStatus::Canceled) {
				result.status = AnimationStatus::Cancelled;
			} else if(status == AnimationAuthoringStatus::Stale
				|| (status == AnimationAuthoringStatus::PublicationFailed
					&& diagnostic.find("changed") != std::string::npos)) {
				result.status = AnimationStatus::StaleHandle;
			} else {
				result.status = product.validationStatus == AnimationStatus::Ok
					? AnimationStatus::InternalError : product.validationStatus;
			}
			result.diagnostic = diagnostic.empty() ? product.authoring.diagnostic : diagnostic;
			model.diagnostic_ = result.diagnostic;
			return result;
		}
		const AnimationAuthoringStatus historyStatus = model.history_.Commit(before, published);
		if(historyStatus != AnimationAuthoringStatus::Ok) {
			result.authoringStatus = historyStatus;
			result.status = AnimationStatus::InternalError;
			result.diagnostic = "The file was published but its grouped history entry was rejected.";
			return result;
		}
		result.authoringStatus = model.AdoptPublishedRevision(std::move(published), generation);
		result.status = result.authoringStatus == AnimationAuthoringStatus::Ok
			? AnimationStatus::Ok : AnimationStatus::InternalError;
		result.published = result.status == AnimationStatus::Ok;
		result.diagnostic = result.published
			? "Validated ragdoll profile published atomically." : "Published bytes could not be adopted.";
		model.diagnostic_ = result.diagnostic;
		return result;
	}

	RagdollAuthoringPublishResult RagdollAuthoringService::PublishHistory(
			bool undo,
			const std::filesystem::path& destination,
			RagdollEditorModel& model,
			const RagdollAuthoringService::SnapshotProvider& currentSnapshot) {
			RagdollAuthoringPublishResult result;
			if(model.editing_ || model.dirty_) {
				result.authoringStatus = AnimationAuthoringStatus::Rejected;
				result.status = AnimationStatus::Rejected;
				result.diagnostic = "Save or cancel the current grouped edit before history publication.";
				return result;
			}
			AnimationAuthoringRevision published;
			RagdollAuthoringGenerationSnapshot generation;
			std::string diagnostic;
			const auto publisher = [&](const auto& expected, const auto& replacement,
				std::string& publisherDiagnostic) {
				return RagdollAuthoringService::PublishRevision(destination, model, currentSnapshot,
					expected, replacement, generation, publisherDiagnostic);
			};
			const AnimationAuthoringStatus status = undo
				? model.history_.Undo(model.publishedRevision_, publisher, published, diagnostic)
				: model.history_.Redo(model.publishedRevision_, publisher, published, diagnostic);
			result.authoringStatus = status;
			if(status != AnimationAuthoringStatus::Ok) {
				result.status = status == AnimationAuthoringStatus::Stale
					|| (status == AnimationAuthoringStatus::PublicationFailed
						&& diagnostic.find("changed") != std::string::npos)
					? AnimationStatus::StaleHandle : AnimationStatus::Rejected;
				result.diagnostic = diagnostic;
				model.diagnostic_ = result.diagnostic;
				return result;
			}
			result.authoringStatus = model.AdoptPublishedRevision(std::move(published), generation);
			result.status = result.authoringStatus == AnimationAuthoringStatus::Ok
				? AnimationStatus::Ok : AnimationStatus::InternalError;
			result.published = result.status == AnimationStatus::Ok;
			result.diagnostic = result.published
				? (undo ? "Undo published the complete prior ragdoll revision."
					: "Redo published the complete later ragdoll revision.")
				: "History bytes could not be adopted.";
			model.diagnostic_ = result.diagnostic;
			return result;
	}

	RagdollAuthoringPublishResult RagdollAuthoringService::Undo(
		const std::filesystem::path& destination,
		RagdollEditorModel& model,
		const SnapshotProvider& currentSnapshot) {
		return PublishHistory(true, destination, model, currentSnapshot);
	}

	RagdollAuthoringPublishResult RagdollAuthoringService::Redo(
		const std::filesystem::path& destination,
		RagdollEditorModel& model,
		const SnapshotProvider& currentSnapshot) {
		return PublishHistory(false, destination, model, currentSnapshot);
	}
}

#ifndef TERRAENGINE_EDITOR_TESTS
namespace {
	struct RagdollAsyncCompileState {
		std::mutex mutex;
		std::optional<TerraEngine::Editor::Animation::RagdollAuthoringCompileProduct> completed;
		std::atomic_bool running{false};
	};
}

struct RagdollEditorWindow::State {
	TerraEngine::Editor::Animation::CookedAnimationAssetSelection<
		TerraEngine::Animation::RagdollProfileAsset> selection;
	TerraEngine::Editor::Animation::RagdollEditorModel model;
	std::shared_ptr<RagdollAsyncCompileState> compile =
		std::make_shared<RagdollAsyncCompileState>();
	std::filesystem::path modelPath;
	std::uint64_t selectedBody = 0U;
	std::uint64_t selectedConstraint = 0U;
	std::uint64_t selectedRecovery = 0U;
	std::vector<TerraEngine::Editor::Animation::RagdollRecoveryClipDependency> recoveryClips;
	TerraEngine::Animation::AnimationCompatibilityFingerprint recoveryClipFingerprint = 0U;
	TerraEngine::Editor::Animation::RagdollRecoveryTestInput recoveryInput{};
	TerraEngine::Editor::Animation::RagdollRecoveryTestResult recoveryResult{};
	// GUID text accepts a copied Project Browser/Inspector identity while the
	// picker below offers the discoverable package-child skeleton names.
	std::array<char, 33U> skeletonGuidText{};
	std::vector<RagdollSkeletonBindingOption> skeletonChoices;
	float activationTime = 0.0F;
	bool activating = true;
	std::string message;
};

RagdollEditorWindow::RagdollEditorWindow(MainEditorSubsystem* owner)
	: EditorWindow("Ragdoll Editor", false, "Animation/Authoring"),
	  owner_(owner), state_(std::make_unique<State>()) {
}

RagdollEditorWindow::~RagdollEditorWindow() = default;

void RagdollEditorWindow::OnGui() {
	using namespace TerraEngine::Animation;
	using namespace TerraEngine::Editor::Animation;
	state_->selection.Synchronize(owner_, ".teragdoll", &DeserializeRagdollProfileAsset);
	const RagdollProfileAsset* selected = state_->selection.Get();
	if(!state_->selection.IsSelectedForWindow()) {
		ImGui::TextWrapped("Select a .teragdoll profile to generate and author bodies, colliders, constraints, motors, partial blends, and recovery tests.");
		return;
	}
	ImGui::TextWrapped("%s", state_->selection.Path().string().c_str());
	if(selected == nullptr) {
		ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
			state_->selection.Message().c_str());
		return;
	}
	RagdollEditorModel& model = state_->model;
	const auto bindSkeleton = [&](AnimationGuid skeletonId) {
		SkeletonAsset skeleton;
		std::string diagnostic;
		const auto snapshot = CaptureSnapshot(state_->selection.Path(), selected->assetId,
			&skeleton, &diagnostic, skeletonId);
		if(!snapshot.has_value()) {
			state_->message = diagnostic.empty()
				? "The requested skeleton is absent or incompatible." : diagnostic;
			return false;
		}
		// Keep this binding detached until the full generated profile validates. A
		// malformed GUID therefore cannot overwrite the starter or last-good file.
		RagdollProfileAsset boundProfile = *selected;
		boundProfile.skeletonAssetId = skeleton.assetId;
		boundProfile.skeletonFingerprint = skeleton.compatibilityFingerprint;
		boundProfile.referencePoseHash = skeleton.referencePoseHash;
		const bool opened = (!model.IsLoaded() || state_->modelPath != state_->selection.Path())
			? model.Begin(boundProfile, skeleton, *snapshot,
				[]() { return CurrentProjectCollisionLayers(); },
				[](AnimationGuid id) { return ResolveRecoveryClip(id); })
			: model.RebindSkeleton(skeleton, *snapshot);
		if(!opened) {
			state_->message = "The checked skeleton could not be attached to the detached candidate.";
			return false;
		}
		state_->modelPath = state_->selection.Path();
		std::snprintf(state_->skeletonGuidText.data(), state_->skeletonGuidText.size(), "%s",
			TerraEngine::ToHexString(skeleton.assetId.value).c_str());
		state_->recoveryClips.clear();
		state_->recoveryClipFingerprint = 0U;
		state_->message = "Skeleton bound in memory. Generate from current skeleton, then Validate & Save.";
		return true;
	};
	const auto drawSkeletonBinding = [&]() {
		ImGui::SeparatorText("Skeleton binding");
		ImGui::TextWrapped("Choose the skeleton that supplies the body map and reference pose. The GUID is checked against the cooked asset; binding is only written when the generated profile validates and saves.");
		ImGui::SetNextItemWidth(-1.0F);
		ImGui::InputText("Skeleton asset GUID", state_->skeletonGuidText.data(),
			state_->skeletonGuidText.size(), ImGuiInputTextFlags_CharsHexadecimal);
		if(ImGui::Button("Bind entered skeleton GUID")) {
			const auto parsed = TerraEngine::ParseUUID(state_->skeletonGuidText.data());
			if(!parsed.has_value()) state_->message = "Enter a 32-character skeleton GUID from the Project Browser or Inspector.";
			else static_cast<void>(bindSkeleton(AnimationGuid{*parsed}));
		}
		ImGui::SameLine();
		if(ImGui::Button("Refresh skeleton list")) {
			state_->skeletonChoices = DiscoverRagdollSkeletonBindings();
		}
		if(state_->skeletonChoices.empty()) {
			state_->skeletonChoices = DiscoverRagdollSkeletonBindings();
		}
		if(ImGui::BeginCombo("Imported/package skeleton", "Select discovered skeleton")) {
			for(const RagdollSkeletonBindingOption& option : state_->skeletonChoices) {
				const std::string label = option.displayName + "  ("
					+ TerraEngine::ToHexString(option.assetId.value) + ")";
				if(ImGui::Selectable(label.c_str(), false)) static_cast<void>(bindSkeleton(option.assetId));
			}
			ImGui::EndCombo();
		}
		if(state_->skeletonChoices.empty()) {
			ImGui::TextDisabled("No imported skeletons were discovered. Import the Fox model, then refresh this list.");
		}
	};
	if((!state_->model.IsLoaded() || state_->modelPath != state_->selection.Path())
		&& !state_->compile->running.load(std::memory_order_acquire)) {
		SkeletonAsset skeleton;
		std::string diagnostic;
		const auto snapshot = CaptureSnapshot(state_->selection.Path(), selected->assetId,
			&skeleton, &diagnostic);
		if(!snapshot.has_value()) {
			ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
				diagnostic.empty() ? "This starter profile needs a real skeleton binding." : diagnostic.c_str());
			drawSkeletonBinding();
			return;
		}
		if(!model.Begin(*selected, skeleton, *snapshot,
			[]() { return CurrentProjectCollisionLayers(); },
			[](AnimationGuid id) { return ResolveRecoveryClip(id); })) {
			ImGui::TextColored(ImVec4(1.0F, 0.45F, 0.35F, 1.0F), "%s",
				diagnostic.empty() ? "The current skeleton generation could not be captured."
					: diagnostic.c_str());
			return;
		}
		state_->modelPath = state_->selection.Path();
		state_->recoveryClips.clear();
		state_->recoveryClipFingerprint = 0U;
		state_->message = "Detached ragdoll candidate loaded.";
		std::snprintf(state_->skeletonGuidText.data(), state_->skeletonGuidText.size(), "%s",
			TerraEngine::ToHexString(skeleton.assetId.value).c_str());
	}
	drawSkeletonBinding();
	std::optional<RagdollAuthoringCompileProduct> completed;
	{
		std::scoped_lock lock(state_->compile->mutex);
		if(state_->compile->completed) {
			completed = std::move(state_->compile->completed);
			state_->compile->completed.reset();
		}
	}
	if(completed) {
		if(model.HasPendingEdit()) {
			const auto path = state_->selection.Path();
			const AnimationGuid profileId = model.Candidate().assetId;
			const AnimationGuid skeletonId = model.Skeleton().assetId;
			const RagdollAuthoringPublishResult result = RagdollAuthoringService::CompletePublish(
				path, model, *completed,
				// The disk starter can still contain its placeholder identity while a
				// valid bound candidate is being generated. Check the selected skeleton
				// at this boundary and replace the starter only after validation passes.
				[path, profileId, skeletonId]() {
					return CaptureSnapshot(path, profileId, nullptr, nullptr, skeletonId);
				});
			state_->message = result.diagnostic;
			if(result.published) AssetDatabase::Instance().ImportAssetWithProgress(path, true);
		}
		state_->compile->running.store(false, std::memory_order_release);
	}

	if(!state_->compile->running.load(std::memory_order_acquire)
		&& ImGui::Button("Generate from current skeleton")) {
		state_->message = std::string("Generation: ")
			+ std::string(ToString(model.GenerateFromSkeleton()));
	}
	ImGui::SameLine();
	const bool canSave = model.IsDirty() && model.Validation().IsValid()
		&& model.DependencyDiagnostics().empty() && !state_->selection.IsStale()
		&& !state_->compile->running.load(std::memory_order_acquire);
	ImGui::BeginDisabled(!canSave);
	if(ImGui::Button("Validate & Save")) {
		RagdollAuthoringCompileRequest request;
		if(model.PrepareCommit(request) == AnimationAuthoringStatus::Ok) {
			state_->compile->running.store(true, std::memory_order_release);
			const std::shared_ptr<RagdollAsyncCompileState> shared = state_->compile;
			auto work = [shared, request]() {
				RagdollAuthoringCompileProduct product =
					RagdollAuthoringService::CompileCandidate(request);
				std::scoped_lock lock(shared->mutex);
				shared->completed = std::move(product);
			};
			if(ThreadPool::instance().getThreadCount() == 0U) work();
			else ThreadPool::instance().enqueue(
				ThreadPool::instance().createTask(std::move(work)), 1U);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!model.HasPendingEdit());
	if(ImGui::Button("Cancel Edit")) {
		const AnimationAuthoringStatus status = model.Cancel();
		state_->message = status == AnimationAuthoringStatus::Canceled
			? model.Diagnostic() : "No ragdoll edit was available to cancel.";
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(model.HasPendingEdit()
		|| state_->compile->running.load(std::memory_order_acquire) || model.UndoDepth() == 0U);
	if(ImGui::Button("Undo")) {
		const auto path = state_->selection.Path();
		const AnimationGuid profileId = model.Candidate().assetId;
		const auto result = RagdollAuthoringService::Undo(path, model,
			[path, profileId]() { return CaptureSnapshot(path, profileId); });
		state_->message = result.diagnostic;
		if(result.published) AssetDatabase::Instance().ImportAssetWithProgress(path, true);
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(model.HasPendingEdit()
		|| state_->compile->running.load(std::memory_order_acquire) || model.RedoDepth() == 0U);
	if(ImGui::Button("Redo")) {
		const auto path = state_->selection.Path();
		const AnimationGuid profileId = model.Candidate().assetId;
		const auto result = RagdollAuthoringService::Redo(path, model,
			[path, profileId]() { return CaptureSnapshot(path, profileId); });
		state_->message = result.diagnostic;
		if(result.published) AssetDatabase::Instance().ImportAssetWithProgress(path, true);
	}
	ImGui::EndDisabled();
	if(state_->compile->running.load(std::memory_order_acquire)) {
		ImGui::TextDisabled("Validating a copied ragdoll packet on a worker...");
	}

	const RagdollProfileAsset& profile = model.Candidate();
	const RagdollProfileValidationReport& validation = model.Validation();
	const bool publishable = validation.IsValid() && model.DependencyDiagnostics().empty();
	const ImVec4 validationColor = publishable
		? ImVec4(0.35F, 0.85F, 0.45F, 1.0F) : ImVec4(1.0F, 0.45F, 0.35F, 1.0F);
	ImGui::TextColored(validationColor, "%s | %s | %s",
		publishable ? "Publishable" : "Invalid candidate",
		model.IsDirty() ? "unsaved" : "saved", state_->message.c_str());
	ImGui::Text("Bodies %zu  Constraints %zu  Mass %.3f kg  Skeleton generation %u",
		profile.bodies.size(), profile.constraints.size(), validation.totalMassKilograms,
		model.Generation().skeleton.generation);
	for(const RagdollProfileDiagnostic& diagnostic : validation.diagnostics) {
		ImGui::BulletText("[%u] %s — %s", static_cast<unsigned int>(diagnostic.code),
			diagnostic.message.data(), diagnostic.correctiveAction.data());
	}

	for(const std::string& dependencyDiagnostic : model.DependencyDiagnostics()) {
		ImGui::BulletText("Dependency: %s", dependencyDiagnostic.c_str());
	}

	const bool disableEditors = state_->compile->running.load(std::memory_order_acquire);
	if(disableEditors) ImGui::BeginDisabled();
	if(!ImGui::BeginTabBar("RagdollEditorTabs")) {
		if(disableEditors) ImGui::EndDisabled();
		return;
	}
	if(ImGui::BeginTabItem("Bodies / Collision")) {
		bool visualize = model.CollisionVisualizationEnabled();
		if(ImGui::Checkbox("Show authored collision visualization", &visualize)) {
			model.SetCollisionVisualizationEnabled(visualize);
		}
		ImGui::TextDisabled("Visualization is deterministic authoring data; no live physics body is created.");
		for(const RagdollBodyRecord& bodyValue : profile.bodies) {
			ImGui::PushID(static_cast<int>(bodyValue.bodyId));
			if(ImGui::Selectable(("Body " + std::to_string(bodyValue.bodyId)).c_str(),
				state_->selectedBody == bodyValue.bodyId)) state_->selectedBody = bodyValue.bodyId;
			ImGui::PopID();
		}
		for(const RagdollBodyRecord& bodyValue : profile.bodies) {
			if(bodyValue.bodyId != state_->selectedBody) continue;
			RagdollBodyRecord body = bodyValue;
			bool changed = ImGui::DragFloat("Mass (kg)", &body.massKilograms, 0.1F, 0.001F, 1000.0F);
			int motion = static_cast<int>(body.motionType);
			const char* motions[] = {"Dynamic", "Kinematic", "Animation Driven"};
			if(EnumCombo("Motion", motion, motions, 3)) {
				body.motionType = static_cast<RagdollMotionType>(motion); changed = true;
			}
			changed = ImGui::InputScalar("Collision layer", ImGuiDataType_U32,
				&body.collisionLayer) || changed;
			changed = ImGui::InputScalar("Collision mask", ImGuiDataType_U32,
				&body.collisionMask, nullptr, nullptr, "%08X", ImGuiInputTextFlags_CharsHexadecimal) || changed;
			changed = ImGui::DragFloat("Linear damping", &body.linearDamping, 0.01F, 0.0F, 10.0F) || changed;
			changed = ImGui::DragFloat("Angular damping", &body.angularDamping, 0.01F, 0.0F, 10.0F) || changed;
			changed = ImGui::Checkbox("Allow sleep", &body.allowSleep) || changed;
			if(changed) model.ReplaceBody(body);
			for(const RagdollColliderRecord& colliderValue : bodyValue.colliders) {
				RagdollColliderRecord collider = colliderValue;
				ImGui::PushID(static_cast<int>(collider.colliderId));
				if(ImGui::TreeNode("Collider", "%s collider #%llu", ShapeName(collider.shape),
					static_cast<unsigned long long>(collider.colliderId))) {
					model.SelectCollider(collider.colliderId);
					int shape = static_cast<int>(collider.shape);
					const char* shapes[] = {"Sphere", "Capsule", "Box", "Convex Hull"};
					bool colliderChanged = EnumCombo("Shape", shape, shapes, 4);
					if(colliderChanged) collider.shape = static_cast<RagdollColliderShape>(shape);
					colliderChanged = ImGui::DragFloat3("Center", &collider.localCenter.x, 0.01F) || colliderChanged;
					colliderChanged = ImGui::DragFloat3("Dimensions", &collider.dimensions.x, 0.01F, 0.001F, 10.0F) || colliderChanged;
					colliderChanged = ImGui::DragFloat("Friction", &collider.friction, 0.01F, 0.0F, 1.0F) || colliderChanged;
					colliderChanged = ImGui::DragFloat("Restitution", &collider.restitution, 0.01F, 0.0F, 1.0F) || colliderChanged;
					colliderChanged = ImGui::Checkbox("Sensor", &collider.isSensor) || colliderChanged;
					if(colliderChanged) model.ReplaceCollider(body.bodyId, collider);
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
		}
		if(visualize) {
			ImGui::SeparatorText("World-space collision geometry");
			DrawWirePacket("RagdollCollisionGeometry", model.CollisionGeometry());
			for(const auto& entry : model.CollisionVisualization()) {
				ImGui::BulletText("%s body=%llu collider=%llu center=(%.2f %.2f %.2f) size=(%.2f %.2f %.2f)%s",
					ShapeName(entry.shape), static_cast<unsigned long long>(entry.bodyId),
					static_cast<unsigned long long>(entry.colliderId), entry.colliderPose.translation.x,
					entry.colliderPose.translation.y, entry.colliderPose.translation.z,
					entry.dimensions.x, entry.dimensions.y, entry.dimensions.z,
					entry.selected ? " [selected]" : "");
			}
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Constraints / Motors")) {
		for(const RagdollConstraintRecord& constraint : profile.constraints) {
			if(ImGui::Selectable(("#" + std::to_string(constraint.constraintId) + " "
				+ ConstraintKindName(constraint.kind)).c_str(),
				state_->selectedConstraint == constraint.constraintId)) {
				state_->selectedConstraint = constraint.constraintId;
			}
		}
		for(const RagdollConstraintRecord& source : profile.constraints) {
			if(source.constraintId != state_->selectedConstraint) continue;
			RagdollConstraintRecord constraint = source;
			bool changed = ImGui::InputScalar("Parent body", ImGuiDataType_U64,
				&constraint.parentBodyId);
			changed = ImGui::InputScalar("Child body", ImGuiDataType_U64,
				&constraint.childBodyId) || changed;
			int kind = static_cast<int>(constraint.kind);
			const char* kinds[] = {"Fixed", "Hinge", "Cone", "Swing / Twist", "Six DoF"};
			if(EnumCombo("Kind", kind, kinds, 5)) {
				constraint.kind = static_cast<RagdollConstraintKind>(kind); changed = true;
			}
			for(std::size_t axis = 0U; axis < constraint.limits.size(); ++axis) {
				ImGui::PushID(static_cast<int>(axis));
				if(ImGui::TreeNode("Axis", "Axis %zu", axis)) {
					RagdollAxisLimit& limit = constraint.limits[axis];
					RagdollAxisMotor& motor = constraint.motors[axis];
					int limitMode = static_cast<int>(limit.mode);
					const char* limits[] = {"Free", "Limited", "Locked"};
					if(EnumCombo("Limit", limitMode, limits, 3)) {
						limit.mode = static_cast<RagdollLimitMode>(limitMode); changed = true;
					}
					changed = ImGui::DragFloatRange2("Minimum / maximum", &limit.minimum,
						&limit.maximum, 0.01F) || changed;
					int motorMode = static_cast<int>(motor.mode);
					const char* motors[] = {"Disabled", "Position", "Velocity"};
					if(EnumCombo("Motor", motorMode, motors, 3)) {
						motor.mode = static_cast<RagdollMotorMode>(motorMode); changed = true;
					}
					changed = ImGui::DragFloat("Target position", &motor.targetPosition, 0.01F) || changed;
					changed = ImGui::DragFloat("Target velocity", &motor.targetVelocity, 0.01F) || changed;
					changed = ImGui::DragFloat("Stiffness", &motor.stiffness, 0.1F, 0.0F) || changed;
					changed = ImGui::DragFloat("Damping", &motor.damping, 0.1F, 0.0F) || changed;
					changed = ImGui::DragFloat("Maximum force", &motor.maximumForce, 1.0F, 0.0F) || changed;
					ImGui::TextDisabled("Current: %s / %s", LimitModeName(limit.mode), MotorModeName(motor.mode));
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			changed = ImGui::DragFloat("Break force", &constraint.breakForce, 1.0F, 0.0F) || changed;
			changed = ImGui::DragFloat("Break torque", &constraint.breakTorque, 1.0F, 0.0F) || changed;
			if(changed) model.ReplaceConstraint(constraint);
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Activation / Partial Blend")) {
		bool previewChanged = false;
		if(ImGui::RadioButton("Activation", state_->activating)) {
			state_->activating = true;
			previewChanged = true;
		}
		ImGui::SameLine();
		if(ImGui::RadioButton("Recovery", !state_->activating)) {
			state_->activating = false;
			previewChanged = true;
		}
		if(ImGui::SliderFloat("Preview normalized time", &state_->activationTime, 0.0F, 1.0F)) {
			previewChanged = true;
		}
		if(previewChanged) model.SetActivationPreview(state_->activationTime, state_->activating);
		for(const RagdollBlendRecord& source : profile.blends) {
			RagdollBlendRecord blend = source;
			ImGui::PushID(static_cast<int>(blend.stableJointId));
			if(ImGui::TreeNode("Blend", "Joint %llu", static_cast<unsigned long long>(blend.stableJointId))) {
				bool changed = ImGui::SliderFloat("Physics weight", &blend.defaultWeight, 0.0F, 1.0F);
				changed = ImGui::DragFloat("Activation seconds", &blend.activationBlendSeconds, 0.01F, 0.0F, 10.0F) || changed;
				changed = ImGui::DragFloat("Recovery seconds", &blend.recoveryBlendSeconds, 0.01F, 0.0F, 10.0F) || changed;
				changed = ImGui::DragFloat("Motor strength", &blend.motorStrengthScale, 0.01F, 0.0F, 10.0F) || changed;
				changed = ImGui::Checkbox("Include descendants", &blend.includeDescendants) || changed;
				int boundary = static_cast<int>(blend.boundaryPolicy);
				const char* boundaries[] = {"Match Animation", "Kinematic Anchor", "Motor to Animation"};
				if(EnumCombo("Boundary", boundary, boundaries, 3)) {
					blend.boundaryPolicy = static_cast<RagdollBoundaryPolicy>(boundary); changed = true;
				}
				if(changed) model.ReplaceBlend(blend);
				ImGui::TextDisabled("%s", BoundaryPolicyName(blend.boundaryPolicy));
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
		ImGui::SeparatorText("Deterministic preview result");
		DrawWirePacket("RagdollActivationPose", model.ActivationPreview().poseLines);
		for(const RagdollActivationPreviewJoint& joint : model.ActivationPreview().joints) {
			ImGui::ProgressBar(joint.physicsWeight, ImVec2(-1.0F, 0.0F),
				("Joint " + std::to_string(joint.stableJointId)).c_str());
		}
		ImGui::EndTabItem();
	}

	if(ImGui::BeginTabItem("Recovery Testing")) {
		const bool refreshRecoveryClips = ImGui::Button("Refresh compatible clips");
		if(state_->recoveryClipFingerprint != profile.skeletonFingerprint
			|| refreshRecoveryClips) {
			state_->recoveryClips = DiscoverRecoveryClips(profile.skeletonFingerprint);
			state_->recoveryClipFingerprint = profile.skeletonFingerprint;
		}
		const std::vector<RagdollRecoveryClipDependency>& recoveryClips = state_->recoveryClips;
		ImGui::BeginDisabled(recoveryClips.empty());
		if(ImGui::Button("Add recovery candidate")) {
			RagdollRecoveryRecord recovery;
			recovery.candidateId = profile.recovery.empty() ? 1U
				: profile.recovery.back().candidateId + 1U;
			recovery.clipAssetId = recoveryClips.front().assetId;
			recovery.clipGeneration = recoveryClips.front().generation;
			recovery.clipSkeletonFingerprint = recoveryClips.front().skeletonFingerprint;
			recovery.requiredReferencePoseHash = profile.referencePoseHash;
			recovery.rootJointStableId = model.Skeleton().joints[model.Skeleton().rootJointIndex].stableId;
			model.ReplaceRecovery(recovery);
			state_->selectedRecovery = recovery.candidateId;
		}
		ImGui::EndDisabled();
		if(recoveryClips.empty()) {
			ImGui::TextDisabled("Import a compatible .teanim clip before adding recovery data.");
		}
		for(const RagdollRecoveryRecord& source : profile.recovery) {
			if(ImGui::Selectable(("Candidate " + std::to_string(source.candidateId)).c_str(),
				state_->selectedRecovery == source.candidateId)) state_->selectedRecovery = source.candidateId;
		}
		for(const RagdollRecoveryRecord& source : profile.recovery) {
			if(source.candidateId != state_->selectedRecovery) continue;
			RagdollRecoveryRecord recovery = source;
			bool changed = false;
			const auto currentClip = std::find_if(recoveryClips.begin(), recoveryClips.end(),
				[&recovery](const RagdollRecoveryClipDependency& dependency) {
					return dependency.assetId == recovery.clipAssetId;
				});
			const char* currentClipName = currentClip == recoveryClips.end()
				? "Missing or stale clip" : currentClip->displayName.c_str();
			if(ImGui::BeginCombo("Recovery clip", currentClipName)) {
				for(const RagdollRecoveryClipDependency& dependency : recoveryClips) {
					if(ImGui::Selectable(dependency.displayName.c_str(),
						dependency.assetId == recovery.clipAssetId)) {
						recovery.clipAssetId = dependency.assetId;
						recovery.clipGeneration = dependency.generation;
						recovery.clipSkeletonFingerprint = dependency.skeletonFingerprint;
						recovery.requiredReferencePoseHash = profile.referencePoseHash;
						changed = true;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::TextDisabled("Clip generation %u | fingerprint %llu",
				recovery.clipGeneration,
				static_cast<unsigned long long>(recovery.clipSkeletonFingerprint));
			changed = ImGui::InputScalar("Recovery root joint", ImGuiDataType_U64,
				&recovery.rootJointStableId) || changed;
			changed = ImGui::InputScalar("Required contact mask", ImGuiDataType_U64,
				&recovery.requiredContactMask, nullptr, nullptr, "%016llX",
				ImGuiInputTextFlags_CharsHexadecimal) || changed;
			int posture = static_cast<int>(recovery.posture);
			const char* postures[] = {"Any", "Face Up", "Face Down", "Left Side", "Right Side"};
			if(EnumCombo("Posture", posture, postures, 5)) {
				recovery.posture = static_cast<RagdollRecoveryPosture>(posture); changed = true;
			}
			changed = ImGui::SliderFloat("Minimum upright dot", &recovery.minimumUprightDot,
				-1.0F, 1.0F) || changed;
			changed = ImGui::DragFloat("Maximum pose error", &recovery.maximumPoseErrorMeters, 0.01F, 0.0F) || changed;
			changed = ImGui::DragFloat("Maximum linear speed", &recovery.maximumLinearSpeed, 0.01F, 0.0F) || changed;
			changed = ImGui::DragFloat("Maximum angular speed", &recovery.maximumAngularSpeed, 0.01F, 0.0F) || changed;
			changed = ImGui::DragFloat("Blend duration", &recovery.blendDurationSeconds, 0.01F, 0.001F) || changed;
			changed = ImGui::DragFloat("Clearance radius", &recovery.clearanceRadiusMeters, 0.01F, 0.001F) || changed;
			changed = ImGui::DragFloat("Clearance height", &recovery.clearanceHeightMeters, 0.01F, 0.001F) || changed;
			changed = ImGui::Checkbox("Allow moving support", &recovery.allowMovingSupport) || changed;
			if(changed) model.ReplaceRecovery(recovery);
			ImGui::SeparatorText("Test inputs");
			int observedPosture = static_cast<int>(state_->recoveryInput.posture);
			if(EnumCombo("Observed posture", observedPosture, postures, 5)) {
				state_->recoveryInput.posture =
					static_cast<RagdollRecoveryPosture>(observedPosture);
			}
			ImGui::InputScalar("Active contact mask", ImGuiDataType_U64,
				&state_->recoveryInput.activeContactMask, nullptr, nullptr, "%016llX",
				ImGuiInputTextFlags_CharsHexadecimal);
			ImGui::SliderFloat("Observed upright dot", &state_->recoveryInput.uprightDot,
				-1.0F, 1.0F);
			ImGui::DragFloat("Observed pose error", &state_->recoveryInput.poseErrorMeters, 0.01F, 0.0F);
			ImGui::DragFloat("Observed linear speed", &state_->recoveryInput.linearSpeed, 0.01F, 0.0F);
			ImGui::DragFloat("Observed angular speed", &state_->recoveryInput.angularSpeed, 0.01F, 0.0F);
			ImGui::DragFloat("Available radius", &state_->recoveryInput.availableClearanceRadiusMeters, 0.01F, 0.0F);
			ImGui::DragFloat("Available height", &state_->recoveryInput.availableClearanceHeightMeters, 0.01F, 0.0F);
			if(ImGui::Button("Run deterministic recovery test")) {
				state_->recoveryResult = model.TestRecovery(recovery.candidateId, state_->recoveryInput);
			}
			ImGui::TextColored(state_->recoveryResult.eligible
				? ImVec4(0.35F, 0.85F, 0.45F, 1.0F) : ImVec4(1.0F, 0.6F, 0.25F, 1.0F),
				state_->recoveryResult.eligible ? "Eligible" : "Not eligible");
			for(const std::string& failure : state_->recoveryResult.failures) {
				ImGui::BulletText("%s", failure.c_str());
			}
		}
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
	if(disableEditors) ImGui::EndDisabled();
}
#endif
