// AnimatorComponentBindingCoordinator.cpp - live AnimatorComponent bootstrap.

#include "Animation/Runtime/AnimatorComponentBindingCoordinator.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Codec/AnimationCodec.h"
#include "Animation/Graph/AnimationProgramPublisher.h"
#include "Animation/Streaming/AnimationStreaming.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Components/AnimatorComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Animation {
	namespace {
		struct RuntimeStreamingDependency {
			UUID assetId = EMPTY_UUID;
			AssetType assetType = AssetType::Skeleton;

			auto operator<=>(const RuntimeStreamingDependency&) const = default;
		};

		inline constexpr std::size_t kMaximumDiscoveredStreamingDependencies = 256U;
		inline constexpr std::size_t kMaximumControllerDependencyDepth = 64U;
		inline constexpr std::size_t kMaximumAnimatorLoadRequestsPerFrame = 8U;
		inline constexpr std::size_t kMaximumAnimationRangeRequestsPerBinding = 3U;
		inline constexpr std::size_t kMaximumGpuBindingPayloadBytes = 64U * 1024U * 1024U;
		inline constexpr std::size_t kGpuProgramHeaderBytes = 64U;
		inline constexpr std::size_t kGpuProgramJointRecordBytes = 16U;
		inline constexpr std::size_t kGpuSkeletonDirectoryBytes = 16U;
		inline constexpr std::size_t kGpuSkeletonPaletteRecordBytes = 64U;
		inline constexpr std::size_t kMaximumGpuSkeletonLevels = 128U;

		void AddStreamingDependency(
			std::vector<RuntimeStreamingDependency>& dependencies,
			const UUID& assetId,
			AssetType assetType)
		{
			if(assetId == EMPTY_UUID
				|| dependencies.size() >= kMaximumDiscoveredStreamingDependencies) return;
			dependencies.push_back({assetId, assetType});
		}

		[[nodiscard]] AssetType ControllerNodeDependencyType(
			AnimationControllerNodeKind kind) noexcept
		{
			if(kind == AnimationControllerNodeKind::Subgraph) {
				return AssetType::AnimationController;
			}
			if(kind == AnimationControllerNodeKind::MotionMatch) {
				return AssetType::MotionDatabase;
			}
			// Clip and blend nodes use clip-shaped external dependencies in the
			// current controller schema. Procedural nodes normally leave this empty.
			return AssetType::AnimationClip;
		}

		void CollectControllerStreamingDependencies(
			const AnimationControllerAsset& rootController,
			ResourceManager& resources,
			std::vector<RuntimeStreamingDependency>& dependencies)
		{
			std::vector<AnimationControllerAsset> controllers;
			controllers.push_back(rootController);
			std::set<UUID> inspectedControllers;
			if(rootController.assetId.IsValid()) {
				inspectedControllers.insert(rootController.assetId.value);
			}

			for(std::size_t index = 0U; index < controllers.size()
				&& index < kMaximumControllerDependencyDepth
				&& dependencies.size() < kMaximumDiscoveredStreamingDependencies; ++index) {
				const AnimationControllerAsset& controller = controllers[index];
				AddStreamingDependency(dependencies,
					controller.skeletonAssetId.value, AssetType::Skeleton);

				std::vector<UUID> childControllers;
				for(const AnimationControllerNode& node : controller.nodes) {
					if(!node.dependencyAssetId.IsValid()) continue;
					const AssetType dependencyType = ControllerNodeDependencyType(node.kind);
					AddStreamingDependency(dependencies,
						node.dependencyAssetId.value, dependencyType);
					if(dependencyType == AssetType::AnimationController) {
						childControllers.push_back(node.dependencyAssetId.value);
					}
				}
				for(const AnimationControllerLayer& layer : controller.layers) {
					AddStreamingDependency(dependencies,
						layer.maskAssetId.value, AssetType::AnimationMask);
				}
				for(const AnimationControllerSubgraphDependency& subgraph
					: controller.subgraphDependencies) {
					AddStreamingDependency(dependencies,
						subgraph.controllerAssetId.value, AssetType::AnimationController);
					if(subgraph.controllerAssetId.IsValid()) {
						childControllers.push_back(subgraph.controllerAssetId.value);
					}
				}

				// A loaded subgraph exposes another bounded dependency layer. Missing
				// subgraphs were still recorded above and will be inspected on a later
				// protected frame after their asynchronous publication completes.
				for(const UUID& childId : childControllers) {
					if(controllers.size() >= kMaximumControllerDependencyDepth
						|| !inspectedControllers.insert(childId).second) continue;
					AnimationControllerAsset child;
					AnimationGeneration ignoredGeneration = 0U;
					if(resources.CopyAnimationControllerPublication(
						childId, child, ignoredGeneration)) {
						controllers.push_back(std::move(child));
					}
				}
			}
		}

		[[nodiscard]] std::vector<RuntimeStreamingDependency>
		CollectAuthoredStreamingDependencies(
			const AnimatorComponent& component,
			ResourceManager& resources,
			const AnimationControllerAsset* controller)
		{
			std::vector<RuntimeStreamingDependency> dependencies;
			dependencies.reserve(16U);
			AddStreamingDependency(dependencies,
				component.controllerAssetId, AssetType::AnimationController);
			AddStreamingDependency(dependencies,
				component.defaultClipAssetId, AssetType::AnimationClip);
			AddStreamingDependency(dependencies,
				component.skeletonOverrideAssetId, AssetType::Skeleton);
			AddStreamingDependency(dependencies,
				component.retargetProfileOverrideAssetId, AssetType::RetargetProfile);
			AddStreamingDependency(dependencies,
				component.motionDatabaseOverrideAssetId, AssetType::MotionDatabase);
			AddStreamingDependency(dependencies,
				component.ragdollProfileOverrideAssetId, AssetType::RagdollProfile);

			if(controller != nullptr) {
				CollectControllerStreamingDependencies(*controller, resources, dependencies);
			}

			RetargetProfileAsset retarget;
			AnimationGeneration ignoredGeneration = 0U;
			if(component.retargetProfileOverrideAssetId != EMPTY_UUID
				&& resources.CopyRetargetProfilePublication(
					component.retargetProfileOverrideAssetId, retarget, ignoredGeneration)) {
				AddStreamingDependency(dependencies,
					retarget.sourceSkeletonAssetId.value, AssetType::Skeleton);
				AddStreamingDependency(dependencies,
					retarget.targetSkeletonAssetId.value, AssetType::Skeleton);
			}

			RagdollProfileAsset ragdoll;
			ignoredGeneration = 0U;
			if(component.ragdollProfileOverrideAssetId != EMPTY_UUID
				&& resources.CopyRagdollProfilePublication(
					component.ragdollProfileOverrideAssetId, ragdoll, ignoredGeneration)) {
				AddStreamingDependency(dependencies,
					ragdoll.skeletonAssetId.value, AssetType::Skeleton);
				for(const RagdollRecoveryRecord& recovery : ragdoll.recovery) {
					AddStreamingDependency(dependencies,
						recovery.clipAssetId.value, AssetType::AnimationClip);
				}
			}

			std::sort(dependencies.begin(), dependencies.end());
			dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
				dependencies.end());
			return dependencies;
		}

		void PruneCompletedStreamingLoads(
			std::map<RuntimeStreamingDependency, std::shared_future<UUID>>& pendingLoads)
		{
			for(auto iterator = pendingLoads.begin(); iterator != pendingLoads.end();) {
				if(iterator->second.wait_for(std::chrono::seconds(0))
					!= std::future_status::ready) {
					++iterator;
					continue;
				}
				// Consuming a shared future cannot invalidate other waiters. A failed
				// worker is discarded here so an active dependency can retry next frame.
				try {
					(void)iterator->second.get();
				} catch(...) {
				}
				iterator = pendingLoads.erase(iterator);
			}
		}

		void PublishAndRequestStreamingDependencies(
			const UUID& ownerObjectId,
			std::span<const RuntimeStreamingDependency> dependencies,
			std::uint64_t frameId,
			ResourceManager& resources,
			std::map<RuntimeStreamingDependency, std::shared_future<UUID>>& pendingLoads,
			std::size_t& issuedRequestCount)
		{
			for(const RuntimeStreamingDependency& dependency : dependencies) {
				resources.MarkStreamingNeed(
					dependency.assetId,
					dependency.assetType,
					TerraEngine::Resource::StreamingNeedType::RuntimeExplicit,
					frameId,
					"active animator dependency",
					100,
					ownerObjectId);
				if(resources.IsAnimationAssetPublished(
					dependency.assetId, dependency.assetType)) {
					pendingLoads.erase(dependency);
					continue;
				}

				const auto pending = pendingLoads.find(dependency);
				if(pending != pendingLoads.end()
					&& pending->second.wait_for(std::chrono::seconds(0))
					!= std::future_status::ready) continue;
				if(pending != pendingLoads.end()) {
					try {
						(void)pending->second.get();
					} catch(...) {
					}
					pendingLoads.erase(pending);
					if(resources.IsAnimationAssetPublished(
						dependency.assetId, dependency.assetType)) continue;
				}

				// Only a registered native cooked path is an authority for automatic
				// loading. In export mode ResourceManager reads that logical path from
				// the mounted TEPAK and its native animation reader fails closed.
				if(issuedRequestCount >= kMaximumAnimatorLoadRequestsPerFrame
					|| resources.GetRegisteredAssetPath(
						dependency.assetId, dependency.assetType).empty()) continue;
				++issuedRequestCount;
				std::shared_future<UUID> future = resources.RequestStreamableLoad(
					dependency.assetId,
					dependency.assetType,
					TerraEngine::Resource::StreamingNeedType::RuntimeExplicit,
					frameId,
					"active animator dependency load",
					ownerObjectId);
				if(future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
					pendingLoads[dependency] = std::move(future);
				} else {
					try {
						(void)future.get();
					} catch(...) {
					}
				}
			}
		}

		struct AuthoredBindingSignature {
			UUID controllerAssetId = EMPTY_UUID;
			UUID defaultClipAssetId = EMPTY_UUID;
			bool loopDefaultClip = true;
			bool playAutomatically = true;
			std::uint32_t defaultClipStartFrame = 0U;
			UUID skeletonAssetId = EMPTY_UUID;
			// The resolved controller skeleton and protected render identity are part
			// of the binding signature. A mesh recook or renderer-session rollover
			// therefore rebuilds immutable GPU payloads without an authored edit.
			UUID resolvedSkeletonAssetId = EMPTY_UUID;
			UUID renderMeshAssetId = EMPTY_UUID;
			UUID retargetProfileAssetId = EMPTY_UUID;
			UUID retargetSourceSkeletonAssetId = EMPTY_UUID;
			UUID motionDatabaseAssetId = EMPTY_UUID;
			UUID ragdollProfileAssetId = EMPTY_UUID;
			// Durable asset UUIDs are insufficient for hot reload: these values
			// identify the exact ResourceManager publication seen by this binding.
			AnimationGeneration controllerRuntimeGeneration = 0U;
			AnimationGeneration defaultClipRuntimeGeneration = 0U;
			AnimationGeneration skeletonRuntimeGeneration = 0U;
			AnimationGeneration retargetProfileRuntimeGeneration = 0U;
			AnimationGeneration retargetSourceSkeletonRuntimeGeneration = 0U;
			AnimationGeneration motionDatabaseRuntimeGeneration = 0U;
			AnimationGeneration ragdollProfileRuntimeGeneration = 0U;
			std::uint64_t controllerDependencyRuntimeFingerprint = 0U;
			std::uint64_t resolvedSkeletonCompatibilityFingerprint = 0U;
			std::uint64_t renderCandidateFingerprint = 0U;
			std::uint64_t deformablePayloadGeneration = 0U;
			std::uint64_t rendererSessionGeneration = 0U;
			std::uint32_t resolvedSkeletonJointCount = 0U;
			std::uint32_t renderMeshHeaderVersion = 0U;
			TerraEngine::Renderer::RendererRecoveryStateKind rendererState =
				TerraEngine::Renderer::RendererRecoveryStateKind::Active;
			AnimationExecutionPolicy requestedMode = AnimationExecutionPolicy::Auto;
			AnimatorRootMotionPolicy rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
			AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			AnimatorTimePolicy timePolicy = AnimatorTimePolicy::Scaled;
			std::string initialParametersJson;
			AnimatorEventPolicy eventPolicy = AnimatorEventPolicy::Dispatch;
			AnimatorCullingPolicy cullingPolicy = AnimatorCullingPolicy::Automatic;
			std::uint32_t diagnosticOverrides = 0U;

			auto operator<=>(const AuthoredBindingSignature&) const = default;
		};

		[[nodiscard]] AuthoredBindingSignature MakeSignature(
			const AnimatorComponent& component,
			const ResourceManager& resources)
		{
			AuthoredBindingSignature result;
			result.controllerAssetId = component.controllerAssetId;
			result.defaultClipAssetId = component.defaultClipAssetId;
			result.loopDefaultClip = component.loopDefaultClip;
			result.playAutomatically = component.playAutomatically;
			// The authored frame selects the initial pose for both a direct clip and
			// a controller's resolved entry clip. Keep it in every signature so an
			// Inspector edit replaces the immutable binding at the next boundary.
			result.defaultClipStartFrame = component.defaultClipStartFrame;
			result.skeletonAssetId = component.skeletonOverrideAssetId;
			result.retargetProfileAssetId = component.retargetProfileOverrideAssetId;
			result.motionDatabaseAssetId = component.motionDatabaseOverrideAssetId;
			result.ragdollProfileAssetId = component.ragdollProfileOverrideAssetId;
			result.controllerRuntimeGeneration = resources.GetAnimationAssetRuntimeGeneration(
				component.controllerAssetId, AssetType::AnimationController);
			result.defaultClipRuntimeGeneration = resources.GetAnimationAssetRuntimeGeneration(
				component.defaultClipAssetId, AssetType::AnimationClip);
			result.skeletonRuntimeGeneration = resources.GetAnimationAssetRuntimeGeneration(
				component.skeletonOverrideAssetId, AssetType::Skeleton);
			result.retargetProfileRuntimeGeneration =
				resources.GetAnimationAssetRuntimeGeneration(
					component.retargetProfileOverrideAssetId, AssetType::RetargetProfile);
			result.motionDatabaseRuntimeGeneration = resources.GetAnimationAssetRuntimeGeneration(
				component.motionDatabaseOverrideAssetId, AssetType::MotionDatabase);
			result.ragdollProfileRuntimeGeneration = resources.GetAnimationAssetRuntimeGeneration(
				component.ragdollProfileOverrideAssetId, AssetType::RagdollProfile);
			// The profile's source skeleton is an indirect dependency. Include its
			// durable and runtime identity so a same-GUID source-rig reload cannot leave
			// a compiled mapping bound to retired reference-pose bytes.
			RetargetProfileAsset retargetProfile;
			AnimationGeneration ignoredProfileGeneration = 0U;
			if(component.retargetProfileOverrideAssetId != EMPTY_UUID
				&& resources.CopyRetargetProfilePublication(
					component.retargetProfileOverrideAssetId,
					retargetProfile,
					ignoredProfileGeneration)) {
				result.retargetSourceSkeletonAssetId =
					retargetProfile.sourceSkeletonAssetId.value;
				result.retargetSourceSkeletonRuntimeGeneration =
					resources.GetAnimationAssetRuntimeGeneration(
						result.retargetSourceSkeletonAssetId, AssetType::Skeleton);
			}
			result.requestedMode = component.requestedMode;
			result.rootMotionPolicy = component.rootMotionPolicy;
			result.updatePolicy = component.updatePolicy;
			result.timePolicy = component.timePolicy;
			result.initialParametersJson = component.initialParametersJson;
			result.eventPolicy = component.eventPolicy;
			result.cullingPolicy = component.cullingPolicy;
			result.diagnosticOverrides = component.diagnosticOverrides;
			return result;
		}

		struct GpuBindingRenderIdentity {
			UUID meshId = EMPTY_UUID;
			std::uint32_t meshHeaderVersion = 0U;
			std::uint64_t deformablePayloadGeneration = 0U;
			std::uint64_t rendererSessionGeneration = 0U;
			std::uint64_t sceneGeneration = 0U;
			AnimationCompatibilityFingerprint skeletonCompatibilityFingerprint = 0U;
			std::vector<std::uint32_t> paletteJoints;
			std::vector<std::uint64_t> morphStableIds;
			std::array<float, 3U> deformationBoundsMinimum{};
			std::array<float, 3U> deformationBoundsMaximum{};
			bool valid = false;
		};

		void MixRenderIdentityValue(std::uint64_t& hash, std::uint64_t value) noexcept
		{
			for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
				hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
				hash *= 1099511628211ULL;
			}
		}

		void MixRenderIdentityUuid(std::uint64_t& hash, const UUID& value) noexcept
		{
			for(const std::uint8_t byte : value) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
		}

		[[nodiscard]] bool IsValidatedSinglePaletteDeformableMesh(
			const Mesh& mesh,
			const SkeletonAsset& skeleton) noexcept
		{
			const DeformableMeshPayloadHeader& metadata = mesh.deformableMetadata;
			constexpr std::uint32_t requiredFlags =
				kDeformableMeshPayloadFlagConservativeMorphEnvelope
				| kDeformableMeshPayloadFlagZeroWeightMeansIdentity
				| kDeformableMeshPayloadFlagVertexPaletteIndices
				| kDeformableMeshPayloadFlagDenseMorphPositionDeltas;
			constexpr std::uint32_t requiredVirtualFlags =
				kDeformableMeshVirtualGeometryFlagWholeMeshEnvelope
				| kDeformableMeshVirtualGeometryFlagDisableUnprovenConeCulling
				| kDeformableMeshVirtualGeometryFlagRequireCompleteFallback;
			if(!mesh.hasDeformablePayload
				|| mesh.deformablePayloadGeneration == 0U
				|| metadata.magic != kDeformableMeshPayloadMagicValue
				|| metadata.version != kDeformableMeshPayloadVersion
				|| metadata.headerSize != sizeof(DeformableMeshPayloadHeader)
				|| metadata.compatibleMeshVersion != mesh.header.version
				|| metadata.skinningVertexCount == 0U
				|| metadata.skinningVertexCount != mesh.header.meshletSkinningDataCount
				|| metadata.palettePartitionCount != 1U
				|| mesh.deformablePalettes.size() != 1U
				|| metadata.morphTargetCount != mesh.deformableMorphs.size()
				|| metadata.morphTargetCount
					> kGpuAnimationMaximumMorphTargetsV1
				|| metadata.vertexPaletteIndexCount != metadata.skinningVertexCount
				|| metadata.vertexPaletteIndexCount
					!= mesh.deformableVertexPaletteIndices.size()
				|| metadata.skinningStrideBytes != kDeformableMeshSkinningStrideBytes
				|| metadata.maximumInfluences != kDeformableMeshMaximumInfluences
				|| metadata.maximumPaletteJoints != kDeformableMeshMaximumPaletteJoints
				|| metadata.sourceSkeletonJointCount != skeleton.joints.size()
				|| metadata.skeletonCompatibilityFingerprint == 0U
				|| metadata.skeletonCompatibilityFingerprint
					!= skeleton.compatibilityFingerprint
				|| metadata.flags != requiredFlags
				|| metadata.virtualGeometryDeformationFlags != requiredVirtualFlags) {
				return false;
			}
			for(std::size_t lane = 0U; lane < 3U; ++lane) {
				if(!std::isfinite(metadata.deformationBoundsMin[lane])
					|| !std::isfinite(metadata.deformationBoundsMax[lane])
					|| metadata.deformationBoundsMin[lane]
						> metadata.deformationBoundsMax[lane]) {
					return false;
				}
			}

			const DeformableMeshPaletteRecord& palette =
				mesh.deformablePalettes.front();
			if(palette.jointCount == 0U
				|| palette.jointCount > kGpuAnimationPaletteEntriesPerPartition
				|| palette.jointCount != mesh.deformablePaletteJoints.size()) {
				return false;
			}
			for(std::size_t index = 0U;
				index < mesh.deformablePaletteJoints.size(); ++index) {
				const std::uint32_t joint = mesh.deformablePaletteJoints[index];
				if(joint >= skeleton.joints.size()
					|| (index != 0U
						&& joint <= mesh.deformablePaletteJoints[index - 1U])) {
					return false;
				}
			}
			for(std::size_t index = 0U;
				index < mesh.deformableMorphs.size(); ++index) {
				const std::uint32_t stableTargetId =
					mesh.deformableMorphs[index].stableTargetId;
				if(stableTargetId == 0U) return false;
				for(std::size_t prior = 0U; prior < index; ++prior) {
					if(mesh.deformableMorphs[prior].stableTargetId
						== stableTargetId) {
						return false;
					}
				}
			}
			return true;
		}

		[[nodiscard]] bool IsOwnedByAnimator(
			const Object& renderObject,
			const UUID& animatorObjectId,
			ObjectManager& objects) noexcept
		{
			std::optional<Object> current = renderObject;
			for(std::size_t depth = 0U;
				current.has_value() && depth < 1024U; ++depth) {
				if(objects.HasComponent<AnimatorComponent>(*current)) {
					const AnimatorComponent* nearest =
						objects.GetComponent<AnimatorComponent>(*current);
					// The nearest authored animator blocks every ancestor even when
					// disabled, matching RenderSystem's protected ownership rule.
					return nearest != nullptr && IsRuntimeComponentActive(*nearest)
						&& objects.getUUID(*current) == animatorObjectId;
				}
				current = objects.getParent(*current);
			}
			return false;
		}

		[[nodiscard]] GpuBindingRenderIdentity ResolveGpuBindingRenderIdentity(
			const UUID& animatorObjectId,
			const SkeletonAsset* skeleton,
			ObjectManager& objects,
			ResourceManager& resources,
			AuthoredBindingSignature& signature)
		{
			GpuBindingRenderIdentity result;
			const TerraEngine::Renderer::RendererRecoverySnapshot renderer =
				RenderSystem::Instance().GetLatestRendererRecoverySnapshot();
			result.rendererSessionGeneration = renderer.renderSessionGeneration;
			// Scene generation changes only when the protected scene publication is
			// replaced. Ordinary transform edits must not rebuild immutable clip and
			// skeleton payloads every frame; mesh/hierarchy edits are already covered
			// by the exact candidate fingerprint below.
			result.sceneGeneration = objects.GetSceneGeneration();
			signature.rendererSessionGeneration = renderer.renderSessionGeneration;
			signature.rendererState = renderer.state;

			std::uint64_t candidateFingerprint = 1469598103934665603ULL;
			MixRenderIdentityValue(candidateFingerprint,
				static_cast<std::uint64_t>(renderer.state));
			MixRenderIdentityValue(candidateFingerprint,
				renderer.renderSessionGeneration);
			std::size_t compatibleCandidateCount = 0U;
			for(const Object& renderObject
				: objects.SnapshotObjectsWithComponents<
					Transform, MeshRenderComponent>()) {
				if(!IsOwnedByAnimator(renderObject, animatorObjectId, objects)) continue;
				const MeshRenderComponent* component =
					objects.GetComponent<MeshRenderComponent>(renderObject);
				if(component == nullptr) continue;
				MixRenderIdentityUuid(candidateFingerprint, objects.getUUID(renderObject));
				MixRenderIdentityUuid(candidateFingerprint, component->meshId);
				Mesh* mesh = component->meshId == EMPTY_UUID
					? nullptr : resources.GetMesh(component->meshId);
				if(mesh == nullptr) {
					MixRenderIdentityValue(candidateFingerprint, 0U);
					continue;
				}
				MixRenderIdentityValue(candidateFingerprint, mesh->header.version);
				MixRenderIdentityValue(candidateFingerprint,
					mesh->deformablePayloadGeneration);
				MixRenderIdentityValue(candidateFingerprint,
					mesh->deformableMetadata.skeletonCompatibilityFingerprint);
				if(skeleton == nullptr
					|| !IsValidatedSinglePaletteDeformableMesh(*mesh, *skeleton)) {
					continue;
				}
				++compatibleCandidateCount;
				if(compatibleCandidateCount == 1U) {
					result.meshId = component->meshId;
					result.meshHeaderVersion = mesh->header.version;
					result.deformablePayloadGeneration =
						mesh->deformablePayloadGeneration;
					result.skeletonCompatibilityFingerprint =
						mesh->deformableMetadata.skeletonCompatibilityFingerprint;
					result.paletteJoints = mesh->deformablePaletteJoints;
					result.morphStableIds.clear();
					result.morphStableIds.reserve(
						mesh->deformableMorphs.size());
					for(const DeformableMeshMorphRecord& morph
						: mesh->deformableMorphs) {
						result.morphStableIds.push_back(
							morph.stableTargetId);
					}
					std::sort(
						result.morphStableIds.begin(),
						result.morphStableIds.end());
					std::copy_n(
						mesh->deformableMetadata.deformationBoundsMin,
						result.deformationBoundsMinimum.size(),
						result.deformationBoundsMinimum.begin());
					std::copy_n(
						mesh->deformableMetadata.deformationBoundsMax,
						result.deformationBoundsMaximum.size(),
						result.deformationBoundsMaximum.begin());
				}
			}
			MixRenderIdentityValue(candidateFingerprint, compatibleCandidateCount);
			if(candidateFingerprint == 0U) candidateFingerprint = 1U;
			signature.renderCandidateFingerprint = candidateFingerprint;
			if(compatibleCandidateCount == 1U) {
				signature.renderMeshAssetId = result.meshId;
				signature.renderMeshHeaderVersion = result.meshHeaderVersion;
				signature.deformablePayloadGeneration =
					result.deformablePayloadGeneration;
			}
			result.valid = compatibleCandidateCount == 1U
				&& TerraEngine::Renderer::CanSubmitNormalGpuWork(renderer)
				&& renderer.renderSessionGeneration != 0U
				&& result.sceneGeneration != 0U;
			return result;
		}

		struct ControllerProgramState {
			UUID assetId = EMPTY_UUID;
			AnimationGeneration sourceGeneration = 0U;
			std::optional<AnimationControllerAsset> latestController;
			std::optional<AnimationControllerAsset> publishedController;
			std::optional<AnimationProgramPublishRecord> publishedProgram;
			std::unique_ptr<AnimationProgramPublisher> publisher;
			AnimationStatus status = AnimationStatus::MissingAsset;
			bool pending = false;
		};

		[[nodiscard]] std::vector<AnimationProgramDependencyGeneration> CollectProgramDependencies(
			const AnimationControllerAsset& controller)
		{
			std::vector<AnimationProgramDependencyGeneration> dependencies;
			dependencies.reserve(controller.subgraphDependencies.size()
				+ controller.nodes.size() + controller.layers.size());
			for(const AnimationControllerSubgraphDependency& dependency : controller.subgraphDependencies) {
				dependencies.push_back({dependency.controllerAssetId, dependency.generation});
			}
			for(const AnimationControllerNode& node : controller.nodes) {
				if(node.dependencyAssetId.IsValid()) {
					dependencies.push_back({node.dependencyAssetId, node.dependencyGeneration});
				}
			}
			for(const AnimationControllerLayer& layer : controller.layers) {
				if(layer.maskAssetId.IsValid()) {
					dependencies.push_back({layer.maskAssetId, layer.maskGeneration});
				}
			}
			std::sort(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
				return left.assetId != right.assetId
					? left.assetId < right.assetId : left.generation < right.generation;
			});
			dependencies.erase(std::unique(dependencies.begin(), dependencies.end(), [](const auto& left, const auto& right) {
				return left.assetId == right.assetId && left.generation == right.generation;
			}), dependencies.end());
			return dependencies;
		}

		[[nodiscard]] std::uint64_t MixRuntimeDependency(
			std::uint64_t hash,
			const AnimationGuid& id,
			AnimationGeneration generation) noexcept
		{
			for(const std::uint8_t byte : id.value) {
				hash ^= byte;
				hash *= 1099511628211ULL;
			}
			for(std::size_t byte = 0U; byte < sizeof(generation); ++byte) {
				hash ^= (generation >> (byte * 8U)) & 0xffU;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		[[nodiscard]] std::uint64_t BuildControllerDependencyRuntimeFingerprint(
			const AnimationControllerAsset& controller,
			const ResourceManager& resources) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			for(const AnimationControllerNode& node : controller.nodes) {
				if(!node.dependencyAssetId.IsValid()) continue;
				AssetType type = AssetType::AnimationClip;
				if(node.kind == AnimationControllerNodeKind::Subgraph) {
					type = AssetType::AnimationController;
				} else if(node.kind == AnimationControllerNodeKind::MotionMatch) {
					type = AssetType::MotionDatabase;
				}
				hash = MixRuntimeDependency(hash, node.dependencyAssetId,
					resources.GetAnimationAssetRuntimeGeneration(node.dependencyAssetId.value, type));
			}
			for(const AnimationControllerLayer& layer : controller.layers) {
				if(layer.maskAssetId.IsValid()) {
					hash = MixRuntimeDependency(hash, layer.maskAssetId,
						resources.GetAnimationAssetRuntimeGeneration(
							layer.maskAssetId.value, AssetType::AnimationMask));
				}
			}
			return hash;
		}

		[[nodiscard]] AnimationStatus RefreshControllerProgram(
			const AnimatorComponent& component,
			ResourceManager& resources,
			AuthoredBindingSignature& signature,
			ControllerProgramState& state,
			bool& changed)
		{
			changed = false;
			if(component.controllerAssetId == EMPTY_UUID) {
				if(state.assetId != EMPTY_UUID || state.publishedProgram.has_value()) changed = true;
				state = {};
				state.status = AnimationStatus::Ok;
				return state.status;
			}

			const AnimationGeneration sourceGeneration = signature.controllerRuntimeGeneration;
			if(state.assetId != component.controllerAssetId
				|| state.sourceGeneration != sourceGeneration
				|| sourceGeneration == 0U || !state.latestController) {
				AnimationControllerAsset controller;
				AnimationGeneration copiedGeneration = 0U;
				if(!resources.CopyAnimationControllerPublication(
					component.controllerAssetId, controller, copiedGeneration)) {
					state.assetId = component.controllerAssetId;
					state.sourceGeneration = sourceGeneration;
					state.latestController.reset();
					state.publisher.reset();
					state.pending = false;
					state.status = AnimationStatus::MissingAsset;
					return state.status;
				}
				signature.controllerRuntimeGeneration = copiedGeneration;
				state.assetId = component.controllerAssetId;
				state.sourceGeneration = copiedGeneration;
				state.latestController = std::move(controller);
				state.publisher = std::make_unique<AnimationProgramPublisher>(
					CreateThreadPoolAnimationProgramCompileExecutor(ThreadPool::instance()), 2U);

				AnimationProgramCompileRequest request;
				request.controller = *state.latestController;
				request.sourceGeneration = copiedGeneration;
				request.dependencyGenerations = CollectProgramDependencies(request.controller);
				// Subgraph documents are copied before the worker starts. A missing
				// document leaves compilation pending on a future hot-reload retry and
				// never exposes ResourceManager storage to the compiler task.
				for(const AnimationControllerSubgraphDependency& dependency
					: request.controller.subgraphDependencies) {
					AnimationControllerAsset subgraph;
					AnimationGeneration subgraphGeneration = 0U;
					if(resources.CopyAnimationControllerPublication(
						dependency.controllerAssetId.value, subgraph, subgraphGeneration)) {
						request.availableControllers.push_back(std::move(subgraph));
					}
				}
				state.status = state.publisher->RequestCompile(request);
				state.pending = state.status == AnimationStatus::Queued;
				changed = true;
			}

			if(state.latestController) {
				signature.controllerDependencyRuntimeFingerprint =
					BuildControllerDependencyRuntimeFingerprint(*state.latestController, resources);
			}
			if(!state.pending || !state.publisher || !state.latestController) return state.status;

			AnimationProgramGenerationSnapshot snapshot;
			snapshot.controllerAssetId = state.latestController->assetId;
			snapshot.sourceGeneration = state.sourceGeneration;
			snapshot.dependencyGenerations = CollectProgramDependencies(*state.latestController);
			const AnimationProgramPublishPollResult poll = state.publisher->PollAndPublish(snapshot);
			if(poll.published) {
				state.publishedProgram = state.publisher->GetPublishedRecord();
				state.publishedController = state.latestController;
				state.pending = false;
				state.status = state.publishedProgram
					? AnimationStatus::Ok : AnimationStatus::InternalError;
				changed = true;
			} else if(poll.status != AnimationStatus::NotReady) {
				state.pending = false;
				state.status = poll.status;
				changed = true;
			}
			return state.status;
		}

		[[nodiscard]] AnimationAssetHandle MakeAuthoredAssetHandle(
			const UUID& id,
			AnimationGeneration generation = 1U) noexcept
		{
			AnimationAssetHandle handle;
			handle.assetId.value = id;
			handle.generation = id == EMPTY_UUID ? 0U : (generation == 0U ? 1U : generation);
			return handle;
		}

		[[nodiscard]] const EvaluationState* ResolveEntryLeaf(
			const EvaluationProgram& program) noexcept
		{
			const EvaluationState* current = nullptr;
			for(const EvaluationState& state : program.states) {
				if(state.parentStateId == 0U && state.entry) {
					current = &state;
					break;
				}
			}
			for(std::size_t depth = 0U; current != nullptr && depth < 64U; ++depth) {
				const EvaluationState* child = nullptr;
				for(const EvaluationState& state : program.states) {
					if(state.parentStateId == current->stableId && state.entry) {
						child = &state;
						break;
					}
				}
				if(child == nullptr) return current;
				current = child;
			}
			return nullptr;
		}

		struct ClipStreamGenerationKey {
			UUID assetId = EMPTY_UUID;
			AnimationGeneration generation = 0U;

			auto operator<=>(const ClipStreamGenerationKey&) const = default;
		};

		struct ClipStreamPayloadKey {
			UUID assetId = EMPTY_UUID;
			AnimationStreamPayloadKind kind = AnimationStreamPayloadKind::ClipSegment;
			AnimationGeneration generation = 0U;
			std::uint32_t blockIndex = 0U;

			auto operator<=>(const ClipStreamPayloadKey&) const = default;
		};

		struct ClipStreamingRuntime {
			// These hard bounds cover all active animators together. QueueRead uses
			// the shared ThreadPool and never captures ObjectManager or live bindings.
			AnimationStreamingCoordinator coordinator{8U, 64U, 64U * 1024U * 1024U, 4096U};
			std::map<ClipStreamPayloadKey, AnimationStreamReadJob> pendingReads;
			std::map<ClipStreamPayloadKey, std::uint32_t> gameplayCriticalRetainers;
			std::map<ClipStreamGenerationKey, AnimationCookedRangeTable> clipRangeTables;
			struct MotionStreamPlan {
				AnimationCookedRangeTable ranges;
				std::vector<std::uint64_t> expectedContentFingerprints;
				bool usable = false;
			};
			std::map<ClipStreamGenerationKey, MotionStreamPlan> motionPlans;
			std::shared_ptr<const AnimationStreamingResidencySnapshot> snapshot;
		};

		struct ClipStreamingBindingState {
			std::vector<AnimationStreamRequest> retainedRequests;
			bool pending = false;
			bool motionDatabaseStreamed = false;
			bool motionDatabaseFallback = false;
		};

		struct ResolvedClipStreamingPayloads {
			std::vector<std::uint8_t> residentSegments;
			std::shared_ptr<const AnimationClipSegmentPayloadSet> payloads;
		};

		[[nodiscard]] ClipStreamPayloadKey MakeClipStreamPayloadKey(
			const AnimationStreamRequest& request) noexcept
		{
			return {request.assetId,
				request.kind,
				static_cast<AnimationGeneration>(request.generation), request.blockIndex};
		}

		[[nodiscard]] bool SameClipStreamRequest(
			const AnimationStreamRequest& left,
			const AnimationStreamRequest& right) noexcept
		{
			return left.assetId == right.assetId && left.kind == right.kind
				&& left.blockIndex == right.blockIndex && left.byteSize == right.byteSize
				&& left.generation == right.generation
				&& left.prefetch == right.prefetch && left.priority == right.priority
				&& left.logicalPath.lexically_normal() == right.logicalPath.lexically_normal()
				&& left.byteOffset == right.byteOffset
				&& left.expectedContentFingerprint == right.expectedContentFingerprint;
		}

		bool PollClipStreamReads(ClipStreamingRuntime& runtime)
		{
			bool published = false;
			for(auto iterator = runtime.pendingReads.begin();
				iterator != runtime.pendingReads.end();) {
				if(iterator->second.completion.wait_for(std::chrono::seconds(0))
					!= std::future_status::ready) {
					++iterator;
					continue;
				}
				try {
					published = runtime.coordinator.PublishReadResult(
						iterator->second.completion.get()) || published;
				} catch(...) {
					// A worker exception cannot cross the protected boundary. Mark this
					// exact generation failed and keep the immutable fallback visible.
					(void)runtime.coordinator.Complete(
						iterator->second.decision.request, false);
				}
				iterator = runtime.pendingReads.erase(iterator);
			}
			runtime.snapshot = runtime.coordinator.LoadResidencySnapshot();
			return published;
		}

		void AppendRetainedRequest(
			ClipStreamingBindingState& state,
			const AnimationStreamRequest& request)
		{
			if(std::none_of(state.retainedRequests.begin(), state.retainedRequests.end(),
				[&](const AnimationStreamRequest& value) {
					return SameClipStreamRequest(value, request);
				})) {
				state.retainedRequests.push_back(request);
			}
		}

		[[nodiscard]] ResolvedClipStreamingPayloads ResolveClipSegmentResidency(
			const AnimationClipAsset& clip,
			AnimationGeneration generation,
			bool gameplayCritical,
			ResourceManager& resources,
			ClipStreamingRuntime& runtime,
			ClipStreamingBindingState& state)
		{
			ResolvedClipStreamingPayloads resolved;
			resolved.residentSegments.assign(clip.segments.size(), 0U);
			AnimationClipSegmentPayloadSet payloadSet;
			payloadSet.assetId = clip.assetId;
			payloadSet.generation = generation;
			if(!runtime.snapshot) runtime.snapshot = runtime.coordinator.LoadResidencySnapshot();
			payloadSet.publicationSequence = runtime.snapshot
				? runtime.snapshot->publicationSequence : 0U;
			auto finish = [&]() {
				const bool allSegmentsResident =
					payloadSet.segments.size() == clip.segments.size()
					&& std::all_of(resolved.residentSegments.begin(),
						resolved.residentSegments.end(), [](std::uint8_t value) {
							return value != 0U;
						});
				if(gameplayCritical && !allSegmentsResident) {
					// Events and root motion are authoritative gameplay outputs. Until
					// every independently checked segment is available, an empty bitmap
					// plus null payload set deliberately selects AnimationClipView's
					// complete DATA path instead of turning one residency miss into a
					// silent skipped event or root-motion interval.
					resolved.residentSegments.clear();
					resolved.payloads.reset();
					return;
				}
				resolved.payloads = std::make_shared<const AnimationClipSegmentPayloadSet>(
					std::move(payloadSet));
			};
			if(clip.segments.empty() || !clip.assetId.IsValid() || generation == 0U) {
				finish();
				return resolved;
			}

			const ClipStreamGenerationKey generationKey{clip.assetId.value, generation};
			auto ranges = runtime.clipRangeTables.find(generationKey);
			if(ranges == runtime.clipRangeTables.end()) {
				ranges = runtime.clipRangeTables.emplace(generationKey,
					resources.DescribeAnimationClipSegmentRanges(
						clip.assetId.value, generation)).first;
			}
			if(!ranges->second.Succeeded()
				|| ranges->second.blocks.size() != clip.segments.size()) {
				finish();
				return resolved;
			}

			std::size_t issued = 0U;
			for(std::size_t index = 0U; index < clip.segments.size(); ++index) {
				const AnimationClipSegment& segment = clip.segments[index];
				const AnimationCookedBlockRange& block = ranges->second.blocks[index];
				if(block.blockIndex != index || block.byteSize != segment.streamSize
					|| segment.streamOffset > clip.streamData.size()
					|| segment.streamSize > clip.streamData.size() - segment.streamOffset) continue;

				AnimationStreamRequest request;
				request.assetId = clip.assetId.value;
				request.kind = AnimationStreamPayloadKind::ClipSegment;
				request.blockIndex = static_cast<std::uint32_t>(index);
				request.byteSize = block.byteSize;
				request.generation = generation;
				request.gameplayCritical = gameplayCritical;
				// Segment zero is the immediate demand; later segments form a small,
				// stable prefetch horizon and never change immutable request priority.
				request.prefetch = index != 0U;
				request.priority = index == 0U ? 100 : 10;
				request.logicalPath = ranges->second.logicalPath;
				request.byteOffset = block.byteOffset;
				request.expectedContentFingerprint =
					ComputeAnimationStreamContentFingerprint(std::span<const std::uint8_t>(
						clip.streamData).subspan(static_cast<std::size_t>(segment.streamOffset),
							static_cast<std::size_t>(segment.streamSize)));

				if(runtime.snapshot) {
					const AnimationResidentPayload* resident = runtime.snapshot->Find(request);
					if(resident != nullptr && resident->bytes) {
						resolved.residentSegments[index] = 1U;
						AnimationClipSegmentPayload payload;
						payload.segmentIndex = static_cast<std::uint32_t>(index);
						payload.streamOffset = segment.streamOffset;
						payload.streamSize = segment.streamSize;
						payload.contentFingerprint = request.expectedContentFingerprint;
						payload.bytes = resident->bytes;
						payloadSet.segments.push_back(std::move(payload));
						AppendRetainedRequest(state, request);
						continue;
					}
				}

				const std::optional<AnimationStreamDecision> decision =
					runtime.coordinator.Query(request);
				if(decision && decision->state == AnimationStreamState::Requested) {
					state.pending = true;
					AppendRetainedRequest(state, request);
					continue;
				}
				if(decision && decision->state != AnimationStreamState::Missing) continue;
				if(issued >= kMaximumAnimationRangeRequestsPerBinding) {
					state.pending = true;
					continue;
				}

				AnimationStreamReadJob job = runtime.coordinator.QueueRead(resources, request);
				if(job.decision.accepted) {
					runtime.pendingReads.insert_or_assign(
						MakeClipStreamPayloadKey(request), std::move(job));
					AppendRetainedRequest(state, request);
					state.pending = true;
					++issued;
				} else {
					// Capacity rejection is transient. Keep rebuilding at protected
					// boundaries until another request completes and frees the bound.
					state.pending = true;
				}
			}
			finish();
			return resolved;
		}

		[[nodiscard]] std::shared_ptr<const MotionDatabaseAsset>
		ResolveMotionDatabaseStreaming(
			MotionDatabaseAsset database,
			AnimationGeneration generation,
			bool gameplayCritical,
			ResourceManager& resources,
			ClipStreamingRuntime& runtime,
			ClipStreamingBindingState& state)
		{
			state.motionDatabaseFallback = true;
			auto useCompleteDataFallback = [&]() {
				// DATA is already a complete, validated ResourceManager publication.
				// Moving it into the binding preserves deterministic full search when
				// STRM is absent, incomplete, stale, cancelled, or corrupt.
				return std::make_shared<const MotionDatabaseAsset>(std::move(database));
			};
			if(database.assetId.value == EMPTY_UUID || generation == 0U
				|| database.runtimeGeneration != generation
				|| database.residentBlocks.empty()) {
				return useCompleteDataFallback();
			}

			const ClipStreamGenerationKey generationKey{database.assetId.value, generation};
			auto plan = runtime.motionPlans.find(generationKey);
			if(plan == runtime.motionPlans.end()) {
				ClipStreamingRuntime::MotionStreamPlan stagedPlan;
				stagedPlan.ranges = resources.DescribeMotionDatabaseBlockRanges(
					database.assetId.value, generation);
				if(stagedPlan.ranges.Succeeded()
					&& stagedPlan.ranges.blocks.size() == database.residentBlocks.size()) {
					stagedPlan.expectedContentFingerprints.reserve(
						database.residentBlocks.size());
					stagedPlan.usable = true;
					for(std::size_t index = 0U;
						index < database.residentBlocks.size(); ++index) {
						const MotionDatabaseResidentBlock& block =
							database.residentBlocks[index];
						const AnimationCookedBlockRange& range =
							stagedPlan.ranges.blocks[index];
						std::vector<std::uint8_t> encodedBlock;
						if(block.blockIndex != index || range.blockIndex != index
							|| range.byteSize != block.cpuByteSize
							|| EncodeMotionDatabaseStreamBlock(
								database, block.blockIndex, encodedBlock) != AnimationStatus::Ok
							|| encodedBlock.size() != range.byteSize) {
							stagedPlan.usable = false;
							stagedPlan.expectedContentFingerprints.clear();
							break;
						}
						// The coordinator protects physical bytes with an independent raw
						// FNV. MotionDatabaseResidentBlock::contentFingerprint remains the
						// decoded DATA sample fingerprint checked by Decode below.
						stagedPlan.expectedContentFingerprints.push_back(
							ComputeAnimationStreamContentFingerprint(encodedBlock));
					}
				}
				plan = runtime.motionPlans.emplace(
					generationKey, std::move(stagedPlan)).first;
			}
			if(!plan->second.usable
				|| plan->second.expectedContentFingerprints.size()
					!= database.residentBlocks.size()) {
				return useCompleteDataFallback();
			}
			if(!runtime.snapshot) runtime.snapshot = runtime.coordinator.LoadResidencySnapshot();

			bool allBlocksResident = true;
			std::size_t issued = 0U;
			std::vector<MotionDatabaseSample> decodedSamples;
			decodedSamples.reserve(database.samples.size());
			for(std::size_t index = 0U; index < database.residentBlocks.size(); ++index) {
				const MotionDatabaseResidentBlock& block = database.residentBlocks[index];
				const AnimationCookedBlockRange& range = plan->second.ranges.blocks[index];
				AnimationStreamRequest request;
				request.assetId = database.assetId.value;
				request.kind = AnimationStreamPayloadKind::MotionDatabaseBlock;
				request.blockIndex = block.blockIndex;
				request.byteSize = range.byteSize;
				request.generation = generation;
				request.gameplayCritical = gameplayCritical;
				request.prefetch = index != 0U;
				request.priority = index == 0U ? 90 : 9;
				request.logicalPath = plan->second.ranges.logicalPath;
				request.byteOffset = range.byteOffset;
				request.expectedContentFingerprint =
					plan->second.expectedContentFingerprints[index];

				const AnimationResidentPayload* resident = runtime.snapshot
					? runtime.snapshot->Find(request) : nullptr;
				if(resident != nullptr && resident->bytes) {
					// Copy the shared owner before decoding so this exact immutable block
					// remains alive through candidate validation even if a later protected
					// boundary replaces the coordinator snapshot.
					const std::shared_ptr<const std::vector<std::uint8_t>> pinnedBytes =
						resident->bytes;
					std::vector<MotionDatabaseSample> decodedBlock;
					const AnimationStatus decodeStatus = DecodeMotionDatabaseStreamBlock(
						database, block.blockIndex,
						std::span<const std::uint8_t>(*pinnedBytes), decodedBlock);
					if(decodeStatus != AnimationStatus::Ok
						|| decodedBlock.size() != block.sampleCount
						|| block.firstSample != decodedSamples.size()) {
						allBlocksResident = false;
						continue;
					}
					decodedSamples.insert(decodedSamples.end(),
						decodedBlock.begin(), decodedBlock.end());
					AppendRetainedRequest(state, request);
					continue;
				}

				allBlocksResident = false;
				const std::optional<AnimationStreamDecision> decision =
					runtime.coordinator.Query(request);
				if(decision && decision->state == AnimationStreamState::Requested) {
					state.pending = true;
					AppendRetainedRequest(state, request);
					continue;
				}
				if(decision && decision->state != AnimationStreamState::Missing) continue;
				if(issued >= kMaximumAnimationRangeRequestsPerBinding) {
					state.pending = true;
					continue;
				}

				AnimationStreamReadJob job = runtime.coordinator.QueueRead(resources, request);
				if(job.decision.accepted) {
					runtime.pendingReads.insert_or_assign(
						MakeClipStreamPayloadKey(request), std::move(job));
					AppendRetainedRequest(state, request);
					state.pending = true;
					++issued;
				} else {
					state.pending = true;
				}
			}

			if(allBlocksResident && decodedSamples.size() == database.samples.size()) {
				std::vector<MotionDatabaseSample> completeDataSamples =
					std::move(database.samples);
				database.samples = std::move(decodedSamples);
				if(ValidateMotionDatabaseAsset(database) == AnimationStatus::Ok) {
					state.motionDatabaseStreamed = true;
					state.motionDatabaseFallback = false;
					return std::make_shared<const MotionDatabaseAsset>(std::move(database));
				}
				database.samples = std::move(completeDataSamples);
			}
			return useCompleteDataFallback();
		}

		void ReconcileClipStreamRetains(
			ClipStreamingRuntime& runtime,
			std::vector<AnimationStreamRequest>& current,
			const std::vector<AnimationStreamRequest>& desired)
		{
			for(const AnimationStreamRequest& request : current) {
				const auto replacement = std::find_if(desired.begin(), desired.end(),
					[&](const AnimationStreamRequest& candidate) {
						return SameClipStreamRequest(request, candidate);
					});
				if(replacement == desired.end()) {
					(void)runtime.coordinator.Release(request);
					if(request.gameplayCritical) {
						const ClipStreamPayloadKey key = MakeClipStreamPayloadKey(request);
						if(auto critical = runtime.gameplayCriticalRetainers.find(key);
							critical != runtime.gameplayCriticalRetainers.end()) {
							if(critical->second > 1U) --critical->second;
							else runtime.gameplayCriticalRetainers.erase(critical);
						}
						(void)runtime.coordinator.SetGameplayCritical(request,
							runtime.gameplayCriticalRetainers.contains(key));
					}
				}
			}
			for(const AnimationStreamRequest& request : desired) {
				const auto prior = std::find_if(current.begin(), current.end(),
					[&](const AnimationStreamRequest& candidate) {
						return SameClipStreamRequest(request, candidate);
					});
				const ClipStreamPayloadKey key = MakeClipStreamPayloadKey(request);
				if(prior == current.end()) {
					(void)runtime.coordinator.Retain(request);
					if(request.gameplayCritical) {
						std::uint32_t& count = runtime.gameplayCriticalRetainers[key];
						if(count < std::numeric_limits<std::uint32_t>::max()) ++count;
					}
					(void)runtime.coordinator.SetGameplayCritical(request,
						runtime.gameplayCriticalRetainers.contains(key));
				} else if(prior->gameplayCritical != request.gameplayCritical) {
					if(request.gameplayCritical) {
						std::uint32_t& count = runtime.gameplayCriticalRetainers[key];
						if(count < std::numeric_limits<std::uint32_t>::max()) ++count;
					} else if(auto critical = runtime.gameplayCriticalRetainers.find(key);
						critical != runtime.gameplayCriticalRetainers.end()) {
						if(critical->second > 1U) --critical->second;
						else runtime.gameplayCriticalRetainers.erase(critical);
					}
					(void)runtime.coordinator.SetGameplayCritical(request,
						runtime.gameplayCriticalRetainers.contains(key));
				}
			}
			current = desired;
		}

		void ReleaseClipStreamRetains(
			ClipStreamingRuntime& runtime,
			std::vector<AnimationStreamRequest>& requests)
		{
			for(const AnimationStreamRequest& request : requests) {
				(void)runtime.coordinator.Release(request);
				if(!request.gameplayCritical) continue;
				const ClipStreamPayloadKey key = MakeClipStreamPayloadKey(request);
				if(auto critical = runtime.gameplayCriticalRetainers.find(key);
					critical != runtime.gameplayCriticalRetainers.end()) {
					if(critical->second > 1U) --critical->second;
					else runtime.gameplayCriticalRetainers.erase(critical);
				}
				(void)runtime.coordinator.SetGameplayCritical(request,
					runtime.gameplayCriticalRetainers.contains(key));
			}
			requests.clear();
		}

		[[nodiscard]] bool TryAlignGpuPayload(
			std::vector<std::uint8_t>& bytes,
			std::size_t alignment)
		{
			if(alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
			const std::size_t padding = (alignment - (bytes.size() & (alignment - 1U)))
				& (alignment - 1U);
			if(padding > kMaximumGpuBindingPayloadBytes - bytes.size()) return false;
			bytes.resize(bytes.size() + padding, 0U);
			return true;
		}

		template<typename Value>
		[[nodiscard]] bool TryAppendGpuValue(
			std::vector<std::uint8_t>& bytes,
			const Value& value)
		{
			if(sizeof(Value) > kMaximumGpuBindingPayloadBytes - bytes.size()) return false;
			const std::size_t offset = bytes.size();
			bytes.resize(offset + sizeof(Value));
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
			return true;
		}

		template<typename Value>
		void WriteGpuValue(
			std::vector<std::uint8_t>& bytes,
			std::size_t offset,
			const Value& value)
		{
			std::memcpy(bytes.data() + offset, &value, sizeof(Value));
		}

		[[nodiscard]] bool IsIdentityMatrix(
			const AnimationMatrix4x4& matrix) noexcept
		{
			for(std::size_t row = 0U; row < 4U; ++row) {
				for(std::size_t column = 0U; column < 4U; ++column) {
					const float expected = row == column ? 1.0F : 0.0F;
					if(std::abs(matrix.values[row * 4U + column] - expected)
						> 1.0e-6F) {
						return false;
					}
				}
			}
			return true;
		}

		enum class InverseBindStorage {
			Unknown,
			RowMajor,
			ColumnMajor
		};

		[[nodiscard]] InverseBindStorage ResolveInverseBindStorage(
			const SkeletonAsset& skeleton)
		{
			std::string extension =
				std::filesystem::path(skeleton.source.sourcePackagePath)
					.extension().string();
			for(char& value : extension) {
				if(value >= 'A' && value <= 'Z') {
					value = static_cast<char>(value - 'A' + 'a');
				}
			}
			if(extension == ".fbx") return InverseBindStorage::RowMajor;
			if(extension == ".gltf" || extension == ".glb") {
				return InverseBindStorage::ColumnMajor;
			}
			return InverseBindStorage::Unknown;
		}

		[[nodiscard]] bool ConvertInverseBindToRowMajor(
			const AnimationMatrix4x4& source,
			InverseBindStorage storage,
			std::array<float, 16U>& output) noexcept
		{
			// Legacy assets did not persist an explicit matrix-major flag. Known
			// importers have stable conventions; unknown provenance is accepted only
			// for identity matrices, whose transpose is identical.
			if(storage == InverseBindStorage::Unknown
				&& !IsIdentityMatrix(source)) {
				return false;
			}
			for(std::size_t row = 0U; row < 4U; ++row) {
				for(std::size_t column = 0U; column < 4U; ++column) {
					const std::size_t sourceIndex =
						storage == InverseBindStorage::ColumnMajor
							? column * 4U + row : row * 4U + column;
					const float value = source.values[sourceIndex];
					if(!std::isfinite(value)) return false;
					output[row * 4U + column] = value;
				}
			}
			// The skinning shader consumes three affine rows. Reject perspective or
			// malformed matrices instead of silently dropping their fourth row.
			return std::abs(output[12U]) <= 1.0e-4F
				&& std::abs(output[13U]) <= 1.0e-4F
				&& std::abs(output[14U]) <= 1.0e-4F
				&& std::abs(output[15U] - 1.0F) <= 1.0e-4F;
		}

		struct GpuSkeletonPaletteRecord {
			std::array<std::uint32_t, 4U> jointAndReserved{};
			std::array<float, 12U> inverseBindRows{};
		};
		static_assert(sizeof(GpuSkeletonPaletteRecord)
			== kGpuSkeletonPaletteRecordBytes);

		[[nodiscard]] bool BuildGpuSkeletonTable(
			const SkeletonAsset& skeleton,
			std::span<const std::uint32_t> paletteJoints,
			std::vector<std::uint8_t>& output,
			std::uint64_t& topologyId)
		{
			if(skeleton.joints.empty()
				|| skeleton.joints.size() > kGpuAnimationMaximumJointsV1
				|| skeleton.joints.size() != skeleton.inverseBindMatrices.size()
				|| paletteJoints.empty()
				|| paletteJoints.size()
					> kGpuAnimationPaletteEntriesPerPartition) {
				return false;
			}

			std::vector<std::uint32_t> depths(skeleton.joints.size(), 0U);
			std::uint32_t levelCount = 0U;
			for(std::size_t index = 0U; index < skeleton.joints.size(); ++index) {
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				if(parent < -1 || (parent >= 0 && static_cast<std::size_t>(parent) >= index)) {
					return false;
				}
				const std::uint32_t depth = parent < 0
					? 0U : depths[static_cast<std::size_t>(parent)] + 1U;
				if(depth >= kMaximumGpuSkeletonLevels) return false;
				depths[index] = depth;
				levelCount = std::max(levelCount, depth + 1U);
			}
			if(levelCount == 0U) return false;

			std::vector<std::uint8_t> table(kGpuSkeletonDirectoryBytes, 0U);
			const std::uint32_t parentTableOffset =
				static_cast<std::uint32_t>(table.size());
			for(const SkeletonJoint& joint : skeleton.joints) {
				if(!TryAppendGpuValue(table, joint.parentIndex)) return false;
			}
			if(!TryAlignGpuPayload(table, 16U)
				|| table.size() > std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			const std::uint32_t levelTableOffset =
				static_cast<std::uint32_t>(table.size());
			// One explicit depth per joint supports ordinary depth-first imported
			// skeletons whose depth decreases at sibling branches. The shader still
			// proves every followed parent has a strictly smaller bounded depth.
			for(const std::uint32_t depth : depths) {
				if(!TryAppendGpuValue(table, depth)) return false;
			}
			if(!TryAlignGpuPayload(table, 16U)
				|| table.size() > std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			const std::uint32_t paletteTableOffset =
				static_cast<std::uint32_t>(table.size());
			const InverseBindStorage storage =
				ResolveInverseBindStorage(skeleton);
			for(const std::uint32_t jointIndex : paletteJoints) {
				if(jointIndex >= skeleton.inverseBindMatrices.size()) return false;
				std::array<float, 16U> rowMajor{};
				if(!ConvertInverseBindToRowMajor(
					skeleton.inverseBindMatrices[jointIndex],
					storage,
					rowMajor)) {
					return false;
				}
				GpuSkeletonPaletteRecord record{};
				record.jointAndReserved[0U] = jointIndex;
				std::copy_n(rowMajor.begin(), record.inverseBindRows.size(),
					record.inverseBindRows.begin());
				if(!TryAppendGpuValue(table, record)) return false;
			}
			if(table.size() > kMaximumGpuBindingPayloadBytes) return false;
			WriteGpuValue(table, 0U, parentTableOffset);
			WriteGpuValue(table, 4U, levelTableOffset);
			WriteGpuValue(table, 8U, levelCount);
			WriteGpuValue(table, 12U, paletteTableOffset);

			// Topology identity is derived only from durable joint IDs and parents.
			// It remains stable across cache placement and same-content reloads.
			std::uint64_t hash = 1469598103934665603ULL;
			const auto mix = [&hash](std::uint64_t value) {
				for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
					hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
					hash *= 1099511628211ULL;
				}
			};
			mix(skeleton.compatibilityFingerprint);
			mix(skeleton.joints.size());
			for(const SkeletonJoint& joint : skeleton.joints) {
				mix(joint.stableId);
				mix(static_cast<std::uint32_t>(joint.parentIndex));
			}
			if(hash == 0U) hash = 1U;
			output = std::move(table);
			topologyId = hash;
			return true;
		}

		[[nodiscard]] bool CopyGpuClipBytes(
			const AnimatorPlaybackBinding& binding,
			std::vector<std::uint8_t>& output)
		{
			if(!binding.clip || binding.clipGeneration == 0U
				|| binding.clip->segments.empty()
				|| binding.clip->segments.size()
					> kGpuAnimationMaximumTrackSegmentsV1
				|| ValidateAnimationClipAsset(*binding.clip) != AnimationStatus::Ok) {
				return false;
			}

			AnimationCodecView view;
			if(view.Open(
				*binding.clip,
				binding.clipGeneration,
				binding.streamedSegmentPayloads) != AnimationStatus::Ok) {
				return false;
			}
			if(binding.clip->streamData.size() > kMaximumGpuBindingPayloadBytes) return false;

			if(!binding.streamedSegmentPayloads) {
				output = binding.clip->streamData;
				return true;
			}
			const AnimationClipSegmentPayloadSet& payloadSet =
				*binding.streamedSegmentPayloads;
			if(payloadSet.assetId != binding.clip->assetId
				|| payloadSet.generation != binding.clipGeneration
				|| payloadSet.segments.size() != binding.clip->segments.size()) {
				return false;
			}
			output.assign(binding.clip->streamData.size(), 0U);
			for(const AnimationClipSegmentPayload& payload : payloadSet.segments) {
				if(!payload.bytes
					|| payload.streamOffset > output.size()
					|| payload.bytes->size() > output.size() - payload.streamOffset) {
					return false;
				}
				std::memcpy(
					output.data() + static_cast<std::size_t>(payload.streamOffset),
					payload.bytes->data(),
					payload.bytes->size());
			}
			return true;
		}

		[[nodiscard]] bool TryAppendGpuConstantTrack(
			std::vector<std::uint8_t>& clipBytes,
			const AnimationClipSegment& segment,
			AnimationClipTrackKind kind,
			const AnimationTransform& bindTransform,
			std::uint32_t& offset)
		{
			if(!TryAlignGpuPayload(clipBytes, alignof(std::uint32_t))
				|| clipBytes.size() > std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			offset = static_cast<std::uint32_t>(clipBytes.size());
			AnimationCodecTrackHeaderV1 header;
			header.encoding = kind == AnimationClipTrackKind::Rotation
				? AnimationCodecValueEncoding::QuaternionSmallestThree16
				: AnimationCodecValueEncoding::Vector3Unorm16;
			header.flags = kAnimationCodecConstantTrack;
			header.byteSize = sizeof(header);
			header.keyCount = 1U;
			header.segmentStartTick = segment.startTick;
			header.segmentEndTick = segment.endTick;
			header.constantVector = kind == AnimationClipTrackKind::Translation
				? bindTransform.translation : bindTransform.scale;
			header.constantRotation = bindTransform.rotation;
			return TryAppendGpuValue(clipBytes, header);
		}

		[[nodiscard]] bool TryAppendGpuConstantScalarTrack(
			std::vector<std::uint8_t>& clipBytes,
			const AnimationClipSegment& segment,
			float bindWeight,
			std::uint32_t& offset)
		{
			if(!std::isfinite(bindWeight)
				|| !TryAlignGpuPayload(
					clipBytes, alignof(std::uint32_t))
				|| clipBytes.size()
					> std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			offset = static_cast<std::uint32_t>(clipBytes.size());
			AnimationCodecTrackHeaderV1 header;
			header.encoding =
				AnimationCodecValueEncoding::ScalarUnorm16;
			header.flags = kAnimationCodecConstantTrack;
			header.byteSize = sizeof(header);
			header.keyCount = 1U;
			header.segmentStartTick = segment.startTick;
			header.segmentEndTick = segment.endTick;
			header.constantScalar = bindWeight;
			return TryAppendGpuValue(clipBytes, header);
		}

		[[nodiscard]] bool TryBuildGpuTrackDirectory(
			const AnimationClipAsset& clip,
			const SkeletonJoint& joint,
			AnimationClipTrackKind kind,
			std::vector<std::uint8_t>& clipBytes,
			std::uint32_t& offset)
		{
			if(clip.segments.empty()
				|| clip.segments.size() > kGpuAnimationMaximumTrackSegmentsV1) {
				return false;
			}
			std::vector<GpuAnimationTrackDirectoryEntry> entries;
			entries.reserve(clip.segments.size());
			for(std::size_t segmentIndex = 0U;
				segmentIndex < clip.segments.size(); ++segmentIndex) {
				const AnimationClipSegment& segment = clip.segments[segmentIndex];
				if(segment.startTick < 0 || segment.endTick < segment.startTick
					|| static_cast<std::uint64_t>(segment.endTick)
						> std::numeric_limits<std::uint32_t>::max()) {
					return false;
				}
				const auto found = std::find_if(
					clip.tracks.begin(), clip.tracks.end(),
					[&](const AnimationClipTrack& track) {
						return track.segmentIndex == segmentIndex
							&& track.stableTargetId == joint.stableId
							&& track.kind == kind;
					});
				std::uint32_t trackOffset = 0U;
				if(found == clip.tracks.end()) {
					if(!TryAppendGpuConstantTrack(
						clipBytes,
						segment,
						kind,
						joint.localBindTransform,
						trackOffset)) {
						return false;
					}
				} else {
					if(found->sampleOffset
							> std::numeric_limits<std::uint32_t>::max()
						|| found->sampleOffset > clipBytes.size()
						|| sizeof(AnimationCodecTrackHeaderV1)
							> clipBytes.size()
								- static_cast<std::size_t>(
									found->sampleOffset)) {
						return false;
					}
					AnimationCodecTrackHeaderV1 header{};
					std::memcpy(
						&header,
						clipBytes.data()
							+ static_cast<std::size_t>(found->sampleOffset),
						sizeof(header));
					const bool constant =
						(header.flags & kAnimationCodecConstantTrack) != 0U;
					if(header.segmentStartTick != segment.startTick
						|| header.segmentEndTick != segment.endTick
						|| header.byteSize < sizeof(AnimationCodecTrackHeaderV1)
						|| header.byteSize > clipBytes.size()
							- static_cast<std::size_t>(found->sampleOffset)
						|| (!constant
							&& found->interpolation
								!= AnimationInterpolation::Linear)) {
						// The GPU decoder is linear. Step and cubic tracks stay on
						// the CPU path unless the cooked record is truly constant.
						return false;
					}
					if(found->sampleOffset % alignof(std::uint32_t) == 0U) {
						trackOffset =
							static_cast<std::uint32_t>(found->sampleOffset);
					} else {
						// Older cooked clips packed two-byte value streams back to
						// back, so a following header could be only uint16 aligned.
						// Re-home that complete immutable record at a dword boundary
						// before publishing it to ByteAddressBuffer.Load.
						std::vector<std::uint8_t> alignedTrack(header.byteSize);
						std::memcpy(
							alignedTrack.data(),
							clipBytes.data()
								+ static_cast<std::size_t>(found->sampleOffset),
							alignedTrack.size());
						if(!TryAlignGpuPayload(
								clipBytes, alignof(std::uint32_t))
							|| alignedTrack.size()
								> kMaximumGpuBindingPayloadBytes
									- clipBytes.size()) {
							return false;
						}
						trackOffset =
							static_cast<std::uint32_t>(clipBytes.size());
						clipBytes.insert(
							clipBytes.end(),
							alignedTrack.begin(), alignedTrack.end());
					}
				}
				entries.push_back({
					static_cast<std::uint32_t>(segment.startTick),
					static_cast<std::uint32_t>(segment.endTick),
					trackOffset,
					0U});
			}

			if(!TryAlignGpuPayload(clipBytes, 16U)
				|| clipBytes.size()
					> std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			offset = static_cast<std::uint32_t>(clipBytes.size());
			GpuAnimationTrackDirectoryHeader header;
			header.entryCount = static_cast<std::uint32_t>(entries.size());
			if(!TryAppendGpuValue(clipBytes, header)) return false;
			for(const GpuAnimationTrackDirectoryEntry& entry : entries) {
				if(!TryAppendGpuValue(clipBytes, entry)) return false;
			}
			return true;
		}

		[[nodiscard]] bool TryBuildGpuMorphTrackDirectory(
			const AnimationClipAsset& clip,
			std::uint64_t stableTargetId,
			float bindWeight,
			std::vector<std::uint8_t>& clipBytes,
			std::uint32_t& offset)
		{
			if(stableTargetId == 0U || !std::isfinite(bindWeight)
				|| clip.segments.empty()
				|| clip.segments.size()
					> kGpuAnimationMaximumTrackSegmentsV1) {
				return false;
			}
			std::vector<GpuAnimationTrackDirectoryEntry> entries;
			entries.reserve(clip.segments.size());
			for(std::size_t segmentIndex = 0U;
				segmentIndex < clip.segments.size(); ++segmentIndex) {
				const AnimationClipSegment& segment =
					clip.segments[segmentIndex];
				if(segment.startTick < 0
					|| segment.endTick < segment.startTick
					|| static_cast<std::uint64_t>(segment.endTick)
						> std::numeric_limits<std::uint32_t>::max()) {
					return false;
				}
				const auto found = std::find_if(
					clip.tracks.begin(), clip.tracks.end(),
					[&](const AnimationClipTrack& track) {
						return track.segmentIndex == segmentIndex
							&& track.stableTargetId == stableTargetId
							&& track.kind
								== AnimationClipTrackKind::MorphWeight;
					});
				std::uint32_t trackOffset = 0U;
				if(found == clip.tracks.end()) {
					// A mesh target omitted by this clip remains at its bind
					// weight while preserving the mesh's stable target order.
					if(!TryAppendGpuConstantScalarTrack(
						clipBytes, segment, bindWeight, trackOffset)) {
						return false;
					}
				} else {
					if(found->sampleOffset
							> std::numeric_limits<std::uint32_t>::max()
						|| found->sampleOffset > clipBytes.size()
						|| sizeof(AnimationCodecTrackHeaderV1)
							> clipBytes.size()
								- static_cast<std::size_t>(
									found->sampleOffset)) {
						return false;
					}
					AnimationCodecTrackHeaderV1 header{};
					std::memcpy(
						&header,
						clipBytes.data()
							+ static_cast<std::size_t>(
								found->sampleOffset),
						sizeof(header));
					const bool constant =
						(header.flags
							& kAnimationCodecConstantTrack) != 0U;
					if(header.encoding
							!= AnimationCodecValueEncoding::ScalarUnorm16
						|| header.segmentStartTick != segment.startTick
						|| header.segmentEndTick != segment.endTick
						|| header.byteSize < sizeof(AnimationCodecTrackHeaderV1)
						|| header.byteSize > clipBytes.size()
							- static_cast<std::size_t>(found->sampleOffset)
						|| (!constant
							&& found->interpolation
								!= AnimationInterpolation::Linear)) {
						return false;
					}
					if(found->sampleOffset % alignof(std::uint32_t) == 0U) {
						trackOffset = static_cast<std::uint32_t>(
							found->sampleOffset);
					} else {
						// Apply the same legacy uint16-packed compatibility path
						// used by skeletal channels before GPU morph sampling.
						std::vector<std::uint8_t> alignedTrack(header.byteSize);
						std::memcpy(
							alignedTrack.data(),
							clipBytes.data()
								+ static_cast<std::size_t>(found->sampleOffset),
							alignedTrack.size());
						if(!TryAlignGpuPayload(
								clipBytes, alignof(std::uint32_t))
							|| alignedTrack.size()
								> kMaximumGpuBindingPayloadBytes
									- clipBytes.size()) {
							return false;
						}
						trackOffset =
							static_cast<std::uint32_t>(clipBytes.size());
						clipBytes.insert(
							clipBytes.end(),
							alignedTrack.begin(), alignedTrack.end());
					}
				}
				entries.push_back({
					static_cast<std::uint32_t>(segment.startTick),
					static_cast<std::uint32_t>(segment.endTick),
					trackOffset,
					0U});
			}

			if(!TryAlignGpuPayload(clipBytes, 16U)
				|| clipBytes.size()
					> std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			offset = static_cast<std::uint32_t>(clipBytes.size());
			GpuAnimationTrackDirectoryHeader header;
			header.entryCount =
				static_cast<std::uint32_t>(entries.size());
			if(!TryAppendGpuValue(clipBytes, header)) return false;
			for(const GpuAnimationTrackDirectoryEntry& entry : entries) {
				if(!TryAppendGpuValue(clipBytes, entry)) return false;
			}
			return true;
		}

		[[nodiscard]] bool ProgramHasOneGpuSafeClip(
			const AnimatorPlaybackBinding& binding) noexcept
		{
			if(!binding.evaluationProgram || !binding.clip || !binding.skeleton
				|| binding.controllerClips.size() != 1U
				|| binding.controllerClips.front().clip != binding.clip
				|| binding.evaluationProgram->instructions.size() != 1U
				|| binding.evaluationProgram->states.size() != 1U
				|| !binding.evaluationProgram->transitions.empty()
				|| !binding.evaluationProgram->blendNodes.empty()
				|| binding.evaluationProgram->layers.size() > 1U) {
				return false;
			}
			const EvaluationInstruction& instruction =
				binding.evaluationProgram->instructions.front();
			const EvaluationState& state = binding.evaluationProgram->states.front();
			const EvaluationLayer* rootLayer =
				binding.evaluationProgram->layers.empty()
					? nullptr : &binding.evaluationProgram->layers.front();
			if(instruction.opcode != EvaluationOpcode::SampleClip
				|| instruction.dependencyAssetId != binding.clip->assetId
				|| instruction.dependencyFingerprint
					!= binding.clip->compatibilityFingerprint
				|| (instruction.dependencyGeneration != 0U
					&& instruction.dependencyGeneration != binding.clipGeneration)
				|| state.nodeOpcode != EvaluationOpcode::SampleClip
				|| state.nodeId != instruction.sourceStableId
				|| binding.controllerClips.front().nodeStableId != state.nodeId
				|| (rootLayer != nullptr
					&& (rootLayer->rootStateId != state.stableId
						|| rootLayer->maskAssetId.IsValid()
						|| rootLayer->maskGeneration != 0U
						|| rootLayer->maskFingerprint != 0U
						|| rootLayer->additive
						|| !std::isfinite(rootLayer->defaultWeight)
						|| std::abs(rootLayer->defaultWeight - 1.0F)
							> 1.0e-6F))) {
				return false;
			}
			if(!binding.evaluationProgram->layers.empty()) {
				const EvaluationLayer& layer = binding.evaluationProgram->layers.front();
				if(layer.additive || layer.maskAssetId.IsValid()
					|| layer.defaultWeight != 1.0F) {
					return false;
				}
			}
			return true;
		}

		void PopulateBindingNamedOutputs(AnimatorPlaybackBinding& binding)
		{
			std::vector<std::uint64_t> morphs;
			std::vector<std::uint64_t> curves;
			const auto append = [&](const std::shared_ptr<const AnimationClipAsset>& clip) {
				if(!clip) return;
				for(const AnimationClipTrack& track : clip->tracks) {
					if(track.kind == AnimationClipTrackKind::MorphWeight) {
						morphs.push_back(track.stableTargetId);
					} else if(track.kind == AnimationClipTrackKind::Curve) {
						curves.push_back(track.stableTargetId);
					}
				}
			};
			append(binding.clip);
			for(const AnimatorControllerClipBinding& clip : binding.controllerClips) {
				append(clip.clip);
			}
			std::sort(morphs.begin(), morphs.end());
			morphs.erase(std::unique(morphs.begin(), morphs.end()), morphs.end());
			std::sort(curves.begin(), curves.end());
			curves.erase(std::unique(curves.begin(), curves.end()), curves.end());
			binding.morphStableIds = std::move(morphs);
			binding.curveStableIds = std::move(curves);
		}

		[[nodiscard]] std::uint64_t MakeDirectClipStableId(
			const AnimationClipAsset& clip,
			const SkeletonAsset& skeleton,
			AnimationGeneration clipGeneration) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			MixRenderIdentityUuid(hash, clip.assetId.value);
			MixRenderIdentityUuid(hash, skeleton.assetId.value);
			MixRenderIdentityValue(hash, clip.compatibilityFingerprint);
			MixRenderIdentityValue(hash, clipGeneration);
			return hash == 0U ? 1U : hash;
		}

		[[nodiscard]] bool EnsureDirectClipEvaluationProgram(
			AnimatorPlaybackBinding& binding)
		{
			if(binding.controllerProgramRequired || binding.evaluationProgram
				|| !binding.clip || !binding.skeleton
				|| binding.clipGeneration == 0U) {
				return binding.evaluationProgram != nullptr;
			}

			EvaluationProgram program;
			for(std::size_t index = 0U;
				index < program.controllerAssetId.value.size(); ++index) {
				program.controllerAssetId.value[index] = static_cast<std::uint8_t>(
					binding.clip->assetId.value[index]
					^ binding.skeleton->assetId.value[
						program.controllerAssetId.value.size() - 1U - index]
					^ static_cast<std::uint8_t>(0x5dU + index));
			}
			if(!program.controllerAssetId.IsValid()) {
				program.controllerAssetId.value.front() = 1U;
			}
			program.compatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
			program.scratchPlan.byteCount = 64U;
			program.scratchPlan.alignmentBytes = 16U;

			const std::uint64_t nodeStableId = MakeDirectClipStableId(
				*binding.clip, *binding.skeleton, binding.clipGeneration);
			EvaluationInstruction instruction;
			instruction.opcode = EvaluationOpcode::SampleClip;
			instruction.sourceStableId = nodeStableId;
			instruction.dependencyAssetId = binding.clip->assetId;
			instruction.dependencyGeneration = binding.clipGeneration;
			instruction.dependencyFingerprint =
				binding.clip->compatibilityFingerprint;
			instruction.scratchOffsetBytes = 0U;
			program.instructions.push_back(instruction);
			program.bytecode.push_back(
				static_cast<std::uint8_t>(EvaluationOpcode::SampleClip));

			EvaluationState state;
			state.stableId = nodeStableId;
			state.entry = true;
			state.nodeId = nodeStableId;
			state.nodeOpcode = EvaluationOpcode::SampleClip;
			program.states.push_back(state);

			EvaluationLayer layer;
			layer.stableId = nodeStableId ^ 0xa11ce5eedULL;
			if(layer.stableId == 0U) layer.stableId = 1U;
			layer.rootStateId = nodeStableId;
			layer.defaultWeight = 1.0F;
			program.layers.push_back(layer);
			program.stableHash = ComputeEvaluationProgramStableHash(program);
			EvaluationDispatchState validation;
			if(DispatchEvaluationProgramCpu(program, validation)
					!= AnimationStatus::Ok
				|| validation.dispatchedInstructionCount != 1U) {
				return false;
			}

			AnimatorControllerClipBinding clipBinding;
			clipBinding.nodeStableId = nodeStableId;
			clipBinding.clip = binding.clip;
			clipBinding.residentSegments = binding.residentSegments;
			clipBinding.clipGeneration = binding.clipGeneration;
			clipBinding.streamedSegmentPayloads =
				binding.streamedSegmentPayloads;
			binding.controllerClips.push_back(std::move(clipBinding));
			binding.evaluationProgram =
				std::make_shared<const EvaluationProgram>(std::move(program));
			binding.programGeneration = binding.clipGeneration;
			binding.stateId = nodeStableId;
			return true;
		}

		void ConfigureGpuResidentBinding(
			AnimatorPlaybackBinding& binding,
			const GpuBindingRenderIdentity& renderIdentity) noexcept
		{
			// Stage into private values first. Any malformed asset, unsupported ABI
			// shape, or allocation failure leaves the binding on the complete CPU path.
			binding.gpuExecution.resourcesResident = false;
			binding.gpuExecution.evaluationHealthy = true;
			binding.gpuExecution.requiredBudget = 0.0;
			binding.gpuProgramResourceIndex = kGpuAnimationInvalidResourceIndex;
			binding.gpuVisual = {};
			binding.gpuResidentPayloads.reset();
			try {
				EvaluationDispatchState dispatchValidation;
				if(!renderIdentity.valid
					|| !ProgramHasOneGpuSafeClip(binding)
					|| binding.retargetProfile || binding.retargetSourceSkeleton
					|| binding.morphStableIds
						!= renderIdentity.morphStableIds
					|| binding.evaluationProgram->compatibilityFingerprint
						!= binding.skeleton->compatibilityFingerprint
					|| binding.clip->compatibilityFingerprint
						!= binding.skeleton->compatibilityFingerprint
					|| binding.clip->skeletonAssetId != binding.skeleton->assetId
					|| DispatchEvaluationProgramCpu(
						*binding.evaluationProgram, dispatchValidation)
						!= AnimationStatus::Ok) {
					return;
				}

				GpuAnimationResidentPayloads payloads;
				if(!CopyGpuClipBytes(binding, payloads.packedClipBytes)) return;
				GpuAnimationMorphTableHeader morphHeader;
				morphHeader.morphCount = static_cast<std::uint32_t>(
					binding.morphStableIds.size());
				GpuAnimationMorphBoundsRecord morphBounds;
				for(std::size_t lane = 0U; lane < 3U; ++lane) {
					morphBounds.minimum[lane] =
						renderIdentity.deformationBoundsMinimum[lane];
					morphBounds.maximum[lane] =
						renderIdentity.deformationBoundsMaximum[lane];
				}
				if(!TryAppendGpuValue(
						payloads.morphInputs, morphHeader)
					|| !TryAppendGpuValue(
						payloads.morphInputs, morphBounds)) {
					return;
				}
				for(const std::uint64_t morphStableId
					: binding.morphStableIds) {
					GpuAnimationMorphInputRecord record;
					if(!TryBuildGpuMorphTrackDirectory(
							*binding.clip,
							morphStableId,
							0.0F,
							payloads.packedClipBytes,
							record.trackLocatorOffsetBytes)
						|| !TryAppendGpuValue(
							payloads.morphInputs, record)) {
						return;
					}
				}
				std::uint64_t topologyId = 0U;
				if(!BuildGpuSkeletonTable(
					*binding.skeleton, renderIdentity.paletteJoints,
					payloads.skeletonTables, topologyId)) {
					return;
				}

				std::size_t programBytes = kGpuProgramHeaderBytes;
				if(binding.skeleton->joints.size()
					> (kMaximumGpuBindingPayloadBytes - programBytes)
						/ kGpuProgramJointRecordBytes) {
					return;
				}
				programBytes += binding.skeleton->joints.size()
					* kGpuProgramJointRecordBytes;
				std::vector<std::uint8_t> packedProgram(programBytes, 0U);
				for(std::size_t jointIndex = 0U;
					jointIndex < binding.skeleton->joints.size(); ++jointIndex) {
					const SkeletonJoint& joint = binding.skeleton->joints[jointIndex];
					std::array<std::uint32_t, 4U> record{};
					if(!TryBuildGpuTrackDirectory(
							*binding.clip, joint, AnimationClipTrackKind::Translation,
							payloads.packedClipBytes, record[0U])
						|| !TryBuildGpuTrackDirectory(
							*binding.clip, joint, AnimationClipTrackKind::Rotation,
							payloads.packedClipBytes, record[1U])
						|| !TryBuildGpuTrackDirectory(
							*binding.clip, joint, AnimationClipTrackKind::Scale,
							payloads.packedClipBytes, record[2U])) {
						return;
					}
					std::memcpy(
						packedProgram.data() + kGpuProgramHeaderBytes
							+ jointIndex * kGpuProgramJointRecordBytes,
						record.data(),
						sizeof(record));
				}
				if(payloads.packedClipBytes.empty()
					|| payloads.packedClipBytes.size()
						> kMaximumGpuBindingPayloadBytes
					|| packedProgram.size() % sizeof(std::uint32_t) != 0U) {
					return;
				}
				payloads.programWords.resize(
					packedProgram.size() / sizeof(std::uint32_t));
				std::memcpy(
					payloads.programWords.data(),
					packedProgram.data(),
					packedProgram.size());
				if(!payloads.HasRequiredSampleAndHierarchyInputs()) return;

				const std::size_t totalBytes =
					payloads.programWords.size() * sizeof(std::uint32_t)
					+ payloads.packedClipBytes.size()
					+ payloads.skeletonTables.size()
					+ payloads.morphInputs.size();
				if(totalBytes > kMaximumGpuBindingPayloadBytes) return;
				auto immutablePayloads =
					std::make_shared<const GpuAnimationResidentPayloads>(
						std::move(payloads));

				GpuAnimationVisualInput visual;
				visual.skeletonTopologyId = topologyId;
				visual.compatibilityFingerprint =
					binding.skeleton->compatibilityFingerprint;
				visual.clipResourceIndex = 0U;
				visual.skeletonResourceIndex = 0U;
				visual.maskResourceIndex = kGpuAnimationInvalidResourceIndex;
				visual.morphResourceIndex = 0U;
				visual.jointCount =
					static_cast<std::uint32_t>(binding.skeleton->joints.size());
				visual.morphCount = static_cast<std::uint32_t>(
					binding.morphStableIds.size());
				visual.paletteEntryCount = static_cast<std::uint32_t>(
					renderIdentity.paletteJoints.size());
				visual.palettePartitionCount = 1U;
				visual.looping = binding.playback.loop;
				visual.reverse = binding.playback.reverse;
				visual.hasRootMotion = binding.clip->rootMotion.enabled;
				if(binding.evaluationGenerations.residencyGeneration == 0U
					|| binding.evaluationGenerations.residencyGeneration
						> std::numeric_limits<std::uint32_t>::max()) {
					return;
				}
				visual.generations.resourceGeneration =
					static_cast<std::uint32_t>(
						binding.evaluationGenerations.residencyGeneration);
				// Keep the exact 64-bit render identity beside the frozen shader ABI.
				// Render collection validates this copied record before it lets a draw
				// consume the packet; no payload hash or UUID is narrowed to uint32.
				visual.renderIdentity.meshId = renderIdentity.meshId;
				visual.renderIdentity.meshHeaderVersion =
					renderIdentity.meshHeaderVersion;
				visual.renderIdentity.deformablePayloadGeneration =
					renderIdentity.deformablePayloadGeneration;
				visual.renderIdentity.rendererGeneration =
					renderIdentity.rendererSessionGeneration;
				visual.renderIdentity.sceneGeneration =
					renderIdentity.sceneGeneration;
				visual.renderIdentity.skeletonCompatibilityFingerprint =
					renderIdentity.skeletonCompatibilityFingerprint;

				binding.gpuProgramResourceIndex = 0U;
				binding.gpuVisual = visual;
				binding.gpuResidentPayloads = std::move(immutablePayloads);
				binding.gpuExecution.requiredBudget =
					static_cast<double>(totalBytes);
				binding.gpuExecution.resourcesResident = true;
			} catch(const std::bad_alloc&) {
				// The default state above is a complete, atomic CPU fallback.
			} catch(...) {
				// Asset-driven conversion is fail-closed even for unexpected library
				// exceptions; no partially built descriptor data escapes this function.
			}
		}

		[[nodiscard]] AnimatorPlaybackBinding BuildPlaybackBinding(
			const AnimatorComponent& component,
			ResourceManager& resources,
			AuthoredBindingSignature& signature,
			const ControllerProgramState& controllerState,
			const GpuBindingRenderIdentity& renderIdentity,
			ClipStreamingRuntime& streaming,
			ClipStreamingBindingState& streamingState)
		{
			AnimatorPlaybackBinding binding;
			binding.controllerProgramRequired = component.controllerAssetId != EMPTY_UUID;
			binding.controllerProgramStatus = binding.controllerProgramRequired
				? (controllerState.pending ? AnimationStatus::NotReady : controllerState.status)
				: AnimationStatus::Ok;
			const AnimationControllerAsset* controller = controllerState.publishedController
				? &*controllerState.publishedController
				: (controllerState.latestController ? &*controllerState.latestController : nullptr);
			if(controllerState.publishedProgram && controllerState.publishedProgram->program) {
				binding.evaluationProgram = controllerState.publishedProgram->program;
				binding.programGeneration = controllerState.publishedProgram->sourceGeneration;
				binding.controllerProgramStatus = AnimationStatus::Ok;
			}
			binding.requestedExecutionPolicy = component.requestedMode;
			binding.rootMotionPolicy = component.rootMotionPolicy;
			binding.updatePolicy = component.updatePolicy;
			binding.eventPolicy = component.eventPolicy;
			binding.cullingPolicy = component.cullingPolicy;
			binding.playback.timePolicy = component.timePolicy;
			// Parsing occurs inside AnimationSystem's atomic bind path. Keeping the
			// authored text copied here prevents worker code from reading the component.
			binding.initialParametersJson = component.initialParametersJson;
			// Preserve a delete/reload tombstone in the safe-pose publication. A
			// never-published authored reference uses generation one only to satisfy
			// AnimationSystem's nonzero binding contract; its signature remains zero
			// and will rebind as soon as ResourceManager publishes the first payload.
			binding.skeletonGeneration = signature.skeletonRuntimeGeneration == 0U
				? 1U : signature.skeletonRuntimeGeneration;
			binding.clipGeneration = signature.defaultClipRuntimeGeneration == 0U
				? 1U : signature.defaultClipRuntimeGeneration;
			binding.retargetProfileGeneration = signature.retargetProfileRuntimeGeneration;
			binding.motionDatabaseGeneration = signature.motionDatabaseRuntimeGeneration;
			binding.ragdollProfileGeneration = signature.ragdollProfileRuntimeGeneration;
			// Events, extracted/applied root motion, fixed-step simulation, and an
			// explicit always-update policy remain authoritative while visual work
			// is culled. Recording/replication have no authored component flags yet;
			// their future producers can promote the same retained request path.
			const bool gameplayCritical = component.rootMotionPolicy
					!= AnimatorRootMotionPolicy::Ignore
				|| component.eventPolicy != AnimatorEventPolicy::Suppress
				|| component.updatePolicy == AnimatorUpdatePolicy::FixedStep
				|| component.cullingPolicy == AnimatorCullingPolicy::AlwaysUpdate;

			// Copy one payload/generation pair while ResourceManager holds its cache
			// read lock. This runs only after the cheap signature changes, keeping
			// unchanged animator frames free of deep animation asset copies.
			UUID resolvedSkeletonAssetId = component.skeletonOverrideAssetId;
			if(resolvedSkeletonAssetId == EMPTY_UUID && controller != nullptr) {
				resolvedSkeletonAssetId = controller->skeletonAssetId.value;
			}
			if(resolvedSkeletonAssetId != EMPTY_UUID) {
				SkeletonAsset skeleton;
				AnimationGeneration generation = 0U;
				if(resources.CopySkeletonPublication(resolvedSkeletonAssetId, skeleton, generation)) {
					binding.skeleton = std::make_shared<SkeletonAsset>(std::move(skeleton));
					binding.skeletonGeneration = generation;
					signature.skeletonRuntimeGeneration = generation;
				}
			}
			if(component.defaultClipAssetId != EMPTY_UUID) {
				AnimationClipAsset clip;
				AnimationGeneration generation = 0U;
				if(resources.CopyAnimationClipPublication(component.defaultClipAssetId, clip, generation)) {
					binding.clip = std::make_shared<AnimationClipAsset>(std::move(clip));
					binding.clipGeneration = generation;
					signature.defaultClipRuntimeGeneration = generation;
					ResolvedClipStreamingPayloads streamed = ResolveClipSegmentResidency(
						*binding.clip, generation, gameplayCritical,
						resources, streaming, streamingState);
					binding.residentSegments = std::move(streamed.residentSegments);
					binding.streamedSegmentPayloads = std::move(streamed.payloads);
				}
			}
			if(controller != nullptr && binding.evaluationProgram) {
				const EvaluationState* entry = ResolveEntryLeaf(*binding.evaluationProgram);
				if(entry != nullptr) binding.stateId = entry->stableId;
				for(const AnimationControllerNode& node : controller->nodes) {
					if(node.kind != AnimationControllerNodeKind::Clip
						|| !node.dependencyAssetId.IsValid()) continue;
					AnimationClipAsset clip;
					AnimationGeneration generation = 0U;
					if(!resources.CopyAnimationClipPublication(
						node.dependencyAssetId.value, clip, generation)) continue;
					AnimatorControllerClipBinding nodeBinding;
					nodeBinding.nodeStableId = node.stableId;
					nodeBinding.clip = std::make_shared<AnimationClipAsset>(std::move(clip));
					nodeBinding.clipGeneration = generation;
					ResolvedClipStreamingPayloads streamed = ResolveClipSegmentResidency(
						*nodeBinding.clip, generation, gameplayCritical,
						resources, streaming, streamingState);
					nodeBinding.residentSegments = std::move(streamed.residentSegments);
					nodeBinding.streamedSegmentPayloads = std::move(streamed.payloads);
					const bool isEntryClip = entry != nullptr && entry->nodeId == node.stableId;
					if(isEntryClip) {
						binding.clip = nodeBinding.clip;
						binding.clipGeneration = nodeBinding.clipGeneration;
						binding.residentSegments = nodeBinding.residentSegments;
						binding.streamedSegmentPayloads =
							nodeBinding.streamedSegmentPayloads;
					}
					binding.controllerClips.push_back(std::move(nodeBinding));
				}
				std::sort(binding.controllerClips.begin(), binding.controllerClips.end(),
					[](const auto& left, const auto& right) {
						return left.nodeStableId < right.nodeStableId;
					});
			}
			if(component.retargetProfileOverrideAssetId != EMPTY_UUID) {
				RetargetProfileAsset profile;
				AnimationGeneration generation = 0U;
				if(resources.CopyRetargetProfilePublication(
					component.retargetProfileOverrideAssetId, profile, generation)) {
					binding.retargetProfile =
						std::make_shared<RetargetProfileAsset>(std::move(profile));
					binding.retargetProfileGeneration = generation;
					signature.retargetProfileRuntimeGeneration = generation;
				}
			}
			if(component.motionDatabaseOverrideAssetId != EMPTY_UUID) {
				MotionDatabaseAsset database;
				AnimationGeneration generation = 0U;
				if(resources.CopyMotionDatabasePublication(
					component.motionDatabaseOverrideAssetId, database, generation)) {
					binding.motionDatabaseGeneration = generation;
					signature.motionDatabaseRuntimeGeneration = generation;
					binding.motionDatabase = ResolveMotionDatabaseStreaming(
						std::move(database), generation, gameplayCritical,
						resources, streaming, streamingState);
				}
			}
			if(component.ragdollProfileOverrideAssetId != EMPTY_UUID) {
				RagdollProfileAsset profile;
				AnimationGeneration generation = 0U;
				if(resources.CopyRagdollProfilePublication(
					component.ragdollProfileOverrideAssetId, profile, generation)) {
					binding.ragdollProfile =
						std::make_shared<RagdollProfileAsset>(std::move(profile));
					binding.ragdollProfileGeneration = generation;
					signature.ragdollProfileRuntimeGeneration = generation;
				}
			}
			if(binding.retargetProfile
				&& binding.retargetProfile->sourceSkeletonAssetId.value != EMPTY_UUID) {
				signature.retargetSourceSkeletonAssetId =
					binding.retargetProfile->sourceSkeletonAssetId.value;
				SkeletonAsset sourceSkeleton;
				AnimationGeneration generation = 0U;
				if(resources.CopySkeletonPublication(
					binding.retargetProfile->sourceSkeletonAssetId.value,
					sourceSkeleton, generation)) {
					binding.retargetSourceSkeleton =
						std::make_shared<SkeletonAsset>(std::move(sourceSkeleton));
					binding.retargetSourceSkeletonGeneration = generation;
					signature.retargetSourceSkeletonRuntimeGeneration = generation;
				}
			}
			// A never-published dependency still needs a valid binding generation so
			// AnimationSystem can expose its explicit MissingAsset safe-pose state.

			if(binding.clip) {
				binding.playback.durationTicks = binding.clip->durationTicks;
				// This component-level policy follows the active playback clip. A
				// controller transition still uses its independent exit time and
				// crossfade duration, both of which advance on the unwrapped clock.
				binding.playback.loop = component.loopDefaultClip;
				// The component stores an easy-to-author unsigned frame, while the
				// runtime clock uses signed fixed ticks. Clamp against the resolved
				// entry/direct clip so shortening either clip during reimport is safe.
				binding.initialPlaybackTick = ClampAnimatorInitialPlaybackTick(
					static_cast<AnimationFixedTick>(component.defaultClipStartFrame),
					binding.playback.durationTicks,
					binding.playback.loop);
			}
			binding.evaluationGenerations.residencyGeneration = streaming.snapshot
				? std::max<std::uint64_t>(1U, streaming.snapshot->publicationSequence) : 1U;
			if(binding.skeleton) {
				binding.jointStableIds.reserve(binding.skeleton->joints.size());
				binding.jointParents.reserve(binding.skeleton->joints.size());
				for(const SkeletonJoint& joint : binding.skeleton->joints) {
					binding.jointStableIds.push_back(joint.stableId);
					binding.jointParents.push_back(joint.parentIndex);
				}
			}

			// Direct-clip animators use the same immutable single-node program shape
			// as authored controllers. This keeps CPU behavior deterministic while
			// also giving the GPU planner a real program identity and dependency.
			(void)EnsureDirectClipEvaluationProgram(binding);
			PopulateBindingNamedOutputs(binding);
			if(renderIdentity.valid) {
				// GPU output lanes follow the deformable mesh target order. Add
				// targets omitted by this clip so both CPU fallback and GPU visual
				// publication keep positional target identity stable at zero.
				binding.morphStableIds.insert(
					binding.morphStableIds.end(),
					renderIdentity.morphStableIds.begin(),
					renderIdentity.morphStableIds.end());
				std::sort(
					binding.morphStableIds.begin(),
					binding.morphStableIds.end());
				binding.morphStableIds.erase(
					std::unique(
						binding.morphStableIds.begin(),
						binding.morphStableIds.end()),
					binding.morphStableIds.end());
			}
			ConfigureGpuResidentBinding(binding, renderIdentity);
			return binding;
		}
	}

	struct AnimatorComponentBindingCoordinator::Impl {
		struct Entry {
			AnimatorInstanceHandle handle;
			std::uint64_t objectLifetimeGeneration = 0U;
			AnimationGeneration componentGeneration = 0U;
			AuthoredBindingSignature signature{};
			ControllerProgramState controllerProgram;
			std::vector<RuntimeStreamingDependency> streamingDependencies;
			std::vector<AnimationStreamRequest> retainedClipStreams;
			AnimationStatus lastBindingStatus = AnimationStatus::NotReady;
			bool clipStreamingPending = false;
			bool motionDatabaseStreamed = false;
			bool motionDatabaseFallback = false;
		};

		std::map<UUID, Entry> entries;
		std::map<RuntimeStreamingDependency, std::shared_future<UUID>> pendingStreamingLoads;
		ClipStreamingRuntime clipStreaming;
		ResourceManager* resources = nullptr;
		std::uint64_t synchronizationFrame = 0U;
		bool playbackRestartRequested = false;
	};

	AnimatorComponentBindingCoordinator::AnimatorComponentBindingCoordinator()
		: impl_(std::make_unique<Impl>())
	{
	}

	AnimatorComponentBindingCoordinator::~AnimatorComponentBindingCoordinator() = default;

	AnimatorComponentSynchronizationSummary AnimatorComponentBindingCoordinator::Synchronize(
		AnimationSystem& animation,
		ObjectManager& objects,
		ResourceManager& resources) noexcept
	{
		AnimatorComponentSynchronizationSummary summary;
		try {
			impl_->resources = &resources;
			if(impl_->synchronizationFrame < std::numeric_limits<std::uint64_t>::max()) {
				++impl_->synchronizationFrame;
			}
			const std::uint64_t streamingFrame = impl_->synchronizationFrame;
			const bool playbackRestartRequested =
				impl_->playbackRestartRequested;
			std::size_t issuedStreamingRequests = 0U;
			const bool clipPublicationChanged = PollClipStreamReads(impl_->clipStreaming);
			PruneCompletedStreamingLoads(impl_->pendingStreamingLoads);
			const std::vector<Object> authoredObjects =
				objects.SnapshotObjectsWithComponents<AnimatorComponent>();
			summary.authoredActiveCount = authoredObjects.size();
			std::set<UUID> seen;
			std::set<ClipStreamPayloadKey> desiredClipPayloads;

			for(const Object& object : authoredObjects) {
				const UUID ownerObjectId = objects.getUUID(object);
				const AnimatorComponent* component = objects.GetComponent<AnimatorComponent>(object);
				const std::uint64_t lifetimeGeneration = objects.getObjectLifetimeGeneration(object);
				const AnimationGeneration componentGeneration =
					CompressObjectGeneration(objects.getObjectComponentGeneration(object));
				if(ownerObjectId == EMPTY_UUID || component == nullptr
					|| lifetimeGeneration == 0U || componentGeneration == 0U) {
					++summary.failedCount;
					continue;
				}
				seen.insert(ownerObjectId);
				AuthoredBindingSignature signature = MakeSignature(*component, resources);
				auto existing = impl_->entries.find(ownerObjectId);
				std::optional<ControllerProgramState> retainedControllerProgram;
				const bool identityChanged = existing != impl_->entries.end()
					&& (existing->second.objectLifetimeGeneration != lifetimeGeneration
						|| existing->second.componentGeneration != componentGeneration
						// A controller is part of the instance's durable descriptor and
						// event identity, so changing it must retire the old handle.
						|| existing->second.signature.controllerAssetId != signature.controllerAssetId);
				if(identityChanged || (existing != impl_->entries.end()
					&& !animation.IsAnimatorInstanceAlive(existing->second.handle))) {
					if(identityChanged
						&& signature.controllerAssetId != EMPTY_UUID
						&& existing->second.signature.controllerAssetId
							== signature.controllerAssetId
						&& existing->second.controllerProgram.assetId
							== signature.controllerAssetId) {
						// Stop restores the authored scene atomically, which replaces
						// component generations even when the durable object UUID and
						// controller asset are identical. Transfer the immutable compiled
						// controller to the replacement entry so its first evaluation can
						// publish the requested start frame instead of a NotReady pose.
						retainedControllerProgram.emplace(
							std::move(existing->second.controllerProgram));
					}
					ReleaseClipStreamRetains(
						impl_->clipStreaming, existing->second.retainedClipStreams);
					(void)resources.ReleaseStreamingNeedsForObject(
						ownerObjectId,
						TerraEngine::Resource::StreamingNeedType::RuntimeExplicit);
					if(animation.InvalidateAnimatorInstance(existing->second.handle)) {
						++summary.invalidatedCount;
					}
					impl_->entries.erase(existing);
					existing = impl_->entries.end();
				}

				bool createdNow = false;
				if(existing == impl_->entries.end()) {
					AnimatorInstanceDescriptor descriptor;
					descriptor.ownerObjectId = ownerObjectId;
					descriptor.componentGeneration = componentGeneration;
					descriptor.controller = MakeAuthoredAssetHandle(
						component->controllerAssetId, signature.controllerRuntimeGeneration);
					const AnimatorInstanceHandle handle = animation.CreateAnimatorInstance(descriptor);
					if(!handle.IsValid()) {
						++summary.failedCount;
						continue;
					}
					Impl::Entry entry;
					entry.handle = handle;
					entry.objectLifetimeGeneration = lifetimeGeneration;
					entry.componentGeneration = componentGeneration;
					entry.signature = signature;
					if(retainedControllerProgram) {
						entry.controllerProgram =
							std::move(*retainedControllerProgram);
					}
					try {
						existing = impl_->entries.emplace(ownerObjectId, std::move(entry)).first;
					} catch(...) {
						// Creating the runtime handle and recording its authored owner form one
						// protected operation. Roll the handle back if map allocation fails so
						// shutdown and later synchronization never inherit an orphan instance.
						(void)animation.InvalidateAnimatorInstance(handle);
						++summary.failedCount;
						continue;
					}
					++summary.createdCount;
					createdNow = true;
				}

				bool controllerProgramChanged = false;
				(void)RefreshControllerProgram(*component, resources, signature,
					existing->second.controllerProgram, controllerProgramChanged);
				const AnimationControllerAsset* resolvedController =
					existing->second.controllerProgram.publishedController
						? &*existing->second.controllerProgram.publishedController
						: (existing->second.controllerProgram.latestController
							? &*existing->second.controllerProgram.latestController : nullptr);
				if(component->skeletonOverrideAssetId == EMPTY_UUID
					&& resolvedController != nullptr
					&& resolvedController->skeletonAssetId.IsValid()) {
					signature.skeletonRuntimeGeneration =
						resources.GetAnimationAssetRuntimeGeneration(
							resolvedController->skeletonAssetId.value, AssetType::Skeleton);
				}
				const UUID resolvedSkeletonAssetId =
					component->skeletonOverrideAssetId != EMPTY_UUID
						? component->skeletonOverrideAssetId
						: (resolvedController != nullptr
							&& resolvedController->skeletonAssetId.IsValid()
								? resolvedController->skeletonAssetId.value
								: EMPTY_UUID);
				signature.resolvedSkeletonAssetId = resolvedSkeletonAssetId;
				SkeletonAsset renderSkeleton;
				AnimationGeneration renderSkeletonGeneration = 0U;
				const SkeletonAsset* renderSkeletonView = nullptr;
				if(resolvedSkeletonAssetId != EMPTY_UUID
					&& resources.CopySkeletonPublication(
						resolvedSkeletonAssetId,
						renderSkeleton,
						renderSkeletonGeneration)) {
					signature.skeletonRuntimeGeneration =
						renderSkeletonGeneration;
					signature.resolvedSkeletonCompatibilityFingerprint =
						renderSkeleton.compatibilityFingerprint;
					if(renderSkeleton.joints.size()
						<= std::numeric_limits<std::uint32_t>::max()) {
						signature.resolvedSkeletonJointCount =
							static_cast<std::uint32_t>(
								renderSkeleton.joints.size());
						renderSkeletonView = &renderSkeleton;
					}
				}
				const GpuBindingRenderIdentity renderIdentity =
					ResolveGpuBindingRenderIdentity(
						ownerObjectId,
						renderSkeletonView,
						objects,
						resources,
						signature);
				std::vector<RuntimeStreamingDependency> streamingDependencies =
					CollectAuthoredStreamingDependencies(
						*component, resources, resolvedController);
				if(existing->second.streamingDependencies != streamingDependencies) {
					// Replace all needs owned by this component as one protected scene
					// synchronization step. Shared assets keep other owners' records.
					(void)resources.ReleaseStreamingNeedsForObject(
						ownerObjectId,
						TerraEngine::Resource::StreamingNeedType::RuntimeExplicit);
					existing->second.streamingDependencies = std::move(streamingDependencies);
				}
				PublishAndRequestStreamingDependencies(
					ownerObjectId,
					existing->second.streamingDependencies,
					streamingFrame,
					resources,
					impl_->pendingStreamingLoads,
					issuedStreamingRequests);
				const bool authoredValuesChanged = existing->second.signature != signature;
				const bool dependencyRetry = existing->second.lastBindingStatus == AnimationStatus::MissingAsset
					|| existing->second.lastBindingStatus == AnimationStatus::NotReady;
				if(createdNow || authoredValuesChanged || dependencyRetry
					|| playbackRestartRequested
					|| controllerProgramChanged || existing->second.controllerProgram.pending
					|| existing->second.clipStreamingPending
					|| (clipPublicationChanged && !existing->second.retainedClipStreams.empty())) {
					ClipStreamingBindingState clipStreamingState;
					AnimatorPlaybackBinding binding = BuildPlaybackBinding(
						*component, resources, signature, existing->second.controllerProgram,
						renderIdentity, impl_->clipStreaming, clipStreamingState);
					// Runtime worlds autoplay authored components, while the editor
					// deliberately leaves the same binding stopped outside Play Mode.
					// Rebinds inherit that domain policy so hot reload cannot restart an
					// animation after the user has stopped the scene.
					binding.playOnBind =
						animation.IsAuthoredComponentPlaybackEnabled()
						&& component->playAutomatically;
					const AnimationStatus status = animation.BindAnimatorPlayback(
						existing->second.handle,
						std::move(binding));
					existing->second.signature = signature;
					existing->second.lastBindingStatus = status;
					existing->second.clipStreamingPending = clipStreamingState.pending;
					existing->second.motionDatabaseStreamed =
						clipStreamingState.motionDatabaseStreamed;
					existing->second.motionDatabaseFallback =
						clipStreamingState.motionDatabaseFallback;
					ReconcileClipStreamRetains(impl_->clipStreaming,
						existing->second.retainedClipStreams,
						clipStreamingState.retainedRequests);
					if(status == AnimationStatus::Ok || status == AnimationStatus::MissingAsset
						|| status == AnimationStatus::IncompatibleAsset) {
						++summary.reboundCount;
					} else {
						++summary.failedCount;
					}
				}
				summary.streamedMotionDatabaseCount +=
					existing->second.motionDatabaseStreamed ? 1U : 0U;
				summary.fallbackMotionDatabaseCount +=
					existing->second.motionDatabaseFallback ? 1U : 0U;
				for(const AnimationStreamRequest& request
					: existing->second.retainedClipStreams) {
					desiredClipPayloads.insert(MakeClipStreamPayloadKey(request));
				}
			}

			for(auto iterator = impl_->entries.begin(); iterator != impl_->entries.end();) {
				if(seen.contains(iterator->first)) {
					++iterator;
					continue;
				}
				if(animation.InvalidateAnimatorInstance(iterator->second.handle)) {
					++summary.invalidatedCount;
				}
				ReleaseClipStreamRetains(
					impl_->clipStreaming, iterator->second.retainedClipStreams);
				(void)resources.ReleaseStreamingNeedsForObject(
					iterator->first,
					TerraEngine::Resource::StreamingNeedType::RuntimeExplicit);
				iterator = impl_->entries.erase(iterator);
			}
			// A rebind or component removal can retire a worker future before it is
			// ready. Signal its token here; any late bytes will fail generation/state
			// validation and can never enter the immutable residency snapshot.
			for(auto iterator = impl_->clipStreaming.pendingReads.begin();
				iterator != impl_->clipStreaming.pendingReads.end();) {
				if(desiredClipPayloads.contains(iterator->first)) {
					++iterator;
					continue;
				}
				(void)impl_->clipStreaming.coordinator.Cancel(
					iterator->second.decision.request);
				iterator = impl_->clipStreaming.pendingReads.erase(iterator);
			}
			// Only unretained generations participate in pressure eviction. Active
			// gameplay/event/root-motion dependencies remain protected by retainCount.
			(void)impl_->clipStreaming.coordinator.ApplyResidentBudget(
				64U * 1024U * 1024U);
			const AnimationStreamingDiagnostics streamingDiagnostics =
				impl_->clipStreaming.coordinator.BuildDiagnostics();
			summary.requestedStreamPayloadCount = streamingDiagnostics.requestedPayloads;
			summary.residentStreamPayloadCount = streamingDiagnostics.residentPayloads;
			summary.failedStreamPayloadCount = streamingDiagnostics.failedPayloads;
			summary.cancelledStreamPayloadCount = streamingDiagnostics.cancelledPayloads;
			for(const AnimationStreamDecision& decision
				: impl_->clipStreaming.coordinator.Snapshot()) {
				if(decision.retainCount != 0U) ++summary.retainedStreamPayloadCount;
			}
			// Consume the request only after every retained component entry has
			// reached this protected boundary. An exception leaves it set so the
			// next frame retries instead of silently keeping the old clock domain.
			impl_->playbackRestartRequested = false;
			summary.runtimeActiveCount = impl_->entries.size();
			return summary;
		} catch(const std::bad_alloc&) {
			++summary.failedCount;
			return summary;
		} catch(...) {
			++summary.failedCount;
			return summary;
		}
	}

	void AnimatorComponentBindingCoordinator::RequestPlaybackRestart() noexcept
	{
		// The coordinator is owned and called only by AnimationSystem's protected
		// coordinator thread, so this intent flag needs no cross-thread primitive.
		impl_->playbackRestartRequested = true;
	}

	void AnimatorComponentBindingCoordinator::InvalidateAll(AnimationSystem& animation) noexcept
	{
		for(auto& [ownerObjectId, entry] : impl_->entries) {
			if(impl_->resources != nullptr) {
				(void)impl_->resources->ReleaseStreamingNeedsForObject(
					ownerObjectId,
					TerraEngine::Resource::StreamingNeedType::RuntimeExplicit);
			}
			ReleaseClipStreamRetains(
				impl_->clipStreaming, entry.retainedClipStreams);
			(void)animation.InvalidateAnimatorInstance(entry.handle);
		}
		for(auto& [key, job] : impl_->clipStreaming.pendingReads) {
			(void)key;
			(void)impl_->clipStreaming.coordinator.Cancel(job.decision.request);
		}
		impl_->entries.clear();
		impl_->pendingStreamingLoads.clear();
		impl_->clipStreaming.pendingReads.clear();
		(void)impl_->clipStreaming.coordinator.ApplyResidentBudget(0U);
		impl_->clipStreaming.gameplayCriticalRetainers.clear();
		impl_->clipStreaming.clipRangeTables.clear();
		impl_->clipStreaming.motionPlans.clear();
		impl_->clipStreaming.snapshot =
			impl_->clipStreaming.coordinator.LoadResidencySnapshot();
		impl_->resources = nullptr;
		impl_->playbackRestartRequested = false;
	}
}
