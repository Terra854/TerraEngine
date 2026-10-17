#include "RenderSnapshotPreparation.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureDepthControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureShadowBatchControls.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureCameras.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyFixtureControls.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
// Protected frame collection. Query ordering and publication remain coordinator-owned.
#include "RenderFrameState.h"
#include "RenderFrameCapture.h"
#include <limits>
#include <cctype>
#include <cmath>
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Components/TerrainComponent.h"
#include "Components/AnimatorComponent.h"
#include "Scene/SceneManager.h"
#include "UI/UISystem.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include "Core/Logger/Logger.h"
#include "Profiling/ProfilerScope.h"

// Copies authored cameras only at the existing protected collection boundary.
void RenderSystem::CollectFrameCameraInputs(std::pmr::vector<CameraEntry>& cameras)
{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Cameras", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	objManager.forEachObjectWithComponents<Transform, CameraComponent>(
		[&](Object object, Transform&, CameraComponent& cameraComp)
		{
			CameraComponent reflectedCamera{};
			reflectedCamera.verticalFovDegrees = cameraComp.verticalFovDegrees;
			reflectedCamera.aspectRatio = cameraComp.aspectRatio;
			reflectedCamera.nearPlane = cameraComp.nearPlane;
			reflectedCamera.farPlane = cameraComp.farPlane;
			reflectedCamera.up = cameraComp.up;
			cameras.push_back({ object.EntityId(), reflectedCamera });
		}
	);
}

// Copies authored lights only at the existing protected collection boundary.
void RenderSystem::CollectFrameLightInputs(std::pmr::vector<LightEntry>& lights)
{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Lights", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	objManager.forEachObjectWithComponents<Transform, LightComponent>(
		[&](Object object, Transform&, LightComponent& lightComp)
		{
		LightComponent reflectedLight{};
		const std::uint32_t encodedType = static_cast<std::uint32_t>(lightComp.type);

		if (encodedType > static_cast<std::uint32_t>(LightType::Area))
		{
			// Unsupported legacy light types are intentionally skipped so they
			// are not silently remapped to another runtime behavior.
			return;
		}
		reflectedLight.type = static_cast<LightType>(encodedType);

		const std::uint32_t encodedUnit = static_cast<std::uint32_t>(lightComp.unit);
		reflectedLight.unit = static_cast<LightUnit>(encodedUnit);

		const std::uint32_t encodedAppearance = static_cast<std::uint32_t>(lightComp.appearance);
		reflectedLight.appearance = static_cast<LightAppearance>(encodedAppearance);

		reflectedLight.color = lightComp.color;
		reflectedLight.temperature = lightComp.temperature;
		reflectedLight.intensity = lightComp.intensity;
		reflectedLight.range = lightComp.range;
		reflectedLight.innerConeAngle = lightComp.innerConeAngle;
		reflectedLight.outerConeAngle = lightComp.outerConeAngle;
		reflectedLight.width = lightComp.width;
		reflectedLight.height = lightComp.height;
		reflectedLight.castsShadows = lightComp.castsShadows;
		reflectedLight.shadowStrength = lightComp.shadowStrength;
		reflectedLight.shadowBias = lightComp.shadowBias;
		reflectedLight.indirectMultiplier = lightComp.indirectMultiplier;
		reflectedLight.authoredPriority = lightComp.authoredPriority;
		reflectedLight.stabilityPreference = lightComp.stabilityPreference;

		lights.push_back({ object.EntityId(), objManager.getUUID(object), reflectedLight });
		}
	);
}

namespace TerraEngine::Renderer::Frame {
	// Resolve one exact GPU identity without consulting live animation state.
	std::optional<AnimationGpuRenderBinding> FindUniqueGpuAnimationIdentity(
		const RenderAnimationCaptureView& inputs, const AnimationRenderBindingRequest& request,
		std::size_t& ownerMappingHint)
	{
		const auto& immutableGpuAnimationPlan = inputs.immutableGpuAnimationPlan;
		const auto& gpuPresentation = inputs.gpuPresentation;
		const auto protectedRendererGeneration = inputs.protectedRendererGeneration;
		const auto protectedSceneGeneration = inputs.protectedSceneGeneration;

		if(immutableGpuAnimationPlan == nullptr || gpuPresentation == nullptr
			|| protectedRendererGeneration == 0U
			|| protectedRendererGeneration > std::numeric_limits<std::uint32_t>::max()
			|| protectedSceneGeneration == 0U
			|| protectedSceneGeneration > std::numeric_limits<std::uint32_t>::max()
			|| immutableGpuAnimationPlan->RenderIdentityCount()
				!= immutableGpuAnimationPlan->WorkPackets().size()
			|| !request.expectedGpuRenderIdentity.IsComplete()) {
			return std::nullopt;
		}

		std::optional<TerraEngine::Animation::GpuAnimationOwnerObjectMapping> ownerMapping;
		const auto& ownerMappings = gpuPresentation->OwnerObjectMappings();
		const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerBegin = nullptr;
		if(ownerMappingHint < ownerMappings.size()
			&& ownerMappings[ownerMappingHint].ownerObjectId
				== request.animatorObjectId)
		{
			ownerBegin = ownerMappings.data() + ownerMappingHint;
		}
		else
		{
			ownerBegin = gpuPresentation->FindFirstOwnerObjectMapping(
				request.animatorObjectId);
			ownerMappingHint = ownerBegin != nullptr
				? static_cast<std::size_t>(ownerBegin - ownerMappings.data())
				: std::numeric_limits<std::size_t>::max();
		}
		const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerEnd =
			ownerMappings.data() + ownerMappings.size();
		for(const TerraEngine::Animation::GpuAnimationOwnerObjectMapping* ownerIt =
				ownerBegin;
			ownerIt != nullptr && ownerIt != ownerEnd
				&& ownerIt->ownerObjectId == request.animatorObjectId; ++ownerIt) {
			const TerraEngine::Animation::GpuAnimationOwnerObjectMapping& mapping = *ownerIt;
			if(!mapping.IsComplete() || mapping.ownerObjectId != request.animatorObjectId
				|| mapping.componentGeneration != request.componentGeneration
				|| !mapping.HasPublishedPacketIndex()) {
				continue;
			}
			if(ownerMapping.has_value()) {
				// Multiple immutable records for one component cannot prove which packet
				// belongs to this draw, so fail open rather than use a global guess.
				return std::nullopt;
			}
			ownerMapping = mapping;
		}
		if(!ownerMapping.has_value()) return std::nullopt;

		std::optional<TerraEngine::Renderer::AnimationGpuRenderBinding> match;
		const std::uint32_t rendererGeneration =
			static_cast<std::uint32_t>(protectedRendererGeneration);
		const std::uint32_t packetIndex = ownerMapping->workPacketIndex;
		if(packetIndex < immutableGpuAnimationPlan->WorkPackets().size()) {
			const TerraEngine::Animation::GpuAnimationWorkPacket& packet =
				immutableGpuAnimationPlan->WorkPackets()[packetIndex];
			const TerraEngine::Animation::GpuAnimationRenderIdentity& renderIdentity =
				immutableGpuAnimationPlan->RenderIdentity(packetIndex);
			if(packet.generationRecordIndex >= immutableGpuAnimationPlan->GenerationRecordCount()
				|| packet.stageMask == 0U
				|| !renderIdentity.IsComplete()
				|| renderIdentity != request.expectedGpuRenderIdentity) {
				return std::nullopt;
			}
			const TerraEngine::Animation::GpuAnimationGenerationRecord& generation =
				immutableGpuAnimationPlan->GenerationRecord(packet.generationRecordIndex);
			if(!TerraEngine::Animation::IsGpuAnimationGenerationRecordValid(generation)
				|| generation.instanceSlot == TerraEngine::Animation::kInvalidAnimatorInstanceSlot
				|| packet.stableInstanceId != ownerMapping->stableInstanceId
				|| generation.instanceSlot != ownerMapping->instanceSlot
				|| generation.instanceGeneration != ownerMapping->instanceGeneration
				|| generation.componentGeneration != ownerMapping->componentGeneration
				|| generation.componentGeneration != request.componentGeneration
				|| generation.meshGeneration != renderIdentity.meshHeaderVersion
				|| generation.sceneGeneration != protectedSceneGeneration
				|| generation.rendererGeneration != rendererGeneration
				|| packet.stableInstanceId != ((static_cast<std::uint64_t>(generation.instanceSlot) << 32U)
					| static_cast<std::uint64_t>(generation.instanceGeneration))) {
				return std::nullopt;
			}

			TerraEngine::Renderer::AnimationGpuRenderBinding candidate{};
			candidate.stableInstanceId = packet.stableInstanceId;
			candidate.presentationGeneration = gpuPresentation->presentationGeneration;
			candidate.sourceGeneration = gpuPresentation->sourceGeneration;
			candidate.paletteGeneration = gpuPresentation->presentationGeneration;
			candidate.planGeneration = immutableGpuAnimationPlan->generation;
			candidate.rendererGeneration = rendererGeneration;
			candidate.workPacketIndex = packetIndex;
			candidate.stableSubmissionOrdinal = packet.stableSubmissionOrdinal;
			candidate.instanceSlot = generation.instanceSlot;
			candidate.instanceGeneration = generation.instanceGeneration;
			candidate.componentGeneration = generation.componentGeneration;
			candidate.skeletonGeneration = generation.skeletonGeneration;
			candidate.meshGeneration = generation.meshGeneration;
			candidate.resourceGeneration = generation.resourceGeneration;
			candidate.sceneGeneration = generation.sceneGeneration;
			candidate.workGeneration = generation.workGeneration;
			candidate.programGeneration = generation.programGeneration;
			candidate.stageMask = packet.stageMask;
			candidate.renderIdentity = renderIdentity;
			candidate.hybridCpuAuthority = TerraEngine::Animation::HasGpuAnimationFlag(
				packet.flags,
				TerraEngine::Animation::GpuAnimationWorkFlag::HybridCpuAuthority);
			candidate.valid = true;
			if(match.has_value()) {
				// The plan ABI names one packet per animator. Without a mesh ID in the
				// packet, two candidates cannot be proven to belong to this draw even
				// when their submission ordinals differ, so reject fail-open.
				return std::nullopt;
			}
			match = candidate;
		}
		return match;

	}
}

// Patch or rebuild the renderer-owned deterministic membership cache while its
// exact scene packet is retained by protected capture. No worker opens live ECS.
void RenderSystem::RefreshFrameRenderableMembership(
	const std::shared_ptr<const TerraEngine::Scene::SceneChangePacket>& protectedSceneChanges,
	bool renderableMetadataCaptureCurrent, std::uint64_t protectedTerrainProxyGeneration)
{
	auto& registry = objManager.getRegistry();
	const std::uint64_t membershipSceneGeneration =
		objManager.GetSceneGeneration();
	const std::uint64_t membershipComponentRevision =
		objManager.GetComponentMutationRevision();
	const std::uint64_t membershipHierarchyRevision =
		objManager.GetHierarchyRevision();
	const std::uint64_t membershipActiveRevision =
		objManager.GetRuntimeActiveMembershipRevision();
	const auto resolveRenderBinding = [&](entt::entity objectEntity)
		-> std::optional<MeshRenderComponent> {
		// Terrain owns a specialized derived binding. When TerrainComponent is
		// present it is authoritative, so a stale legacy mesh component cannot
		// accidentally render a second copy of the same surface.
		if(const TerrainComponent* terrain = registry.try_get<TerrainComponent>(objectEntity))
		{
			if(!IsRuntimeComponentActive(*terrain) || !terrain->visible
				|| terrain->terrainAssetId == TerraEngine::EMPTY_UUID)
			{
				return std::nullopt;
			}
			const auto proxy = TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
				.Resolve(terrain->terrainAssetId);
			if(!proxy || !proxy->IsComplete()) return std::nullopt;
			MeshRenderComponent binding;
			binding.meshId = proxy->meshId;
			binding.materialId = proxy->materialId;
			binding.spatialObjectId = TerraEngine::EMPTY_UUID;
			// Direct terrain meshes use complete per-area bounds and therefore do
			// not rely on generic task-shader culling metadata.
			binding.taskShaderCullingEnabled = false;
			return binding;
		}
		if(const MeshRenderComponent* mesh = registry.try_get<MeshRenderComponent>(objectEntity);
			mesh != nullptr && IsRuntimeComponentActive(*mesh))
		{
			return *mesh;
		}
		return std::nullopt;
	};
	const auto captureMembership = [&](Object object)
		-> std::optional<RenderableMembershipCacheRecord> {
		const entt::entity objectEntity = object.EntityId();
		if(!registry.valid(objectEntity)
			|| !registry.all_of<Transform>(objectEntity)
			|| !objManager.isObjectActiveInHierarchy(object)) {
			return std::nullopt;
		}
		const Transform& transform = registry.get<Transform>(objectEntity);
		const std::optional<MeshRenderComponent> resolvedBinding =
			resolveRenderBinding(objectEntity);
		if(!IsRuntimeComponentActive(transform) || !resolvedBinding) {
			return std::nullopt;
		}
		const MeshRenderComponent& meshComp = *resolvedBinding;
		RenderableMembershipCacheRecord cached;
		cached.object = object;
		cached.objectId = meshComp.spatialObjectId != TerraEngine::EMPTY_UUID
			? meshComp.spatialObjectId : objManager.getUUID(object);
		if(const TerrainComponent* terrain = registry.try_get<TerrainComponent>(objectEntity);
			terrain != nullptr && IsRuntimeComponentActive(*terrain) && terrain->visible)
		{
			cached.terrainAssetId = terrain->terrainAssetId;
		}
		cached.mesh = meshComp;
		cached.objectIdentity = static_cast<std::uint64_t>(
			std::hash<TerraEngine::UUID>{}(cached.objectId));
		cached.meshIdentity = static_cast<std::uint64_t>(
			std::hash<TerraEngine::UUID>{}(meshComp.meshId));
		cached.materialIdentity = std::max<std::uint64_t>(1U,
			static_cast<std::uint64_t>(
				std::hash<TerraEngine::UUID>{}(meshComp.materialId)));
		cached.objectStatic = objManager.isObjectStatic(object);
		std::optional<Object> hierarchyObject = object;
		for(std::size_t depth = 0U;
			hierarchyObject.has_value() && depth < 1024U; ++depth) {
			const entt::entity entity = hierarchyObject->EntityId();
			if(registry.valid(entity)
				&& registry.all_of<AnimatorComponent>(entity)) {
				const AnimatorComponent& animator =
					registry.get<AnimatorComponent>(entity);
				if(IsRuntimeComponentActive(animator)) {
					cached.nearestAnimatorObject = hierarchyObject;
					cached.animatorObjectId = objManager.getUUID(*hierarchyObject);
					cached.animatorObjectLifetime =
						objManager.getObjectLifetimeGeneration(*hierarchyObject);
					const std::uint64_t componentGeneration =
						objManager.getObjectComponentGeneration(*hierarchyObject);
					if(componentGeneration <=
						std::numeric_limits<std::uint32_t>::max()) {
						cached.animatorComponentGeneration =
							static_cast<std::uint32_t>(componentGeneration);
					}
					cached.skeletonOverrideAssetId =
						animator.skeletonOverrideAssetId;
				}
				break;
			}
			hierarchyObject = objManager.getParent(*hierarchyObject);
		}
		return cached;
	};
	const bool membershipChanged =
		!renderableMetadataCaptureCurrent
		|| renderableMembershipSceneGeneration != membershipSceneGeneration
		|| renderableMembershipComponentRevision != membershipComponentRevision
		|| renderableMembershipHierarchyRevision != membershipHierarchyRevision
		|| renderableMembershipActiveRevision != membershipActiveRevision
		|| renderableMembershipTerrainProxyGeneration
			!= protectedTerrainProxyGeneration;
	if(membershipChanged) {
		const bool terrainProxyChanged =
			renderableMembershipTerrainProxyGeneration
				!= protectedTerrainProxyGeneration;
		const bool sameSceneAndHierarchy =
			renderableMembershipSceneGeneration == membershipSceneGeneration
			&& renderableMembershipHierarchyRevision == membershipHierarchyRevision;
		// A consecutive packet can still predate a same-frame Inspector
		// edit. Patch only if its journal covers the live state being
		// captured; otherwise rebuild once at this protected boundary.
		const bool packetIsNext = protectedSceneChanges != nullptr
			&& objManager.CanApplySceneChangePacketToLiveState(
				*protectedSceneChanges, renderableMembershipPacketRevision);
		// A proxy publication can replace the mesh or material for every
		// instance of one terrain asset without producing an ECS change packet.
		// Rebuild the protected membership snapshot in that case instead of
		// retaining stale derived bindings through the object-delta fast path.
		const bool applyDelta = !terrainProxyChanged
			&& sameSceneAndHierarchy && packetIsNext
			&& !renderableMembershipCache.empty();
		if(applyDelta) {
			std::vector<TerraEngine::Scene::SceneObjectRevisionSnapshot>
				changedObjects;
			const auto appendChanged = [&changedObjects](const auto& changes) {
				changedObjects.insert(changedObjects.end(), changes.begin(), changes.end());
			};
			appendChanged(protectedSceneChanges->Added());
			appendChanged(protectedSceneChanges->Removed());
			appendChanged(protectedSceneChanges->MetadataChanged());
			appendChanged(protectedSceneChanges->ComponentChanged());
			std::sort(changedObjects.begin(), changedObjects.end(),
				[](const auto& left, const auto& right) {
					return left.objectId < right.objectId;
				});
			for(std::size_t changeIndex = 0U;
				changeIndex < changedObjects.size();) {
				const TerraEngine::UUID changedId =
					changedObjects[changeIndex].objectId;
				Object candidate = changedObjects[changeIndex].object;
				std::size_t nextChange = changeIndex + 1U;
				while(nextChange < changedObjects.size()
					&& changedObjects[nextChange].objectId == changedId) {
					if(registry.valid(changedObjects[nextChange].object.EntityId())) {
						candidate = changedObjects[nextChange].object;
					}
					++nextChange;
				}
				const auto existing = std::lower_bound(
					renderableMembershipCache.begin(), renderableMembershipCache.end(),
					changedId, [](const RenderableMembershipCacheRecord& record,
						const TerraEngine::UUID& id) { return record.objectId < id; });
				if(existing != renderableMembershipCache.end()
					&& existing->objectId == changedId) {
					renderableMembershipCache.erase(existing);
				}
				if(const auto refreshed = captureMembership(candidate);
					refreshed.has_value()) {
					const auto insertion = std::lower_bound(
						renderableMembershipCache.begin(), renderableMembershipCache.end(),
						refreshed->objectId,
						[](const RenderableMembershipCacheRecord& record,
							const TerraEngine::UUID& id) { return record.objectId < id; });
					renderableMembershipCache.insert(insertion, *refreshed);
				}
				changeIndex = nextChange;
			}
		}
		else {
			// A hierarchy mutation can affect every descendant's animator owner.
			// A missed packet can hide intermediate removals. Both cases require
			// one complete protected rebuild rather than speculative patching.
			renderableMembershipCache.clear();
			objManager.forEachObjectWithComponents<Transform, MeshRenderComponent>(
				[&](Object object, Transform&, MeshRenderComponent&) {
					if(registry.any_of<TerrainComponent>(object.EntityId())) return;
					if(const auto captured = captureMembership(object);
						captured.has_value()) {
						renderableMembershipCache.push_back(*captured);
					}
				});
			objManager.forEachObjectWithComponents<Transform, TerrainComponent>(
				[&](Object object, Transform&, TerrainComponent&) {
					if(const auto captured = captureMembership(object);
						captured.has_value()) {
						renderableMembershipCache.push_back(*captured);
					}
				});
		}
		// The immutable snapshot and every index-aligned render working set use
		// object-id order. Canonicalize membership only when authored structure
		// changes so stable crowds do not sort thousands of records every frame.
		std::sort(renderableMembershipCache.begin(),
			renderableMembershipCache.end(),
			[](const RenderableMembershipCacheRecord& left,
				const RenderableMembershipCacheRecord& right) {
				return left.objectId < right.objectId;
			});
		renderableMembershipSceneGeneration = membershipSceneGeneration;
		renderableMembershipComponentRevision = membershipComponentRevision;
		renderableMembershipHierarchyRevision = membershipHierarchyRevision;
		renderableMembershipActiveRevision = membershipActiveRevision;
		renderableMembershipTerrainProxyGeneration =
			protectedTerrainProxyGeneration;
		renderableMembershipPacketRevision = protectedSceneChanges != nullptr
			? protectedSceneChanges->PublicationRevision() : 0U;
	}
}

namespace TerraEngine::Renderer::Frame {
	TerraEngine::Renderer::AnimationRenderBindingStatus ValidateAnimatorPosePublication(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Animation::AnimationAuthorityRow& authorityRow) noexcept
	{
		using TerraEngine::Renderer::AnimationRenderBindingStatus;
		if(authorityRow.safeFallback
			|| authorityRow.status != TerraEngine::Animation::AnimationStatus::Ok) {
			return AnimationRenderBindingStatus::SafePose;
		}
		const TerraEngine::Animation::AnimatorInstanceHandle animator =
			authorityRow.key.Handle();
		if(!authorityRow.IsValid()
			|| authorityRow.ownerObjectId != request.animatorObjectId
			|| authorityRow.componentGeneration != request.componentGeneration
			|| !animator.IsValid()
			|| animator.slotIndex != request.animatorSlotIndex
			|| animator.generation != request.animatorGeneration
			|| !authorityRow.skeleton.IsValid()
			|| authorityRow.skeleton.assetId.value != request.skeletonAssetId
			|| authorityRow.skeleton.generation != request.skeletonGeneration) {
			return AnimationRenderBindingStatus::StaleGeneration;
		}
		if(authorityRow.skeletonCompatibilityFingerprint
			!= request.skeletonCompatibilityFingerprint) {
			return AnimationRenderBindingStatus::IncompatibleSkeleton;
		}
		if(authorityRow.snapshot == nullptr || authorityRow.snapshot->pose == nullptr
			|| authorityRow.snapshot->animator != animator
			|| authorityRow.snapshot->publicationGeneration == 0U
			|| authorityRow.snapshot->playback.latestStatus
				!= TerraEngine::Animation::AnimationStatus::Ok
			|| authorityRow.snapshot->pose->ValidateForPublication()
				!= TerraEngine::Animation::AnimationStatus::Ok) {
			return AnimationRenderBindingStatus::InvalidPose;
		}

		const TerraEngine::Animation::AnimationPoseGeneration& poseGeneration =
			authorityRow.snapshot->pose->Generation();
		if(poseGeneration.animator != animator
			|| poseGeneration.poseGeneration != authorityRow.snapshot->publicationGeneration
			|| poseGeneration.skeleton != authorityRow.skeleton
			|| poseGeneration.programGeneration == 0U) {
			return AnimationRenderBindingStatus::InvalidPose;
		}
		return AnimationRenderBindingStatus::Ready;
	}

	bool HeldAnimationBindingMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request) noexcept
	{
		using namespace TerraEngine::Renderer;
		const AnimationRenderBinding& held = request.heldBinding;
		if(held.status != AnimationRenderBindingStatus::Ready || held.safePose
			|| !held.HasCpuPose() || held.posePublicationGeneration == 0U
			|| held.meshId != request.meshId
			|| held.animatorObjectId != request.animatorObjectId
			|| held.skeletonAssetId != request.skeletonAssetId
			|| held.animatorObjectGeneration != request.animatorObjectGeneration
			|| held.animationAuthorityGeneration != request.animationAuthorityGeneration
			|| held.animatorSlotIndex != request.animatorSlotIndex
			|| held.animatorGeneration != request.animatorGeneration
			|| held.deformablePayloadGeneration != request.deformablePayloadGeneration
			|| held.componentGeneration != request.componentGeneration
			|| held.skeletonGeneration != request.skeletonGeneration
			|| held.skeletonCompatibilityFingerprint
				!= request.skeletonCompatibilityFingerprint
			|| held.invalidation.asset != request.invalidation.asset
			|| held.invalidation.page != request.invalidation.page
			|| held.invalidation.material != request.invalidation.material
			|| held.invalidation.renderer != request.invalidation.renderer
			|| held.invalidation.residency != request.invalidation.residency) {
			return false;
		}
		return held.authorityRow != nullptr
			&& ValidateAnimatorPosePublication(request, *held.authorityRow)
				== AnimationRenderBindingStatus::Ready
			&& held.authorityRow->snapshot->publicationGeneration
				== held.posePublicationGeneration;
	}

	bool GpuVisualIdentityMatchesRequest(
		const TerraEngine::Renderer::AnimationRenderBindingRequest& request,
		const TerraEngine::Renderer::AnimationGpuRenderBinding& identity) noexcept
	{
		// GPU Visual is allowed to omit a CPU pose, but it is never allowed to
		// omit identity. Every value below is copied from one published packet and
		// compared as one bundle before a draw can reference its palette.
		if(!identity.IsComplete() || identity.hybridCpuAuthority
			|| request.meshId == TerraEngine::EMPTY_UUID
			|| request.animatorObjectId == TerraEngine::EMPTY_UUID
			|| request.animatorObjectGeneration == 0U
			|| request.animationAuthorityGeneration == 0U
			|| request.deformablePayloadGeneration == 0U
			|| request.componentGeneration == 0U
			|| request.skeletonCompatibilityFingerprint == 0U
			|| !request.expectedGpuRenderIdentity.IsComplete()
			|| !identity.renderIdentity.IsComplete()) {
			return false;
		}
		const std::uint64_t packedStableInstanceId =
			(static_cast<std::uint64_t>(identity.instanceSlot) << 32U)
			| static_cast<std::uint64_t>(identity.instanceGeneration);
		return identity.stableInstanceId == packedStableInstanceId
			&& identity.componentGeneration == request.componentGeneration
			&& identity.renderIdentity == request.expectedGpuRenderIdentity
			&& identity.renderIdentity.meshId == request.meshId
			&& identity.renderIdentity.deformablePayloadGeneration
				== request.deformablePayloadGeneration
			&& identity.renderIdentity.skeletonCompatibilityFingerprint
				== request.skeletonCompatibilityFingerprint
			&& identity.meshGeneration
				== identity.renderIdentity.meshHeaderVersion
			&& identity.rendererGeneration
				== identity.renderIdentity.rendererGeneration
			&& identity.sceneGeneration
				== identity.renderIdentity.sceneGeneration;
	}

	bool IsMorphOnlyIdentityDeformableMesh(const Mesh& mesh) noexcept
	{
		const TemfDeformableMetadata& metadata = mesh.deformableMetadata;
		// Morph-only cooks deliberately carry no skeleton fingerprint. Their one
		// identity joint exists solely because skinning and morph rendering share
		// the frozen deformable vertex layout; it must never be interpreted as a
		// mesh authored for an arbitrary skeleton.
		return metadata.skeletonCompatibilityFingerprint == 0U
			&& metadata.sourceSkeletonJointCount == 1U
			&& metadata.morphTargetCount != 0U
			&& metadata.morphTargetCount == mesh.deformableMorphs.size()
			&& metadata.palettePartitionCount == 1U
			&& mesh.deformablePalettes.size() == 1U
			&& mesh.deformablePalettes.front().jointCount == 1U
			&& mesh.deformablePaletteJoints.size() == 1U
			&& mesh.deformablePaletteJoints.front() == 0U;
	}

	TerraEngine::Animation::AnimationCompatibilityFingerprint
	ResolveDeformableSkeletonFingerprint(
		const Mesh& mesh,
		const TerraEngine::Animation::AnimationAuthorityRow* authorityRow) noexcept
	{
		const TerraEngine::Animation::AnimationCompatibilityFingerprint
			authoredFingerprint =
			mesh.deformableMetadata.skeletonCompatibilityFingerprint;
		if(authoredFingerprint != 0U) return authoredFingerprint;
		if(!IsMorphOnlyIdentityDeformableMesh(mesh) || authorityRow == nullptr
			|| authorityRow->skeletonCompatibilityFingerprint == 0U
			|| authorityRow->skeletonAsset == nullptr
			|| authorityRow->skeletonAsset->compatibilityFingerprint
				!= authorityRow->skeletonCompatibilityFingerprint
			|| authorityRow->skeletonAsset->joints.size() != 1U
			|| authorityRow->skeletonAsset->inverseBindMatrices.size() != 1U) {
			return 0U;
		}
		// A checked one-joint publication supplies lifecycle identity for the
		// shared CPU graphics bridge. The mesh remains skeleton-unbound on disk;
		// only its morph lanes are allowed to use this effective fingerprint.
		return authorityRow->skeletonCompatibilityFingerprint;
	}

	bool IsValidatedAnimationDeformableMesh(const Mesh& mesh) noexcept
	{
		const TemfDeformableMetadata& metadata = mesh.deformableMetadata;
		constexpr std::uint32_t requiredFlags = kTemfDeformableFlagConservativeMorphEnvelope
			| kTemfDeformableFlagZeroWeightMeansIdentity
			| kTemfDeformableFlagVertexPaletteIndices
			| kTemfDeformableFlagDenseMorphDeltas;
		// ResourceManager sets hasDeformablePayload only after full range validation.
		// Rechecking canonical counts here keeps synthetic in-memory Mesh values
		// from waking AnimationSystem for static content.
		return mesh.hasDeformablePayload
			&& mesh.deformablePayloadGeneration != 0U
			&& metadata.skinningVertexCount != 0U
			&& metadata.skinningVertexCount == mesh.runtimeLayout.meshletSkinningDataCount
			&& metadata.palettePartitionCount != 0U
			&& metadata.palettePartitionCount == mesh.deformablePalettes.size()
			&& mesh.deformableVertexPaletteIndices.size()
				== metadata.skinningVertexCount
			&& mesh.meshletSkinningData.size()
				== static_cast<std::size_t>(metadata.skinningVertexCount)
					* kTemfSkinningStrideBytes
			&& metadata.sourceSkeletonJointCount != 0U
			&& (metadata.skeletonCompatibilityFingerprint != 0U
				|| IsMorphOnlyIdentityDeformableMesh(mesh))
			&& metadata.morphTargetCount == mesh.deformableMorphs.size()
			&& metadata.flags == requiredFlags;
	}
}

// Collect one owner's binding against exact captured presentation and mesh roots.
TerraEngine::Renderer::AnimationRenderBinding RenderSystem::CollectFrameAnimationBinding(
	RenderableMembershipCacheRecord& cached, const Mesh* runtimeMesh,
	bool lastRuntimeMeshDeformable,
	TerraEngine::Animation::AnimationCompatibilityFingerprint lastRuntimeMeshSkeletonFingerprint,
	const TerraEngine::Renderer::Frame::RenderAnimationCaptureView& animationCapture,
	const std::shared_ptr<const TerraEngine::Animation::AnimationPresentationFrame>& animationPresentationFrame,
	std::optional<std::uint64_t>& animationAuthorityGeneration,
	std::unordered_set<TerraEngine::UUID>& aliveCompatibleAnimationObjects)
{
	const auto meshId = cached.mesh.meshId;
	const auto objectId = cached.objectId;
	const auto protectedRendererGeneration = animationCapture.protectedRendererGeneration;
	const auto protectedSceneGeneration = animationCapture.protectedSceneGeneration;
	TerraEngine::Renderer::AnimationRenderBinding animationBinding;
	if(runtimeMesh != nullptr && lastRuntimeMeshDeformable) {
		animationBinding.meshId = meshId;
		animationBinding.deformablePayloadGeneration =
			runtimeMesh->deformablePayloadGeneration;
		animationBinding.skeletonCompatibilityFingerprint =
			runtimeMesh->deformableMetadata.skeletonCompatibilityFingerprint;
		animationBinding.pageLocalDeformationAvailable =
			runtimeMesh->hasDeformableDemandedPages
			&& runtimeMesh->virtualGeometryPageStreamingReady;

		const std::optional<Object>& nearestAnimatorObject =
			cached.nearestAnimatorObject;
		const TerraEngine::UUID skeletonOverrideAssetId =
			cached.skeletonOverrideAssetId;

		if(nearestAnimatorObject.has_value()) {
			const TerraEngine::UUID animatorObjectId = cached.animatorObjectId;
			const std::uint64_t animatorObjectLifetime =
				cached.animatorObjectLifetime;
			const std::uint64_t componentGeneration64 =
				cached.animatorComponentGeneration;
			if(objectId != TerraEngine::EMPTY_UUID
				&& animatorObjectId != TerraEngine::EMPTY_UUID
				&& animatorObjectLifetime != 0U
				&& componentGeneration64 != 0U
				&& componentGeneration64 <= std::numeric_limits<std::uint32_t>::max()) {
				const std::uint32_t componentGeneration =
					static_cast<std::uint32_t>(componentGeneration64);
				if(!animationAuthorityGeneration.has_value()
					&& animationPresentationFrame != nullptr
					&& animationPresentationFrame->IsValid()) {
					// The presentation root owns the exact authority it represents.
					// Do not reopen AnimationSystem and risk a mixed generation.
					animationAuthorityGeneration =
						animationPresentationFrame->authority->authorityGeneration;
				}
				// GPU-visual crowds do not publish CPU poses. Prove their complete
				// packet identity before consulting either CPU-pose hash table; the
				// protected cache-retirement pass below still removes a stale held
				// pose if an animator changed from CPU authority to GPU visual.
				TerraEngine::Renderer::AnimationRenderBindingRequest gpuVisualRequest;
				gpuVisualRequest.meshId = meshId;
				gpuVisualRequest.animatorObjectId = animatorObjectId;
				gpuVisualRequest.animatorObjectGeneration = animatorObjectLifetime;
				gpuVisualRequest.animationAuthorityGeneration =
					animationAuthorityGeneration.value_or(0U);
				gpuVisualRequest.deformablePayloadGeneration =
					runtimeMesh->deformablePayloadGeneration;
				gpuVisualRequest.componentGeneration = componentGeneration;
				gpuVisualRequest.skeletonCompatibilityFingerprint =
					lastRuntimeMeshSkeletonFingerprint;
				gpuVisualRequest.pageLocalDeformationAvailable =
					runtimeMesh->hasDeformableDemandedPages
					&& runtimeMesh->virtualGeometryPageStreamingReady;
				gpuVisualRequest.expectedGpuRenderIdentity.meshId = meshId;
				gpuVisualRequest.expectedGpuRenderIdentity.meshHeaderVersion =
					runtimeMesh->runtimeLayout.version;
				gpuVisualRequest.expectedGpuRenderIdentity.deformablePayloadGeneration =
					runtimeMesh->deformablePayloadGeneration;
				gpuVisualRequest.expectedGpuRenderIdentity.rendererGeneration =
					protectedRendererGeneration;
				gpuVisualRequest.expectedGpuRenderIdentity.sceneGeneration =
					protectedSceneGeneration;
				gpuVisualRequest.expectedGpuRenderIdentity.skeletonCompatibilityFingerprint =
					gpuVisualRequest.skeletonCompatibilityFingerprint;
				gpuVisualRequest.invalidation.asset =
					runtimeMesh->deformablePayloadGeneration
					^ gpuVisualRequest.skeletonCompatibilityFingerprint;
				gpuVisualRequest.invalidation.page =
					runtimeMesh->deformablePayloadGeneration;
				gpuVisualRequest.invalidation.view = nextRenderFrameId;
				gpuVisualRequest.invalidation.material =
					cached.materialIdentity;
				gpuVisualRequest.invalidation.renderer = protectedRendererGeneration;
				gpuVisualRequest.invalidation.residency =
					runtimeMesh->deformablePayloadGeneration;
				gpuVisualRequest.gpuVisualIdentity =
					TerraEngine::Renderer::Frame::FindUniqueGpuAnimationIdentity(animationCapture,
						gpuVisualRequest, cached.gpuOwnerMappingHint);
				const bool hasDirectGpuVisualBinding =
					gpuVisualRequest.gpuVisualIdentity.has_value()
					&& !gpuVisualRequest.gpuVisualIdentity->hybridCpuAuthority;
if(hasDirectGpuVisualBinding) {
	animationBinding = ResolveAnimationRenderBinding(
		gpuVisualRequest);
				} else {

				const TerraEngine::Animation::AnimationAuthorityRow*
					authorityRow = nullptr;
				if(animationPresentationFrame != nullptr
					&& animationPresentationFrame->IsValid()) {
					authorityRow = animationPresentationFrame
						->FindAuthorityRowForOwner(animatorObjectId);
				}

				auto held = compatibleAnimationPoseCache.find(objectId);
				const TerraEngine::Animation::AnimationAuthorityRow*
					compatibilityAuthorityRow = authorityRow;
				if(compatibilityAuthorityRow == nullptr
					&& held != compatibleAnimationPoseCache.end()) {
					compatibilityAuthorityRow =
						held->second.binding.authorityRow;
				}
				const TerraEngine::Animation::AnimationCompatibilityFingerprint
					effectiveSkeletonFingerprint =
						ResolveDeformableSkeletonFingerprint(
							*runtimeMesh,
						compatibilityAuthorityRow);

				TerraEngine::Renderer::AnimationRenderBindingRequest request;
				request.meshId = meshId;
				request.animatorObjectId = animatorObjectId;
				request.animatorObjectGeneration = animatorObjectLifetime;
				request.animationAuthorityGeneration =
					animationAuthorityGeneration.value_or(0U);
				request.deformablePayloadGeneration = runtimeMesh->deformablePayloadGeneration;
				request.componentGeneration = componentGeneration;
				request.skeletonCompatibilityFingerprint =
					effectiveSkeletonFingerprint;
				request.pageLocalDeformationAvailable =
					runtimeMesh->hasDeformableDemandedPages
					&& runtimeMesh->virtualGeometryPageStreamingReady;
		request.expectedGpuRenderIdentity.meshId = meshId;
		request.expectedGpuRenderIdentity.meshHeaderVersion =
			// Bindings compare the already decoded runtime layout, not an
			// obsolete serialized-header alias. This value changes whenever
			// the renderer-visible meshlet contract changes.
			runtimeMesh->runtimeLayout.version;
				request.expectedGpuRenderIdentity.deformablePayloadGeneration =
					runtimeMesh->deformablePayloadGeneration;
				request.expectedGpuRenderIdentity.rendererGeneration =
					protectedRendererGeneration;
				request.expectedGpuRenderIdentity.sceneGeneration =
					protectedSceneGeneration;
				request.expectedGpuRenderIdentity.skeletonCompatibilityFingerprint =
					effectiveSkeletonFingerprint;
				request.invalidation.asset = runtimeMesh->deformablePayloadGeneration
					^ effectiveSkeletonFingerprint;
				request.invalidation.page = runtimeMesh->deformablePayloadGeneration;
				request.invalidation.view = nextRenderFrameId;
				request.invalidation.material = cached.materialIdentity;
				request.invalidation.renderer = protectedRendererGeneration;
				request.invalidation.residency = runtimeMesh->deformablePayloadGeneration;
				request.presentationFrame = animationPresentationFrame;
				request.authorityRow = authorityRow;

				if(authorityRow != nullptr) {
					const TerraEngine::Animation::AnimatorInstanceHandle animator =
						authorityRow->key.Handle();
					request.animatorSlotIndex = animator.slotIndex;
					request.animatorGeneration = animator.generation;
					request.skeletonAssetId = skeletonOverrideAssetId != TerraEngine::EMPTY_UUID
						? skeletonOverrideAssetId
						: authorityRow->skeleton.assetId.value;
					request.skeletonGeneration = authorityRow->skeleton.generation;
				} else if(held != compatibleAnimationPoseCache.end()) {
					const TerraEngine::Renderer::AnimationRenderBinding& prior = held->second.binding;
					const bool liveIdentityMatches = prior.meshId == request.meshId
						&& prior.animatorObjectId == request.animatorObjectId
						&& prior.animatorObjectGeneration == request.animatorObjectGeneration
						&& prior.animationAuthorityGeneration == request.animationAuthorityGeneration
						&& prior.deformablePayloadGeneration == request.deformablePayloadGeneration
						&& prior.componentGeneration == request.componentGeneration
						&& prior.skeletonCompatibilityFingerprint
							== request.skeletonCompatibilityFingerprint
						&& (skeletonOverrideAssetId == TerraEngine::EMPTY_UUID
							|| prior.skeletonAssetId == skeletonOverrideAssetId);
					if(liveIdentityMatches) {
						request.animatorSlotIndex = prior.animatorSlotIndex;
						request.animatorGeneration = prior.animatorGeneration;
						request.skeletonAssetId = prior.skeletonAssetId;
						request.skeletonGeneration = prior.skeletonGeneration;
						request.fallbackPolicy = TerraEngine::Renderer::AnimationRenderFallbackPolicy::HoldLastCompatiblePose;
						request.heldBinding = prior;
					} else {
						compatibleAnimationPoseCache.erase(held);
						held = compatibleAnimationPoseCache.end();
					}
				}
				// The published owner mapping names one exact packet for both
				// Visual GPU and Hybrid. Resolve it while all mesh identity is
				// already hot instead of rescanning every binding in a later pass.
				request.gpuVisualIdentity =
					TerraEngine::Renderer::Frame::FindUniqueGpuAnimationIdentity(animationCapture,
						request, cached.gpuOwnerMappingHint);

				animationBinding = ResolveAnimationRenderBinding(request);
				if(authorityRow != nullptr
					&& animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
					&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::CpuPose
					&& request.gpuVisualIdentity.has_value()
					&& request.gpuVisualIdentity->hybridCpuAuthority) {
					// Hybrid keeps CPU authority while the exact GPU packet supplies
					// deformation. The helper above already validated every generation.
					animationBinding.gpu = *request.gpuVisualIdentity;
				}
				if(animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
					&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::CpuPose) {
					compatibleAnimationPoseCache[objectId] = {
						animationBinding, nextRenderFrameId };
					aliveCompatibleAnimationObjects.insert(objectId);
				} else if(animationBinding.status == TerraEngine::Renderer::AnimationRenderBindingStatus::Ready
					&& animationBinding.source == TerraEngine::Renderer::AnimationRenderPoseSource::HeldCpuPose
					&& held != compatibleAnimationPoseCache.end()) {
					held->second.lastTouchedFrameId = nextRenderFrameId;
					aliveCompatibleAnimationObjects.insert(objectId);
				} else if(held != compatibleAnimationPoseCache.end()) {
					// The lookup above already proved whether a held CPU pose exists.
					// Erase by iterator so a non-CPU binding does not hash the same
					// object identity twice on every frame.
					compatibleAnimationPoseCache.erase(held);
				}
				}
			}
		}
	}
	return animationBinding;
}

// Collect immutable resource-backed renderable rows after camera collection.
void RenderSystem::CollectFrameRenderableInputs(
	const std::pmr::vector<CameraEntry>& cameras, std::vector<RenderableEntry>& renderables,
	const TerraEngine::Renderer::Frame::RenderResourceCaptureView& resources,
	const TerraEngine::Renderer::Frame::RenderAnimationCollectionView& animation,
	const TerraEngine::Renderer::Frame::RenderMembershipCaptureView& membership)
{
	auto& frameResourceAvailability = resources.frameResourceAvailability;
	auto& frameMeshResources = resources.frameMeshResources;
	auto& frameMaterialResources = resources.frameMaterialResources;
	auto& frameMeshLocalBounds = resources.frameMeshLocalBounds;
	auto& resourceManager = resources.resourceManager;
	auto& renderableResourcePublicationsComplete = resources.renderableResourcePublicationsComplete;
	auto& runtimeLayoutRejectedRecordCount = resources.runtimeLayoutRejectedRecordCount;
	auto& runtimeLayoutRejectionReasons = resources.runtimeLayoutRejectionReasons;
	auto& animationCapture = animation.animationCapture;
	auto& animationPresentationFrame = animation.animationPresentationFrame;
	auto& animationAuthorityGeneration = animation.animationAuthorityGeneration;
	auto& aliveCompatibleAnimationObjects = animation.aliveCompatibleAnimationObjects;
	auto& hasCpuDeformationCandidates = animation.hasCpuDeformationCandidates;
	auto& protectedSceneChanges = membership.protectedSceneChanges;
	auto& protectedSceneGeneration = membership.protectedSceneGeneration;
	auto& protectedTerrainProxyGeneration = membership.protectedTerrainProxyGeneration;
	auto& reuseProtectedRenderableCapture = membership.reuseProtectedRenderableCapture;
	auto& renderableMetadataCaptureCurrent = membership.renderableMetadataCaptureCurrent;
	auto& hasQueuedDedicatedCapture = membership.hasQueuedDedicatedCapture;

	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Collect Renderables", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if (cameras.empty() && !hasSceneViewCamera
		&& !hasQueuedDedicatedCapture)
	{
		// No viewport or dedicated capture can consume this snapshot.
		// Preserve the old no-work fast path for truly headless frames.
		return;
	}
	if(reuseProtectedRenderableCapture)
	{
		// Resource, component, hierarchy, renderer, and animation-topology
		// generations match the prior complete capture. Current GPU pose data
		// is resolved later from the new presentation root, so no ECS or
		// ResourceManager lookup is required for this crowd.
		if(protectedSceneChanges != nullptr
			&& protectedSceneChanges->Validate(protectedSceneGeneration).valid)
		{
			renderableMembershipPacketRevision =
				protectedSceneChanges->PublicationRevision();
		}
		return;
	}
	RefreshFrameRenderableMembership(protectedSceneChanges, renderableMetadataCaptureCurrent,
		protectedTerrainProxyGeneration);
	const auto resourceAvailable = [&frameResourceAvailability, &resourceManager](
		const TerraEngine::UUID& resourceId)
	{
		const auto cachedResource = frameResourceAvailability.find(resourceId);
		if(cachedResource != frameResourceAvailability.end()) {
			return cachedResource->second;
		}
		const bool available = resourceManager.HasResource(resourceId);
		frameResourceAvailability.emplace(resourceId, available);
		return available;
	};
	TerraEngine::UUID lastAvailabilityMeshId = TerraEngine::EMPTY_UUID;
	TerraEngine::UUID lastAvailabilityMaterialId = TerraEngine::EMPTY_UUID;
	bool lastMeshAvailable = false;
	bool lastMaterialAvailable = false;
	const Mesh* lastRuntimeMesh = nullptr;
	bool lastRuntimeMeshDeformable = false;
	TerraEngine::Animation::AnimationCompatibilityFingerprint
		lastRuntimeMeshSkeletonFingerprint = 0U;
	for(RenderableMembershipCacheRecord& cached
		: renderableMembershipCache) {
			const Object& object = cached.object;
			const MeshRenderComponent& meshComp = cached.mesh;
			const TerraEngine::UUID meshId = meshComp.meshId;
			const TerraEngine::UUID materialId = meshComp.materialId;
			if (meshId == TerraEngine::EMPTY_UUID || materialId == TerraEngine::EMPTY_UUID)
			{
				++runtimeLayoutRejectedRecordCount;
				runtimeLayoutRejectionReasons.push_back("missing-render-resource-id");
				continue;
			}

			if(lastAvailabilityMeshId != meshId) {
				lastAvailabilityMeshId = meshId;
				lastMeshAvailable = resourceAvailable(meshId);
				const auto cachedMesh = frameMeshResources.find(meshId);
				if(cachedMesh != frameMeshResources.end()) {
					lastRuntimeMesh = cachedMesh->second;
				} else {
					const auto owner = resourceManager.AcquireMeshSnapshot(meshId);
					if(!owner || owner->publicationGeneration == 0U)
					{
						// Demand every active membership before camera/light culling, including
						// off-camera shadow casters. The frame map deduplicates this ID; native
						// admission owns budgeting and job coalescing without blocking capture.
						renderableResourcePublicationsComplete = false;
						(void)resourceManager.RequestStreamableLoad(meshId, AssetType::Mesh,
							TerraEngine::Resource::StreamingNeedType::Visible, nextRenderFrameId,
							"renderer-missing-mesh-publication", cached.objectId);
					}
					retainedRuntimeMeshOwners[meshId] = owner;
					lastRuntimeMesh = owner.get();
					frameMeshResources.emplace(meshId, lastRuntimeMesh);
				}
				if(!frameMeshLocalBounds.contains(meshId))
				{
					frameMeshLocalBounds.emplace(meshId,
						resourceManager.GetMeshLocalBounds(meshId));
				}
				lastRuntimeMeshDeformable = lastRuntimeMesh != nullptr
					&& IsValidatedAnimationDeformableMesh(*lastRuntimeMesh);
				lastRuntimeMeshSkeletonFingerprint = lastRuntimeMesh != nullptr
					? ResolveDeformableSkeletonFingerprint(*lastRuntimeMesh, nullptr)
					: 0U;
			}
			if(lastAvailabilityMaterialId != materialId) {
				lastAvailabilityMaterialId = materialId;
				lastMaterialAvailable = resourceAvailable(materialId);
				if(!frameMaterialResources.contains(materialId))
				{
					const auto owner = resourceManager.AcquireMaterialSnapshot(materialId);
					if(!owner || owner->publicationGeneration == 0U)
					{
						// Retain the missing result honestly for this immutable frame. A later
						// accepted publication, rather than the request itself, enables reuse.
						renderableResourcePublicationsComplete = false;
						(void)resourceManager.RequestStreamableLoad(materialId, AssetType::Material,
							TerraEngine::Resource::StreamingNeedType::Visible, nextRenderFrameId,
							"renderer-missing-material-publication", cached.objectId);
					}
					retainedRuntimeMaterialOwners[materialId] = owner;
					frameMaterialResources.emplace(materialId, owner.get());
				}
			}
			if (!lastMeshAvailable || !lastMaterialAvailable)
			{
				++runtimeLayoutRejectedRecordCount;
				runtimeLayoutRejectionReasons.push_back("resource-not-loaded");
				continue;
			}

			const TerraEngine::UUID objectId = cached.objectId;

			const Mesh* runtimeMesh = lastRuntimeMesh;
			auto animationBinding = CollectFrameAnimationBinding(cached, runtimeMesh,
				lastRuntimeMeshDeformable, lastRuntimeMeshSkeletonFingerprint,
				animationCapture, animationPresentationFrame, animationAuthorityGeneration,
				aliveCompatibleAnimationObjects);

			hasCpuDeformationCandidates =
				hasCpuDeformationCandidates
				|| (!animationBinding.gpu.IsComplete()
					&& animationBinding.HasCpuPose()
					&& animationBinding.authorityRow != nullptr);
		renderables.push_back({
				object.EntityId(), objectId, cached.terrainAssetId,
				cached.objectIdentity,
				cached.meshIdentity, cached.materialIdentity,
				std::max<std::uint64_t>(1U,
					objManager.GetObjectSpatialRevision(object)),
				cached.objectStatic, &cached.mesh, runtimeMesh,
				std::move(animationBinding) });
		}
}

// Acquire exact immutable scene and animation owners at the existing protected boundary.
RenderSystem::FrameCaptureRoots RenderSystem::CaptureFrameRoots()
{
	FrameCaptureRoots result;
	auto& protectedRendererGeneration = result.protectedRendererGeneration;
	auto& protectedBackendResourceGeneration = result.protectedBackendResourceGeneration;
	auto& protectedSceneChanges = result.protectedSceneChanges;
	auto& protectedSceneGeneration = result.protectedSceneGeneration;
	auto& animationPresentationFrame = result.animationPresentationFrame;
	auto& gpuPresentation = result.gpuPresentation;
	auto& immutableGpuAnimationPlan = result.immutableGpuAnimationPlan;
	auto& denseAnimationOwnerCount = result.denseAnimationOwnerCount;
	auto& hasDenseAnimationTopologyCandidate = result.hasDenseAnimationTopologyCandidate;
	protectedRendererGeneration = rhi
		? rhi->GetLatestRendererRecoverySnapshot().renderSessionGeneration
		: latestRendererRecoverySnapshot.renderSessionGeneration;
	// Snapshot the backend object generation beside the renderer session. A
	// presentation recovery may replace swapchain-compatible resources without
	// changing the session, so every sealed root needs both identities.
	protectedBackendResourceGeneration = rhi
		? rhi->GetBackendResourceGeneration() : 1u;
	// The scheduler owns the one scene-change drain.  Retaining its immutable
	// packet here makes renderer preparation observe the same protected boundary
	// as Scene View and other consumers, without touching mutable scene journals.
	protectedSceneChanges = objManager.LoadPublishedSceneChangePacketShared();
	protectedSceneGeneration =
		protectedSceneChanges != nullptr
			&& protectedSceneChanges->SceneGeneration() != 0u
			? protectedSceneChanges->SceneGeneration()
			: objManager.GetSceneGeneration();
	// Acquire one post-physics presentation root. Its strong authority reference
	// and backend-neutral GPU plan prevent this renderer operation from pairing a
	// newer pose/authority row with an older owner-to-packet lookup.
	animationPresentationFrame = TerraEngine::Animation::AnimationPresentationSystem::Instance()
			.LoadPublishedAnimationPresentationFrame();
	gpuPresentation = animationPresentationFrame != nullptr
			&& animationPresentationFrame->IsValid()
			? animationPresentationFrame->gpuPlan : nullptr;
	if(gpuPresentation != nullptr && !gpuPresentation->safeFallback
		&& gpuPresentation->loweringStatus == TerraEngine::Animation::GpuAnimationLoweringStatus::Ok
		&& gpuPresentation->planStatus == TerraEngine::Animation::GpuAnimationPlanStatus::Ok
		&& gpuPresentation->presentationGeneration != 0U
		&& gpuPresentation->sourceGeneration != 0U
		&& gpuPresentation->plan.generation != 0U
		&& !gpuPresentation->plan.WorkPackets().empty()) {
		immutableGpuAnimationPlan = std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan>(
			gpuPresentation, &gpuPresentation->plan);
	}
	denseAnimationOwnerCount = gpuPresentation != nullptr
		? gpuPresentation->OwnerObjectMappings().size() : 0U;
	hasDenseAnimationTopologyCandidate = immutableGpuAnimationPlan != nullptr
		&& denseAnimationOwnerCount != 0U
		&& denseAnimationOwnerCount == immutableGpuAnimationPlan->WorkPackets().size()
		&& immutableGpuAnimationPlan->topologyInputSignature != 0U
		&& immutableGpuAnimationPlan->topologyRendererGeneration != 0U;
	return result;
}

RenderSystem::FrameSceneRevisions RenderSystem::CaptureFrameSceneRevisions()
{
	FrameSceneRevisions result;
	auto& capturedSceneGeneration = result.capturedSceneGeneration;
	auto& capturedSpatialRevision = result.capturedSpatialRevision;
	auto& capturedComponentMutationRevision = result.capturedComponentMutationRevision;
	auto& capturedMeshComponentRevision = result.capturedMeshComponentRevision;
	auto& capturedTerrainComponentRevision = result.capturedTerrainComponentRevision;
	auto& capturedTerrainProxyGeneration = result.capturedTerrainProxyGeneration;
	auto& capturedRenderBindingRevision = result.capturedRenderBindingRevision;
	auto& capturedHierarchyRevision = result.capturedHierarchyRevision;
	auto& capturedActiveMembershipRevision = result.capturedActiveMembershipRevision;
	auto& capturedAnimatorComponentRevision = result.capturedAnimatorComponentRevision;
	capturedSceneGeneration = objManager.GetSceneGeneration();
	capturedSpatialRevision = objManager.GetSpatialRevision();
	capturedComponentMutationRevision =
		objManager.GetComponentMutationRevision();
	capturedMeshComponentRevision =
		objManager.GetComponentValueMutationRevision(
			MeshRenderComponent::kComponentName);
	capturedTerrainComponentRevision =
		objManager.GetComponentValueMutationRevision(
			TerrainComponent::kComponentName);
	capturedTerrainProxyGeneration =
		TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
			.PublicationGeneration();
	// Existing cache records call this the mesh-component revision. Fold every
	// source of a resolved render binding into one token so TerrainComponent and
	// proxy changes invalidate the same topology, spatial, and draw caches.
	capturedRenderBindingRevision = kRenderHashOffset;
	HashCombine(capturedRenderBindingRevision, capturedMeshComponentRevision);
	HashCombine(capturedRenderBindingRevision, capturedTerrainComponentRevision);
	HashCombine(capturedRenderBindingRevision, capturedTerrainProxyGeneration);
	capturedHierarchyRevision = objManager.GetHierarchyRevision();
	capturedActiveMembershipRevision =
		objManager.GetRuntimeActiveMembershipRevision();
	capturedAnimatorComponentRevision =
		objManager.GetComponentValueMutationRevision(
			AnimatorComponent::kComponentName);
	return result;
}

RenderSystem::FrameResourceCohort RenderSystem::CaptureFrameResourceCohort(
	std::uint64_t protectedRendererResourceOwnerRevision)
{
	FrameResourceCohort result;
	auto& resourceManager = ResourceManager::Instance();
	auto& capturedDefaultShaderCollectionId = result.capturedDefaultShaderCollectionId;
	auto& capturedDefaultTaskShaderCollectionId = result.capturedDefaultTaskShaderCollectionId;
	auto& capturedShadowMeshCollection = result.capturedShadowMeshCollection;
	auto& capturedShadowTaskCollection = result.capturedShadowTaskCollection;
	auto& capturedRendererResourceRevision = result.capturedRendererResourceRevision;
	auto& rendererResourceOwnerCohortStable = result.rendererResourceOwnerCohortStable;
	auto& capturedAnimationBindingResourceRevision = result.capturedAnimationBindingResourceRevision;
	auto& capturedVirtualGeometryResidencyGeneration = result.capturedVirtualGeometryResidencyGeneration;
	auto& capturedRenderSettings = result.capturedRenderSettings;
	auto& capturedEfficiencyOptions = result.capturedEfficiencyOptions;
	// Close the same protected loader cohort after retaining shader, mesh and
	// material owners. A concurrent publication disables snapshot reuse rather
	// than mixing a newer shader owner with an older reusable capture signature.
	capturedDefaultShaderCollectionId =
		resourceManager.GetDefaultShaderCollectionID();
	capturedDefaultTaskShaderCollectionId =
		resourceManager.GetDefaultTaskShaderCollectionID();
	capturedShadowMeshCollection = resourceManager.AcquireShaderCollectionSnapshot(capturedDefaultShaderCollectionId);
	capturedShadowTaskCollection = capturedDefaultTaskShaderCollectionId == capturedDefaultShaderCollectionId
		? capturedShadowMeshCollection : resourceManager.AcquireShaderCollectionSnapshot(capturedDefaultTaskShaderCollectionId);
	capturedRendererResourceRevision =
		resourceManager.GetRendererResourcePublicationGeneration();
	rendererResourceOwnerCohortStable = protectedRendererResourceOwnerRevision == capturedRendererResourceRevision;
	capturedAnimationBindingResourceRevision =
		resourceManager.GetAnimationBindingResourceRevision();
	capturedVirtualGeometryResidencyGeneration =
		TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
			.ResidencySnapshot().publicationGeneration;
	capturedRenderSettings =
		SanitizeRenderSettings(currentSettings);
	capturedEfficiencyOptions = GetRendererEfficiencyOptionsSnapshot();
	return result;
}

namespace TerraEngine::Renderer::Frame {
	void BuildCpuDeformationContentIdentity(
		TerraEngine::Renderer::CpuAnimationDeformationData& deformation) noexcept
	{
		// Hash the exact shader-facing bytes, including counts, with two unrelated
		// update functions. Sharing is admitted only when both values match, making
		// accidental aliasing substantially less likely than a single generation id.
		std::uint64_t low = kRenderHashOffset;
		std::uint64_t high = 0x9e3779b97f4a7c15ULL;
		const auto mixBytes = [&low, &high](const void* source, std::size_t byteCount)
		{
			const auto* bytes = static_cast<const std::uint8_t*>(source);
			for(std::size_t index = 0U; index < byteCount; ++index)
			{
				low = (low ^ bytes[index]) * kRenderHashPrime;
				high ^= static_cast<std::uint64_t>(bytes[index])
					+ 0x9e3779b97f4a7c15ULL + (high << 6U) + (high >> 2U);
			}
		};
		mixBytes(&deformation.jointCount, sizeof(deformation.jointCount));
		if(!deformation.paletteMatrices.empty())
		{
			mixBytes(deformation.paletteMatrices.data(),
				deformation.paletteMatrices.size()
					* sizeof(deformation.paletteMatrices.front()));
		}
		if(!deformation.morphWeights.empty())
		{
			mixBytes(deformation.morphWeights.data(),
				deformation.morphWeights.size() * sizeof(float));
		}
		deformation.contentIdLow = low == 0U ? 1U : low;
		deformation.contentIdHigh = high == 0U ? 1U : high;
	}
}

// Build immutable CPU deformation bytes against the captured authority owner.
std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>
RenderSystem::BuildCpuAnimationDeformation(
	const TerraEngine::Animation::AnimationAuthorityRow& authorityRow,
	std::span<const std::uint32_t> paletteJoints,
	std::span<const std::uint64_t> morphTargetIds) noexcept
{
	using namespace TerraEngine::Animation;
	using TerraEngine::Renderer::CpuAnimationDeformationData;
	if(authorityRow.safeFallback || authorityRow.status != AnimationStatus::Ok
		|| authorityRow.snapshot == nullptr || authorityRow.snapshot->pose == nullptr
		|| authorityRow.skeletonAsset == nullptr
		|| authorityRow.skeletonCompatibilityFingerprint == 0U
		|| authorityRow.skeletonAsset->compatibilityFingerprint
			!= authorityRow.skeletonCompatibilityFingerprint
		|| paletteJoints.empty()
		|| paletteJoints.size() > kGpuAnimationPaletteEntriesPerPartition
		|| morphTargetIds.size() > kGpuAnimationMaximumMorphTargetsV1) {
		return {};
	}

	const PoseBuffer& pose = *authorityRow.snapshot->pose;
	const SkeletonAsset& skeleton = *authorityRow.skeletonAsset;
	const std::span<const AnimationTransform> modelTransforms =
		pose.ModelTransforms();
	if(modelTransforms.empty()
		|| modelTransforms.size() != authorityRow.jointStableIds.size()
		|| skeleton.joints.size() != skeleton.inverseBindMatrices.size()) {
		return {};
	}

	enum class InverseBindStorage
	{
		Unknown,
		RowMajor,
		ColumnMajor
	};
	std::string sourceExtension =
		std::filesystem::path(skeleton.source.sourcePackagePath)
			.extension().string();
	std::transform(sourceExtension.begin(), sourceExtension.end(),
		sourceExtension.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
	const InverseBindStorage inverseBindStorage = sourceExtension == ".fbx"
		? InverseBindStorage::RowMajor
		: (sourceExtension == ".gltf" || sourceExtension == ".glb"
			? InverseBindStorage::ColumnMajor
			: InverseBindStorage::Unknown);

	const auto isIdentityMatrix = [](const AnimationMatrix4x4& matrix) noexcept {
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
	};
	const auto convertInverseBind = [inverseBindStorage, &isIdentityMatrix](
		const AnimationMatrix4x4& source,
		std::array<float, 16U>& output) noexcept {
		// Version-one skeletons predate an explicit matrix-major field. Match the
		// importer's frozen FBX/glTF convention and accept unknown provenance only
		// for identity, whose transpose is unambiguous.
		if(inverseBindStorage == InverseBindStorage::Unknown
			&& !isIdentityMatrix(source)) {
			return false;
		}
		for(std::size_t row = 0U; row < 4U; ++row) {
			for(std::size_t column = 0U; column < 4U; ++column) {
				const std::size_t sourceIndex =
					inverseBindStorage == InverseBindStorage::ColumnMajor
						? column * 4U + row : row * 4U + column;
				const float value = source.values[sourceIndex];
				if(!std::isfinite(value)) return false;
				output[row * 4U + column] = value;
			}
		}
		return std::abs(output[12U]) <= 1.0e-4F
			&& std::abs(output[13U]) <= 1.0e-4F
			&& std::abs(output[14U]) <= 1.0e-4F
			&& std::abs(output[15U] - 1.0F) <= 1.0e-4F;
	};
	const auto transformMatrix = [](const AnimationTransform& transform,
		std::array<float, 16U>& output) noexcept {
		if(!IsFinite(transform)) return false;
		const float lengthSquared =
			transform.rotation.x * transform.rotation.x
			+ transform.rotation.y * transform.rotation.y
			+ transform.rotation.z * transform.rotation.z
			+ transform.rotation.w * transform.rotation.w;
		if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) {
			return false;
		}
		const float inverseLength = 1.0F / std::sqrt(lengthSquared);
		const float x = transform.rotation.x * inverseLength;
		const float y = transform.rotation.y * inverseLength;
		const float z = transform.rotation.z * inverseLength;
		const float w = transform.rotation.w * inverseLength;
		const float xx = x * x;
		const float yy = y * y;
		const float zz = z * z;
		const float xy = x * y;
		const float xz = x * z;
		const float yz = y * z;
		const float wx = w * x;
		const float wy = w * y;
		const float wz = w * z;
		// T * R * S scales the rotation columns. The shader consumes row-major
		// affine rows and applies dot(row.xyz, position) + row.w.
		output = {
			(1.0F - 2.0F * (yy + zz)) * transform.scale.x,
			(2.0F * (xy - wz)) * transform.scale.y,
			(2.0F * (xz + wy)) * transform.scale.z,
			transform.translation.x,
			(2.0F * (xy + wz)) * transform.scale.x,
			(1.0F - 2.0F * (xx + zz)) * transform.scale.y,
			(2.0F * (yz - wx)) * transform.scale.z,
			transform.translation.y,
			(2.0F * (xz - wy)) * transform.scale.x,
			(2.0F * (yz + wx)) * transform.scale.y,
			(1.0F - 2.0F * (xx + yy)) * transform.scale.z,
			transform.translation.z,
			0.0F, 0.0F, 0.0F, 1.0F};
		return true;
	};
	const auto multiply = [](const std::array<float, 16U>& left,
		const std::array<float, 16U>& right,
		std::array<float, 16U>& output) noexcept {
		for(std::size_t row = 0U; row < 4U; ++row) {
			for(std::size_t column = 0U; column < 4U; ++column) {
				float value = 0.0F;
				for(std::size_t lane = 0U; lane < 4U; ++lane) {
					value += left[row * 4U + lane]
						* right[lane * 4U + column];
				}
				if(!std::isfinite(value)) return false;
				output[row * 4U + column] = value;
			}
		}
		return true;
	};

	try {
		auto deformation = std::make_shared<CpuAnimationDeformationData>();
		deformation->jointCount = static_cast<std::uint32_t>(
			std::min<std::size_t>(paletteJoints.size(),
				kGpuAnimationPaletteEntriesPerPartition));
		deformation->paletteMatrices.reserve(paletteJoints.size());
		for(const std::uint32_t skeletonJointIndex : paletteJoints) {
			if(skeletonJointIndex >= skeleton.joints.size()) return {};
			const std::uint64_t stableJointId =
				skeleton.joints[skeletonJointIndex].stableId;
			const auto poseJoint = std::find(authorityRow.jointStableIds.begin(),
				authorityRow.jointStableIds.end(), stableJointId);
			if(poseJoint == authorityRow.jointStableIds.end()) return {};
			const std::size_t poseJointIndex = static_cast<std::size_t>(
				std::distance(authorityRow.jointStableIds.begin(), poseJoint));
			if(poseJointIndex >= modelTransforms.size()) return {};

			std::array<float, 16U> model{};
			std::array<float, 16U> inverseBind{};
			std::array<float, 16U> skinning{};
			if(!transformMatrix(modelTransforms[poseJointIndex], model)
				|| !convertInverseBind(
					skeleton.inverseBindMatrices[skeletonJointIndex],
					inverseBind)
				|| !multiply(model, inverseBind, skinning)) {
				return {};
			}
			GpuAnimationPaletteRecord matrix{};
			std::copy_n(skinning.begin(), 4U, matrix.row0.begin());
			std::copy_n(skinning.begin() + 4U, 4U, matrix.row1.begin());
			std::copy_n(skinning.begin() + 8U, 4U, matrix.row2.begin());
			deformation->paletteMatrices.push_back(matrix);
		}

		const std::span<const AnimationNamedFloatValue> publishedMorphs =
			pose.MorphValues();
		deformation->morphWeights.reserve(morphTargetIds.size());
		for(const std::uint64_t targetId : morphTargetIds) {
			const auto value = std::find_if(publishedMorphs.begin(),
				publishedMorphs.end(), [targetId](
					const AnimationNamedFloatValue& candidate) {
					return candidate.stableId == targetId;
				});
			const float weight =
				value == publishedMorphs.end() ? 0.0F : value->value;
			if(!std::isfinite(weight)) return {};
			deformation->morphWeights.push_back(weight);
		}
		BuildCpuDeformationContentIdentity(*deformation);
		if(!deformation->IsComplete()) return {};
		return deformation;
	} catch(const std::bad_alloc&) {
		return {};
	}
}

namespace TerraEngine::Renderer::Frame {
	std::uint64_t BuildStableAnimationInstanceId(
		const TerraEngine::Renderer::AnimationRenderBinding& binding,
		const TerraEngine::UUID& objectId)
	{
		// Animator slot plus generation is the same stable value used by animation
		// planning. The object hash is only a nonzero fallback for an explicit safe
		// bind pose that has no live instance identity.
		if(binding.animatorGeneration != 0U
			&& binding.animatorSlotIndex != TerraEngine::Animation::kInvalidAnimatorInstanceSlot) {
			return (static_cast<std::uint64_t>(binding.animatorSlotIndex) << 32U)
				| static_cast<std::uint64_t>(binding.animatorGeneration);
		}
		return std::max<std::uint64_t>(1U,
			static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(objectId)));
	}
}

// Build and deduplicate owned CPU bridge data while the captured resource roots live.
std::shared_ptr<TerraEngine::Renderer::CpuAnimationDeformationPlan>
RenderSystem::BuildFrameCpuDeformationPlan(std::span<RenderableEntry> renderables,
	bool hasCpuDeformationCandidates, std::uint64_t protectedRendererGeneration,
	std::vector<std::uint32_t>& cpuDeformationRenderableIndices)
{
	std::shared_ptr<TerraEngine::Renderer::CpuAnimationDeformationPlan>
		cpuDeformationPlan;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build CPU Animation Deformation Plan",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if(hasCpuDeformationCandidates)
	{
		cpuDeformationPlan =
			std::make_shared<TerraEngine::Renderer::CpuAnimationDeformationPlan>();
		cpuDeformationPlan->frameId = activeRenderFrameId;
		cpuDeformationPlan->rendererGeneration = protectedRendererGeneration;
	}
	struct CpuDeformationReuseKey
	{
		std::uint64_t visualPoseContentIdLow = 0U;
		std::uint64_t visualPoseContentIdHigh = 0U;
		TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
		std::uint64_t deformablePayloadGeneration = 0U;
		std::uint64_t skeletonCompatibilityFingerprint = 0U;

		bool operator==(const CpuDeformationReuseKey&) const = default;
	};
	struct CpuDeformationReuseKeyHash
	{
		std::size_t operator()(const CpuDeformationReuseKey& key) const noexcept
		{
			std::uint64_t hash = kRenderHashOffset;
			HashCombine(hash, key.visualPoseContentIdLow);
			HashCombine(hash, key.visualPoseContentIdHigh);
			HashCombine(hash, static_cast<std::uint64_t>(
				std::hash<TerraEngine::UUID>{}(key.meshId)));
			HashCombine(hash, key.deformablePayloadGeneration);
			HashCombine(hash, key.skeletonCompatibilityFingerprint);
			return static_cast<std::size_t>(hash);
		}
	};
	// This cache lasts only for the immutable frame. Synchronized visual clones
	// keep instance-specific pose publications but avoid repeating palette matrix
	// construction for the same model-space pose and exact mesh generation.
	std::unordered_map<CpuDeformationReuseKey,
		std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>,
		CpuDeformationReuseKeyHash> cpuDeformationByVisualPose;
	if(hasCpuDeformationCandidates)
	{
		cpuDeformationByVisualPose.reserve(renderables.size());
	}
	const std::span<RenderableEntry> cpuDeformationCandidates =
		hasCpuDeformationCandidates
			? std::span<RenderableEntry>(renderables.data(), renderables.size())
			: std::span<RenderableEntry>{};
	for(RenderableEntry& entry : cpuDeformationCandidates) {
		TerraEngine::Renderer::AnimationRenderBinding& binding =
			entry.animationBinding;
		// Hybrid/GPU bindings already own a compute-produced palette. Only the
		// CPU deterministic/fallback path needs the host-upload bridge.
		if(binding.gpu.IsComplete() || !binding.HasCpuPose()
			|| binding.authorityRow == nullptr || protectedRendererGeneration == 0U) {
			continue;
		}
		// Collection already resolved this exact resource under the same protected
		// boundary. Reuse it instead of reopening ResourceManager for each of the
		// few CPU-authoritative members of an otherwise GPU-visual crowd.
		const Mesh* mesh = entry.runtimeMesh;
		if(mesh == nullptr
			|| mesh->deformablePayloadGeneration
				!= binding.deformablePayloadGeneration
			|| ResolveDeformableSkeletonFingerprint(
					*mesh, binding.authorityRow)
				!= binding.skeletonCompatibilityFingerprint) {
			continue;
		}
		std::vector<std::uint64_t> morphTargetIds;
		try {
			morphTargetIds.reserve(mesh->deformableMorphs.size());
			for(const TemfDeformableMorphRecord& morph : mesh->deformableMorphs) {
				morphTargetIds.push_back(morph.stableTargetId);
			}
		} catch(const std::bad_alloc&) {
			continue;
		}
		std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationData>
			deformation;
		const CpuDeformationReuseKey reuseKey{
			binding.authorityRow->visualPoseContentIdLow,
			binding.authorityRow->visualPoseContentIdHigh,
			binding.meshId,
			binding.deformablePayloadGeneration,
			binding.skeletonCompatibilityFingerprint};
		if(reuseKey.visualPoseContentIdLow != 0U
			&& reuseKey.visualPoseContentIdHigh != 0U) {
			const auto reused = cpuDeformationByVisualPose.find(reuseKey);
			if(reused != cpuDeformationByVisualPose.end()) {
				deformation = reused->second;
			}
		}
		if(deformation == nullptr) {
			deformation = BuildCpuAnimationDeformation(
				*binding.authorityRow, mesh->deformablePaletteJoints, morphTargetIds);
			if(deformation != nullptr && deformation->IsComplete()
				&& reuseKey.visualPoseContentIdLow != 0U
				&& reuseKey.visualPoseContentIdHigh != 0U) {
				cpuDeformationByVisualPose.emplace(reuseKey, deformation);
			}
		}
		if(deformation == nullptr || !deformation->IsComplete()
			|| cpuDeformationPlan->records.size()
				>= std::numeric_limits<std::uint32_t>::max()) {
			continue;
		}
		binding.cpuDeformation = deformation;
		binding.deformationContentIdLow = deformation->contentIdLow;
		binding.deformationContentIdHigh = deformation->contentIdHigh;
		binding.cpuDeformationRecordIndex = static_cast<std::uint32_t>(
			cpuDeformationPlan->records.size());
		const std::size_t renderableIndex = static_cast<std::size_t>(
			std::addressof(entry) - renderables.data());
		if(renderableIndex < renderables.size()
			&& renderableIndex <= std::numeric_limits<std::uint32_t>::max()) {
			cpuDeformationRenderableIndices.push_back(
				static_cast<std::uint32_t>(renderableIndex));
		}
		cpuDeformationPlan->records.push_back({
			BuildStableAnimationInstanceId(binding, entry.objectId),
			binding.meshId,
			binding.posePublicationGeneration,
			binding.deformablePayloadGeneration,
			protectedRendererGeneration,
			binding.skeletonCompatibilityFingerprint,
			deformation});
	}
	if(cpuDeformationPlan != nullptr && cpuDeformationPlan->records.empty()) {
		cpuDeformationPlan.reset();
	}
	}
	return cpuDeformationPlan;
}

// Preserve the protected no-output retirement path before the first suspension.
void RenderSystem::CompleteFrameWithoutViews(
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	std::uint64_t protectedSceneGeneration, std::uint64_t snapshotRendererGeneration,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities)
{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Skip No Render Outputs", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	TerraEngine::Renderer::RenderDiagnosticsSummary noViewDiagnostics{};
	noViewDiagnostics.frameId = activeRenderFrameId;
	const TerraEngine::Renderer::RendererRecoverySnapshot recoverySnapshot = rhi
		? rhi->GetLatestRendererRecoverySnapshot()
		: latestRendererRecoverySnapshot;
	TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(noViewDiagnostics, recoverySnapshot);
	if (!TerraEngine::Renderer::CanSubmitNormalGpuWork(recoverySnapshot))
	{
		ReportFatalRendererRecoveryFailure(recoverySnapshot);
	}
	const TerraEngine::Renderer::RendererRecoverySnapshot finalRecoverySnapshot = rhi
		? rhi->GetLatestRendererRecoverySnapshot()
		: latestRendererRecoverySnapshot;
	TerraEngine::Renderer::ApplyRendererRecoveryDiagnostics(noViewDiagnostics, finalRecoverySnapshot);
	if (TerraEngine::Renderer::RenderDiagnosticsRequested())
	{
		noViewDiagnostics.delayedWork.push_back("Render frame skipped because no Scene, Game, or runtime output target was requested.");
	}
	if(rhi)
	{
		TerraEngine::Renderer::ApplyVirtualGeometryCapabilityDiagnostics(
			noViewDiagnostics,
			CollectVirtualGeometryCapabilityDiagnostics(
				*rhi,
				backendCapabilities,
				{
					activeRenderFrameId,
					0u,
					snapshotRendererGeneration
				}));
	}
	TerraEngine::Renderer::PublishRenderDiagnostics(noViewDiagnostics);
	// A valid empty publication lets visual-only animation sleep while every
	// output is closed. Scene generation still guards against stale project data.
	TerraEngine::Renderer::AnimatedObjectVisibilitySnapshot emptyVisibility;
	emptyVisibility.frameId = activeRenderFrameId;
	emptyVisibility.sceneGeneration = protectedSceneGeneration;
	emptyVisibility.hasActiveView = false;
	emptyVisibility.valid = true;
	emptyVisibility.canonicalUnique = true;
	TerraEngine::Renderer::PublishAnimatedObjectVisibilitySnapshot(
		std::move(emptyVisibility));
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		std::swap(latestRenderFrameSnapshot, renderFrameSnapshot);
		recycledRenderFrameSnapshot = std::move(renderFrameSnapshot);
		latestRendererRecoverySnapshot = finalRecoverySnapshot;
		latestRenderGraphDiagnostics = noViewDiagnostics;
		latestGameVirtualGeometryVisibilityRecords.clear();
		latestSceneVirtualGeometryVisibilityRecords.clear();
		latestGameCaptureVisibilityRecords.clear();
		latestSceneCaptureVisibilityRecords.clear();
		latestGameCaptureIdentitySnapshot = {};
		latestSceneCaptureIdentitySnapshot = {};
		pendingGameIdentitySnapshot = {};
		pendingSceneIdentitySnapshot = {};
		pendingGameIdentityVisibilityRecords.clear();
		pendingSceneIdentityVisibilityRecords.clear();
		pendingGameAuthoritativeIdentityPixel.reset();
		pendingSceneAuthoritativeIdentityPixel.reset();
		virtualGeometryDebugModes.clear();
	}
	// Closing every output retires all view-local history immediately. A later
	// view may reuse the same numeric output ID, but it must begin with a fresh
	// Hi-Z policy and geometric-transition history.
	virtualGeometryViewPolicies.clear();
	virtualGeometryTransitionCache.clear();
	// Even when no view submits GPU work, streaming visibility needs still
	// have to age out. Without this cleanup pass, assets that were visible
	// on a previous Scene/Game View frame stay marked as active-view after
	// the user hides those views or disables every camera.
	ProcessQueuedCaptureRequests();
	DrainCaptureReadbacks();
	ResourceManager::Instance().RunResidencyCleanup(activeRenderFrameId);
	ClearOverrideCameras();
}

// Snapshot mutually exclusive controls and queue intent before worker-safe work.
RenderSystem::FrameBackendCapture RenderSystem::CaptureFrameBackendInputs()
{
	FrameBackendCapture result;
	auto& backendCapabilities = result.backendCapabilities;
	auto& hardwareVirtualGeometryRouteAvailable = result.hardwareVirtualGeometryRouteAvailable;
	auto& computeVirtualGeometryRouteAvailable = result.computeVirtualGeometryRouteAvailable;
	auto& deviceAuthoredVirtualGeometryAvailable = result.deviceAuthoredVirtualGeometryAvailable;
	auto& validationRouteOverride = result.validationRouteOverride;
	auto& hybridValidationSceneRequested = result.hybridValidationSceneRequested;
	auto& hasPendingDedicatedCaptureRender = result.hasPendingDedicatedCaptureRender;
	auto& pendingDedicatedCaptureIdentity = result.pendingDedicatedCaptureIdentity;
	auto& pendingDedicatedCaptureCanonicalOrigin = result.pendingDedicatedCaptureCanonicalOrigin;
	auto& pendingDedicatedCaptureTargetSize = result.pendingDedicatedCaptureTargetSize;
	backendCapabilities = rhi
		? rhi->GetBackendCapabilityProfile()
		: TerraEngine::Renderer::BackendCapabilityProfile{};
	// Route admission uses immutable device/bootstrap capability, while the
	// stronger supportsVirtualGeometry* stage flags remain fence-delayed proof
	// that work actually completed. Using completion evidence for admission
	// would deadlock a fresh editor: no route could run the first measured frame.
	hardwareVirtualGeometryRouteAvailable =
		rhi != nullptr && rhi->SupportsVirtualGeometryMeshShaderRoute();
	computeVirtualGeometryRouteAvailable =
		backendCapabilities.supportsVirtualGeometryGpuPacketBuffers;
	deviceAuthoredVirtualGeometryAvailable =
		backendCapabilities.supportsVirtualGeometryGpuCompaction
		&& backendCapabilities.supportsVirtualGeometryGeneratedSubmissions;
	// Snapshot the mutually exclusive validation controls once before worker-safe
	// preparation. Packet values, rather than mutable editor state, cross the
	// protected Vulkan recording boundary later in this frame.
	hybridValidationSceneRequested = false;
	{
		std::lock_guard<std::mutex> lock(virtualGeometryValidationMutex);
		validationRouteOverride = virtualGeometryValidationRouteOverride;
		hybridValidationSceneRequested = virtualGeometryHybridValidationSceneRequested;
	}
	hasPendingDedicatedCaptureRender = false;
	pendingDedicatedCaptureIdentity =
		TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
			RenderViewType::Game);
	pendingDedicatedCaptureTargetSize = captureLimits.defaultWidth > 0u && captureLimits.defaultHeight > 0u
		? glm::uvec2(captureLimits.defaultWidth, captureLimits.defaultHeight)
		: glm::uvec2(1280u, 720u);
	if (backendCapabilities.supportsImageReadback)
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (const TerraEngine::Renderer::RenderCaptureRequest& captureRequest : pendingCaptureRequests)
		{
			if ((captureRequest.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
					|| captureRequest.dedicatedRenderTarget)
				&& captureRequest.format == "image/png")
			{
				hasPendingDedicatedCaptureRender = true;
				pendingDedicatedCaptureIdentity =
					TerraEngine::Renderer::ResolveDedicatedCaptureViewIdentity(
						captureRequest.viewType);
				pendingDedicatedCaptureTargetSize = captureRequest.targetSize;
				pendingDedicatedCaptureCanonicalOrigin =
					captureRequest.cameraCanonicalPosition;
				break;
			}
		}
	}
	return result;
}

// Resolve and copy the authored sky before any worker-safe preparation.
RenderSystem::FrameSkyCapture RenderSystem::CaptureFrameSky(std::uint64_t capturedHierarchyRevision)
{
	FrameSkyCapture result;
	auto& resourceManager = ResourceManager::Instance();
	auto& capturedSkyComponent = result.capturedSkyComponent;
	auto& capturedSkyObjectId = result.capturedSkyObjectId;
	auto& capturedSkyTextureId = result.capturedSkyTextureId;
	{
		const std::uint64_t skyHierarchyRevision = capturedHierarchyRevision;
		std::uint64_t skyCandidateSignature = 0U;
		const bool observeSkyCandidates = skyStateDirty
			|| !hasCachedSkyObjectResolution
			|| cachedSkyHierarchyRevision != skyHierarchyRevision;
		if(observeSkyCandidates)
		{
			const auto skyCandidates = objManager.getRegistry().view<SkyComponent>();
			for(const entt::entity entity : skyCandidates)
			{
				const std::optional<Object> object = objManager.getObject(entity);
				const SkyComponent* component = object
					? objManager.GetComponent<SkyComponent>(*object) : nullptr;
				if(component == nullptr) continue;
				const bool objectActive = objManager.isObjectActiveInHierarchy(*object);
				const std::uint64_t entityValue = static_cast<std::uint32_t>(entity);
				const std::uint64_t candidateBits = entityValue
					^ (component->active ? 0x9e3779b97f4a7c15ULL : 0ULL)
					^ (objectActive ? 0xc2b2ae3d27d4eb4fULL : 0ULL);
				skyCandidateSignature ^= candidateBits + 0x9e3779b97f4a7c15ULL
					+ (skyCandidateSignature << 6U) + (skyCandidateSignature >> 2U);
			}
		}
		const bool skyCandidatesChanged = skyStateDirty
			&& skyCandidateSignature != cachedSkyCandidateSignature;
		if(!hasCachedSkyObjectResolution
			|| cachedSkyHierarchyRevision != skyHierarchyRevision
			|| skyCandidatesChanged)
		{
			cachedSkyObject = SceneManager::Instance().GetActiveSkyObject();
			hasCachedSkyObjectResolution = true;
			cachedSkyHierarchyRevision = skyHierarchyRevision;
			cachedSkyCandidateSignature = skyCandidateSignature;
		}
		const std::optional<Object> activeSkyObject = cachedSkyObject.has_value()
			? objManager.getObject(*cachedSkyObject) : std::nullopt;
		if(activeSkyObject.has_value())
		{
			if(const SkyComponent* component =
				objManager.GetComponent<SkyComponent>(*activeSkyObject))
			{
				capturedSkyComponent = *component;
				capturedSkyObjectId = objManager.getUUID(*activeSkyObject);
				if(!component->sourceAsset.empty())
				{
					capturedSkyTextureId = resourceManager.ResolveAssetReference(
						component->sourceAsset, AssetType::Texture);
				}
			}
		}
	}
	return result;
}

// Copy live object lifetime observations only while protected capture owns ECS.
std::vector<std::uint64_t> RenderSystem::CaptureFrameShadowLifetimes(
	std::span<const RenderableEntry> renderables, bool reuseRenderableRecords)
{
	std::vector<std::uint64_t> capturedShadowObjectLifetimes;
	if(!reuseRenderableRecords) {
		capturedShadowObjectLifetimes.resize(renderables.size());
		for(std::size_t index = 0U; index < renderables.size(); ++index) {
			if(const auto object = objManager.getObject(renderables[index].entity); object.has_value())
				capturedShadowObjectLifetimes[index] = objManager.getObjectLifetimeGeneration(*object);
		}
	}
	return capturedShadowObjectLifetimes;
}

// Capture completed backend evidence in the original protected order.
RenderSystem::FrameBackendObservations RenderSystem::CaptureFrameBackendObservations()
{
	FrameBackendObservations result;
	auto& completedVirtualGeometryCounters = result.completedVirtualGeometryCounters;
	auto& capturedBackendBreakdown = result.capturedBackendBreakdown;
	auto& capturedRecoverySnapshot = result.capturedRecoverySnapshot;
	auto& capturedUiBatch = result.capturedUiBatch;
	auto& capturedVirtualGeometryStreamingPlan = result.capturedVirtualGeometryStreamingPlan;
	completedVirtualGeometryCounters = rhi
		? rhi->PollVirtualGeometryReplayCounters()
		: std::vector<TerraEngine::Renderer::VirtualGeometryAuthoritativeCounterReadback>{};
	capturedBackendBreakdown = rhi
		? rhi->GetLatestBackendFrameBreakdown()
		: TerraEngine::Renderer::BackendFrameBreakdown{};
	capturedRecoverySnapshot = rhi
		? rhi->GetLatestRendererRecoverySnapshot()
		: latestRendererRecoverySnapshot;
	capturedUiBatch = UISystem::Instance().GetCombinedFrameBatch();
	capturedVirtualGeometryStreamingPlan =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance()
				.LastPreparedPlan();
	return result;
}

// Validation observes actual owned collection values before children can borrow them.
TerraEngine::Renderer::RenderFrameScratchDiagnostics RenderSystem::ObserveFrameCapturedInputs(
	const std::pmr::vector<CameraEntry>& cameras, const std::pmr::vector<LightEntry>& lights,
	const std::shared_ptr<const TerraEngine::Renderer::RenderFrameScratchValidationCallbacks>& scratchValidation)
{
	TerraEngine::Renderer::RenderFrameScratchDiagnostics capturedScratchDiagnostics;
	if(scratchValidation && scratchValidation->onEvent) {
		auto inputs = std::make_shared<TerraEngine::Renderer::RenderFrameScratchCapturedInputs>();
		inputs->cameras.reserve(cameras.size());
		for(const auto& entry : cameras) {
			const auto object = objManager.getObject(entry.entity);
			if(!object) throw std::logic_error("Captured camera identity retired before protected validation");
			inputs->cameras.push_back({objManager.getUUID(*object), entry.camera});
		}
		inputs->lights.reserve(lights.size());
		for(const auto& entry : lights) inputs->lights.push_back({entry.lightId, entry.light});
		capturedScratchDiagnostics = lifetimeState->frameScratch->Snapshot();
		TerraEngine::Renderer::RenderFrameScratchValidationEvent event;
		event.checkpoint = TerraEngine::Renderer::RenderFrameScratchValidationCheckpoint::Captured;
		event.episode = capturedScratchDiagnostics;
		event.capturedInputs = std::move(inputs);
		scratchValidation->onEvent(event);
	}
	return capturedScratchDiagnostics;
}

// Copy terrain bindings and durable object associations without retaining live ECS.
RenderSystem::FrameTerrainCapture RenderSystem::CaptureFrameTerrainInputs(
	std::span<const RenderableEntry> renderables)
{
	FrameTerrainCapture result;
	auto& frameTerrainMaterialBindings = result.frameTerrainMaterialBindings;
	auto& frameTerrainObjects = result.frameTerrainObjects;
	frameTerrainMaterialBindings.reserve(renderables.size());
	for(const RenderableEntry& renderable : renderables)
	{
		if(renderable.terrainAssetId == TerraEngine::EMPTY_UUID || renderable.mesh == nullptr
			|| frameTerrainMaterialBindings.contains(renderable.mesh->materialId))
		{
			continue;
		}
		if(const auto material = TerraEngine::Renderer::Terrain::
			TerrainMaterialRuntimeRegistry::Instance().Resolve(renderable.mesh->materialId);
			material && material->IsComplete())
		{
			frameTerrainMaterialBindings.emplace(renderable.mesh->materialId, *material);
		}
	}
	// Command preparation retains object identity, so this compact value map is
	// enough to join visible commands back to the already copied terrain binding.
	// No worker callback receives an ECS entity or registry pointer.
	frameTerrainObjects.reserve(frameTerrainMaterialBindings.size());
	for(const RenderableEntry& renderable : renderables)
	{
		if(renderable.terrainAssetId != TerraEngine::EMPTY_UUID && renderable.mesh != nullptr)
		{
			frameTerrainObjects.emplace(renderable.objectId,
				std::pair{renderable.terrainAssetId, renderable.mesh->materialId});
		}
	}

	return result;
}

// Retire held CPU pose owners against the completed protected membership set.
void RenderSystem::RetireFrameAnimationPoseOwners(
	const std::unordered_set<TerraEngine::UUID>& aliveCompatibleAnimationObjects)
{
	constexpr std::size_t kCompatibleAnimationPoseCacheCapacity = 4096U;
	// Scene/object/component retirement removes held ownership immediately. The
	// hard cap is a second guard for extremely large scenes and editor churn.
	for(auto cache = compatibleAnimationPoseCache.begin();
		cache != compatibleAnimationPoseCache.end();) {
		if(!aliveCompatibleAnimationObjects.contains(cache->first)) {
			cache = compatibleAnimationPoseCache.erase(cache);
		} else {
			++cache;
		}
	}
	while(compatibleAnimationPoseCache.size() > kCompatibleAnimationPoseCacheCapacity) {
		auto victim = std::min_element(compatibleAnimationPoseCache.begin(),
			compatibleAnimationPoseCache.end(), [](const auto& lhs, const auto& rhs) {
				if(lhs.second.lastTouchedFrameId != rhs.second.lastTouchedFrameId) {
					return lhs.second.lastTouchedFrameId < rhs.second.lastTouchedFrameId;
				}
				return TerraEngine::ToHexString(lhs.first) < TerraEngine::ToHexString(rhs.first);
			});
		if(victim == compatibleAnimationPoseCache.end()) {
			break;
		}
		compatibleAnimationPoseCache.erase(victim);
	}

}

// Borrow exact copied transforms, never live component storage, across suspensions.
RenderSystem::FrameWorldTransforms RenderSystem::CaptureFrameWorldTransforms()
{
	FrameWorldTransforms result;
	auto& worldTransforms = result.worldTransforms;
	auto& transformPropagation = result.transformPropagation;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Resolve World Transforms", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		worldTransforms = &worldTransformCache.Refresh(objManager, latestTransformCacheDiagnostics);
		transformPropagation = &worldTransformCache.LatestPropagation();
		if (!transformPropagation->valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, "RenderSystem received an invalid scene transform propagation result.");
		}
	}

	return result;
}

const TerraEngine::Scene::WorldPose& RenderSystem::FrameWorldTransforms::Get(entt::entity entity) const
{
	if(const auto found = worldTransforms->find(entity); found != worldTransforms->end()) return found->second;
	return fallbackWorldTransform;
}

namespace {
	TerraEngine::Scheduling::EcsComponentQueryDeclaration MakeProtectedRenderQuery(
		std::string queryName,
		bool includeDiagnosticsMetadata,
		std::initializer_list<const char*> reads,
		std::initializer_list<const char*> readResources,
		std::initializer_list<const char*> writeResources,
		TerraEngine::Scheduling::EcsQueryChunkWork work)
	{
		TerraEngine::Scheduling::EcsComponentQueryDeclaration query;
		query.queryName = std::move(queryName);
		if (includeDiagnosticsMetadata)
		{
			query.access.ownerSystem = "RenderSystem";
			query.access.queryName = query.queryName;
			query.access.readComponents.assign(reads.begin(), reads.end());
			query.access.readResources.assign(readResources.begin(), readResources.end());
			query.access.writeResources.assign(writeResources.begin(), writeResources.end());
			query.access.workerEligible = false;
			query.access.mainThreadReason = "Renderer collection reads live EnTT hierarchy/component storage, protected resources, and frame allocator containers before publishing immutable worker inputs.";
		}
		query.preserveMatchingObjectOrder = true;
		query.copyObjectIdsIntoChunks = false;
		query.runOnceWithoutObjectSnapshot = true;
		query.work = std::move(work);
		return query;
	}
}

// Collect protected ECS rows and loader observations; returned metadata owns every surviving local value.
RenderSystem::FrameRenderableCaptureState RenderSystem::CaptureFrameRenderableInputs(
	const FrameCaptureRoots& roots, std::pmr::vector<CameraEntry>& cameras, std::pmr::vector<LightEntry>& lights,
	const std::shared_ptr<const TerraEngine::Renderer::RenderFrameScratchValidationCallbacks>& scratchValidation)
{
	const auto& protectedRendererGeneration = roots.protectedRendererGeneration;
	const auto& protectedBackendResourceGeneration = roots.protectedBackendResourceGeneration;
	const auto& protectedSceneChanges = roots.protectedSceneChanges;
	const auto& protectedSceneGeneration = roots.protectedSceneGeneration;
	const auto& animationPresentationFrame = roots.animationPresentationFrame;
	const auto& gpuPresentation = roots.gpuPresentation;
	const auto& immutableGpuAnimationPlan = roots.immutableGpuAnimationPlan;
	const auto& hasDenseAnimationTopologyCandidate = roots.hasDenseAnimationTopologyCandidate;
	auto& resourceManager = ResourceManager::Instance();
	const auto protectedRendererResourceOwnerRevision = resourceManager.GetRendererResourcePublicationGeneration();
	const std::uint64_t protectedTerrainProxyGeneration =
		TerraEngine::Renderer::Terrain::TerrainRenderProxyRegistry::Instance()
			.PublicationGeneration();
	// Capture reuse is independent of cameras and transforms. It covers only the
	// live ECS/resource work needed to rediscover render membership and animation
	// binding topology. Fully GPU-visual pose generations are intentionally absent:
	// the backend resolves them from the current immutable presentation root.
	std::uint64_t renderableCaptureSourceSignature = kRenderHashOffset;
	HashCombine(renderableCaptureSourceSignature, protectedSceneGeneration);
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentMutationRevision());
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentValueMutationRevision(
			MeshRenderComponent::kComponentName));
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetComponentValueMutationRevision(
			TerrainComponent::kComponentName));
	HashCombine(renderableCaptureSourceSignature, protectedTerrainProxyGeneration);
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetHierarchyRevision());
	HashCombine(renderableCaptureSourceSignature,
		objManager.GetRuntimeActiveMembershipRevision());
	HashCombine(renderableCaptureSourceSignature,
		protectedRendererResourceOwnerRevision);
	HashCombine(renderableCaptureSourceSignature,
		resourceManager.GetAnimationBindingResourceRevision());
	HashCombine(renderableCaptureSourceSignature, protectedRendererGeneration);
	HashCombine(renderableCaptureSourceSignature,
		protectedBackendResourceGeneration);
	HashCombine(renderableCaptureSourceSignature,
		hasDenseAnimationTopologyCandidate && immutableGpuAnimationPlan != nullptr
			? immutableGpuAnimationPlan->topologyInputSignature
			: animationPresentationFrame != nullptr
				? animationPresentationFrame->presentationGeneration : 0U);
	if(renderableCaptureSourceSignature == 0U)
		renderableCaptureSourceSignature = 1U;
	// Retained authoring flags must observe the scene journal too. SetStatic
	// changes metadata/bounds without changing component or active revisions.
	// A newer metadata row (or a journal gap) requires the existing delta/full
	// capture; warm transform-only packets can retain binding topology.
	const bool renderableMetadataCaptureCurrent = protectedSceneChanges != nullptr
		&& protectedSceneChanges->Validate(protectedSceneGeneration).valid
		&& protectedSceneChanges->PublicationRevision() != 0U
		&& objManager.CanApplySceneChangePacketToLiveState(*protectedSceneChanges,
			protectedSceneChanges->PublicationRevision() - 1U)
		&& (protectedSceneChanges->PublicationRevision() == renderableMembershipPacketRevision
			|| (objManager.CanApplySceneChangePacketToLiveState(*protectedSceneChanges, renderableMembershipPacketRevision)
				&& protectedSceneChanges->MetadataChanged().empty()));
	const bool reuseProtectedRenderableCapture = renderableMetadataCaptureCurrent
		&& retainedRenderableEntriesReusable
		&& retainedRenderableCaptureSourceSignature
			== renderableCaptureSourceSignature
		&& renderableMembershipSceneGeneration == objManager.GetSceneGeneration()
		&& renderableMembershipComponentRevision
			== objManager.GetComponentMutationRevision()
		&& renderableMembershipHierarchyRevision
			== objManager.GetHierarchyRevision()
		&& renderableMembershipActiveRevision
			== objManager.GetRuntimeActiveMembershipRevision()
		&& renderableMembershipTerrainProxyGeneration
			== protectedTerrainProxyGeneration
		&& retainedRenderableEntries.size() == renderableMembershipCache.size();

	cameras.reserve(10);
	// This renderer-owned array crosses the coroutine's worker phases without
	// aliasing live ECS storage. A compatible frame retains its values; a changed
	// producer clears and reconstructs the complete array transactionally below.
	std::vector<RenderableEntry>& renderables = retainedRenderableEntries;
	if(!reuseProtectedRenderableCapture)
		renderables.clear();
	// Collection already classifies the final animation binding for every owner.
	// Preserve whether any CPU pose can need a host deformation bridge so the
	// later builder does not rescan a completely GPU-driven crowd to rediscover
	// that its output would be empty.
	bool hasCpuDeformationCandidates = false;
	// Successful CPU deformation records are normally a tiny exception inside a
	// GPU-resident crowd. Preserve their renderable indices so dynamic publication
	// can copy only the affected 256-lane pages instead of reopening every page.

	// Reuse the previous protected snapshot's cardinality as a conservative capacity
	// hint. Large animated crowds otherwise grow this frame-arena vector several
	// times before reaching the same stable entity count on every frame.
	const std::size_t expectedRenderableCount = std::max<std::size_t>(
		1000U, latestRenderFrameSnapshot.renderables.size());
	if(!reuseProtectedRenderableCapture)
		renderables.reserve(expectedRenderableCount);
	// Snapshot scene light components so packing can run in worker tasks without
	// touching the entity registry after this point.

	lights.reserve(256);
	// These protected-boundary containers are populated only for validated
	// canonical deformable meshes. Static TEMF records never touch the AnimationSystem
	// singleton; morph-only payloads participate only when their exact identity
	// palette and a compatible one-joint animation publication are both present.
	std::unordered_set<TerraEngine::UUID> aliveCompatibleAnimationObjects;
	// GPU-visual crowds do not retain CPU poses. Avoid allocating a large
	// retirement set on their common frame; an existing or newly created CPU-pose
	// cache grows this set on demand without affecting GPU-only collection.
	if(!compatibleAnimationPoseCache.empty()) {
		aliveCompatibleAnimationObjects.reserve(compatibleAnimationPoseCache.size());
	}
	std::optional<std::uint64_t> animationAuthorityGeneration;
	// Missing immutable publications keep capture open for next-frame async retries.
	bool renderableResourcePublicationsComplete = true;
	std::size_t runtimeLayoutRejectedRecordCount = 0u;
	std::vector<std::string> runtimeLayoutRejectionReasons;
	// CaptureImage enqueues a custom/camera-object request before the next render
	// boundary. Snapshot whether that request needs an independent offscreen view
	// now, because renderable collection happens before the request is drained
	// into its dedicated submission below.
	bool hasQueuedDedicatedCapture = false;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for(const TerraEngine::Renderer::RenderCaptureRequest& request
			: pendingCaptureRequests) {
			if((request.sourceKind
					!= TerraEngine::Renderer::RenderCaptureSourceKind::GameView
					|| request.dedicatedRenderTarget)
				&& request.format == "image/png") {
				hasQueuedDedicatedCapture = true;
				break;
			}
		}
	}

	// GPU-visual renderables used to linearly rescan every owner mapping and work
	// packet for every mesh. That is quadratic for crowds. These frame-local
	// immutable indexes retain all duplicate records so the fail-open ambiguity
	// checks below remain identical while ordinary lookups become constant-time.
	const TerraEngine::Renderer::Frame::RenderAnimationCaptureView animationCapture{
		immutableGpuAnimationPlan, gpuPresentation, protectedRendererGeneration, protectedSceneGeneration};
	// Resource availability is stable throughout this protected render boundary.
	// Cache repeated mesh/material queries so thousands of instances sharing one
	// prefab do not repeatedly enter the resource manager for identical UUIDs.
	std::unordered_map<TerraEngine::UUID, bool> frameResourceAvailability;
	std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources =
		retainedRuntimeMeshResources;
	std::unordered_map<TerraEngine::UUID, const Material*>& frameMaterialResources =
		retainedRuntimeMaterialResources;
	// Terrain material publications are copied while registry mutation is
	// protected. Later visibility work reads only this frame-owned table.
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>&
		frameMeshLocalBounds = retainedMeshLocalBounds;

	if(!reuseProtectedRenderableCapture)
	{
		frameMeshResources.clear();
		frameMaterialResources.clear();
		retainedRuntimeMeshOwners.clear();
		retainedRuntimeMaterialOwners.clear();
		frameMeshLocalBounds.clear();
	}
	frameResourceAvailability.reserve(32U);
	frameMeshResources.reserve(16U);
	frameMaterialResources.reserve(16U);
	frameMeshLocalBounds.reserve(16U);
	const TerraEngine::Renderer::Frame::RenderResourceCaptureView resourceCapture{
		frameResourceAvailability, frameMeshResources, frameMaterialResources, frameMeshLocalBounds,
		resourceManager, renderableResourcePublicationsComplete, runtimeLayoutRejectedRecordCount, runtimeLayoutRejectionReasons};
	const TerraEngine::Renderer::Frame::RenderAnimationCollectionView animationCollection{
		animationCapture, animationPresentationFrame, animationAuthorityGeneration,
		aliveCompatibleAnimationObjects, hasCpuDeformationCandidates};
	const TerraEngine::Renderer::Frame::RenderMembershipCaptureView membershipCapture{
		protectedSceneChanges, protectedSceneGeneration, protectedTerrainProxyGeneration,
		reuseProtectedRenderableCapture, renderableMetadataCaptureCurrent, hasQueuedDedicatedCapture};
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: ECS Collection Graph", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool diagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		TerraEngine::Scheduling::EcsQueryTaskGraph renderCollectionGraph({ "render-system-collection", "Render System Collection", false, diagnosticsEnabled });

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectCameras",
			diagnosticsEnabled,
			{ "Transform", "CameraComponent" },
			{},
			{ "Render.CameraCollection" },
			[this, &cameras](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				CollectFrameCameraInputs(cameras);
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectRenderables",
			diagnosticsEnabled,
			{ "Transform", "MeshRenderComponent", "TerrainComponent", "AnimatorComponent" },
			{ "Render.CameraCollection", TerraEngine::Scheduling::kResourceAnimatorComponents,
				TerraEngine::Scheduling::kResourceAnimationRuntimeState },
			// Protected admission records streaming needs and may accept ready resource publications.
			{ "Render.RenderableCollection", "Resources", "StreamingNeeds" },
			[this, &cameras, &renderables, &resourceCapture, &animationCollection, &membershipCapture](
				const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				CollectFrameRenderableInputs(cameras, renderables, resourceCapture, animationCollection, membershipCapture);
			}));

		renderCollectionGraph.AddQuery(MakeProtectedRenderQuery(
			"Render.CollectLights",
			diagnosticsEnabled,
			{ "Transform", "LightComponent" },
			{},
			{ "Render.LightCollection" },
			[this, &lights](const TerraEngine::Scheduling::EcsQueryChunk&, TerraEngine::Scheduling::JobGraphContext&)
			{
				CollectFrameLightInputs(lights);
			}));

		const TerraEngine::Scheduling::EcsSystemGraphFrameSummary& graphSummary = renderCollectionGraph.Execute();
		if (diagnosticsEnabled)
		{
			TerraEngine::Scheduling::PublishEcsGraphFrameSummary(graphSummary);
		}
		if (!graphSummary.valid)
		{
			TERRAENGINE_LOG(LogLevel::Error, TerraEngine::Scheduling::FormatEcsSystemGraphFrameSummary(graphSummary));
		}
	}
	// Validation copies actual captured values before any worker can borrow the
	// PMR arrays. Disabled native hooks add no copy or digest to ordinary frames.
	const auto capturedScratchDiagnostics = ObserveFrameCapturedInputs(cameras, lights, scratchValidation);
	return {protectedRendererResourceOwnerRevision, protectedTerrainProxyGeneration, renderableCaptureSourceSignature, hasQueuedDedicatedCapture, hasCpuDeformationCandidates, renderableResourcePublicationsComplete, runtimeLayoutRejectedRecordCount, std::move(runtimeLayoutRejectionReasons), capturedScratchDiagnostics, std::move(aliveCompatibleAnimationObjects)};
}

// Initialize one recyclable snapshot owner against exact captured revisions before worker suspension.
RenderSystem::FrameSnapshotCapture RenderSystem::CaptureFrameSnapshotHeader(
	std::span<RenderableEntry> renderables, bool hasCameraInputs, const FrameRenderableCaptureState& capture,
	const FrameCaptureRoots& roots, const FrameSceneRevisions& revisions, const FrameResourceCohort& resourceCohort,
	const FrameBackendCapture& backendCapture, const FrameWorldTransforms& worldCapture)
{
	const auto& capturedSceneGeneration = revisions.capturedSceneGeneration;
	const auto& capturedSpatialRevision = revisions.capturedSpatialRevision;
	const auto& capturedComponentMutationRevision = revisions.capturedComponentMutationRevision;
	const auto& capturedMeshComponentRevision = revisions.capturedMeshComponentRevision;
	const auto& capturedTerrainComponentRevision = revisions.capturedTerrainComponentRevision;
	const auto& capturedTerrainProxyGeneration = revisions.capturedTerrainProxyGeneration;
	const auto& capturedHierarchyRevision = revisions.capturedHierarchyRevision;
	const auto& capturedRendererResourceRevision = resourceCohort.capturedRendererResourceRevision;
	const auto& capturedVirtualGeometryResidencyGeneration = resourceCohort.capturedVirtualGeometryResidencyGeneration;
	const auto& capturedRenderSettings = resourceCohort.capturedRenderSettings;
	const auto& capturedEfficiencyOptions = resourceCohort.capturedEfficiencyOptions;
	const auto& rendererResourceOwnerCohortStable = resourceCohort.rendererResourceOwnerCohortStable;
	const auto& hardwareVirtualGeometryRouteAvailable = backendCapture.hardwareVirtualGeometryRouteAvailable;
	const auto& computeVirtualGeometryRouteAvailable = backendCapture.computeVirtualGeometryRouteAvailable;
	const auto& deviceAuthoredVirtualGeometryAvailable = backendCapture.deviceAuthoredVirtualGeometryAvailable;
	const auto& validationRouteOverride = backendCapture.validationRouteOverride;
	const auto& hybridValidationSceneRequested = backendCapture.hybridValidationSceneRequested;
	const auto& hasDenseAnimationTopologyCandidate = roots.hasDenseAnimationTopologyCandidate;
	const auto& immutableGpuAnimationPlan = roots.immutableGpuAnimationPlan;
	const auto& animationPresentationFrame = roots.animationPresentationFrame;
	const auto& hasQueuedDedicatedCapture = capture.hasQueuedDedicatedCapture;
	const auto& renderableResourcePublicationsComplete = capture.renderableResourcePublicationsComplete;
	const auto& renderableCaptureSourceSignature = capture.renderableCaptureSourceSignature;
	const auto* transformPropagation = worldCapture.transformPropagation;
	std::uint64_t capturedRenderableSourceSignature = kRenderHashOffset;
	HashCombine(capturedRenderableSourceSignature, capturedSceneGeneration);
	HashCombine(capturedRenderableSourceSignature, capturedSpatialRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedComponentMutationRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedMeshComponentRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedTerrainComponentRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedTerrainProxyGeneration);
	HashCombine(capturedRenderableSourceSignature, capturedHierarchyRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedRendererResourceRevision);
	HashCombine(capturedRenderableSourceSignature,
		capturedVirtualGeometryResidencyGeneration);
	HashCombine(capturedRenderableSourceSignature,
		BuildRenderSettingsSignature(capturedRenderSettings, currentPreset,
			taskShaderCullingEnabled));
	HashCombine(capturedRenderableSourceSignature,
		hardwareVirtualGeometryRouteAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		computeVirtualGeometryRouteAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		deviceAuthoredVirtualGeometryAvailable ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		validationRouteOverride.has_value()
			? static_cast<std::uint64_t>(*validationRouteOverride) + 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		hybridValidationSceneRequested ? 1u : 0u);
	HashCombine(capturedRenderableSourceSignature,
		hasDenseAnimationTopologyCandidate && immutableGpuAnimationPlan != nullptr
			? immutableGpuAnimationPlan->topologyInputSignature
			: animationPresentationFrame != nullptr
				? animationPresentationFrame->presentationGeneration : 0u);
	HashCombine(capturedRenderableSourceSignature,
		static_cast<std::uint64_t>(renderables.size()));
	if(capturedRenderableSourceSignature == 0u)
		capturedRenderableSourceSignature = 1u;

	// GPU-visual owners advance through the immutable animation publication, not
	// through geometry records. Static owners have no binding. Any CPU/hybrid or
	// incomplete binding disables reuse so its exact pose/fallback owner is rebuilt.
	bool renderableBindingsReusable = true;
	for(const RenderableEntry& entry : renderables)
	{
		const TerraEngine::Renderer::AnimationRenderBinding& binding =
			entry.animationBinding;
		if(binding.meshId == TerraEngine::EMPTY_UUID)
			continue;
		if(!hasDenseAnimationTopologyCandidate || !binding.gpu.IsComplete()
			|| binding.gpu.hybridCpuAuthority || binding.HasCpuPose())
		{
			renderableBindingsReusable = false;
			break;
		}
	}
	if(transformPropagation != nullptr && transformPropagation->valid)
	{
		for(const TerraEngine::Scene::SceneWorldTransformRecord& changedTransform :
			transformPropagation->records)
		{
			const auto changedRenderable = std::lower_bound(
				renderables.begin(), renderables.end(), changedTransform.objectId,
				[](const RenderableEntry& entry, const TerraEngine::UUID& objectId)
				{
					return entry.objectId < objectId;
				});
			if(changedRenderable != renderables.end()
				&& changedRenderable->objectId == changedTransform.objectId)
			{
				changedRenderable->transformGeneration = std::max<std::uint64_t>(
					1U, changedTransform.sourceRevision);
			}
		}
	}
	const bool capturedRenderableConsumer = hasCameraInputs
		|| hasSceneViewCamera || hasQueuedDedicatedCapture;
	retainedRenderableEntriesReusable = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& capturedRenderableConsumer && rendererResourceOwnerCohortStable;
	retainedRenderableCaptureSourceSignature =
		retainedRenderableEntriesReusable
		? renderableCaptureSourceSignature : 0U;
	const bool detailedRenderableEvidenceRequested =
		TerraEngine::Renderer::RenderDiagnosticsRequested();
	TerraEngine::Renderer::RenderFrameSnapshot renderFrameSnapshot =
		std::move(recycledRenderFrameSnapshot);
	// View requests are tiny compared with renderable records. Retain the prior
	// qualified origins so motion matrices can be rebuilt without treating a
	// canonical pose as a global float.
	std::vector<TerraEngine::Renderer::RenderFrameViewRequest>
		previousViewRequests = renderFrameSnapshot.viewRequests;
	const bool reuseRenderableRecords = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& rendererResourceOwnerCohortStable
		&& !detailedRenderableEvidenceRequested
		&& renderFrameSnapshot.reusableRenderableRecords
		&& renderFrameSnapshot.renderableSourceSignature
			== capturedRenderableSourceSignature
		&& renderFrameSnapshot.renderables.size() == renderables.size();
	// Reuse top-level vector capacity from the prior published frame. Clearing the
	// non-visible owner here avoids allocating and later destroying another large
	// crowd snapshot while the concurrently readable latest snapshot stays intact.
	renderFrameSnapshot.viewRequests.clear();
	if(!reuseRenderableRecords)
	{
		renderFrameSnapshot.renderables.clear();
		renderFrameSnapshot.materials.clear();
		renderFrameSnapshot.meshes.clear();
		// Direct-batch contents are rebuilt only with persistent renderable records.
		// Their instance arrays stay intact on a warm reusable frame.
		for(TerraEngine::Renderer::DirectMeshletBatch& batch :
			renderFrameSnapshot.directMeshletBatches)
		{
			batch.instanceIdentities.clear();
		}
	}
	renderFrameSnapshot.conventionalVisibilityIdentityTable.clear();
	renderFrameSnapshot.lights.clear();
	renderFrameSnapshot.textures.clear();
	renderFrameSnapshot.virtualGeometryViewSelections.clear();
	renderFrameSnapshot.virtualGeometryVisibilitySnapshots.clear();
	renderFrameSnapshot.geometryRouteDecisions.clear();
	renderFrameSnapshot.animatedGeometryPageDemands.clear();
	renderFrameSnapshot.animatedGeometryPageDemandCount = 0u;
	renderFrameSnapshot.resourceRevision = 0u;
	renderFrameSnapshot.sceneRevision = 0u;
	renderFrameSnapshot.valid = true;
	renderFrameSnapshot.containsLiveMutableState = false;
	renderFrameSnapshot.frameId = activeRenderFrameId;
	renderFrameSnapshot.renderableSourceSignature =
		capturedRenderableSourceSignature;
	renderFrameSnapshot.reusableRenderableRecords = renderableBindingsReusable
		&& renderableResourcePublicationsComplete
		&& !detailedRenderableEvidenceRequested && rendererResourceOwnerCohortStable;
	renderFrameSnapshot.renderSettings = capturedRenderSettings;
	renderFrameSnapshot.efficiencyOptions = capturedEfficiencyOptions;
	{
		std::lock_guard<std::mutex> lock(runtimeRenderSettingsMutex);
		renderFrameSnapshot.efficiencyFixtureIntent = capturedRendererEfficiencyFixtureIntent;
	}
	// Renderer asset publication is monotonic and advances only after a complete
	// mesh, material, or texture cache replacement. Backend preparation caches use
	// it to reject plans built from older immutable payloads.
	renderFrameSnapshot.resourceRevision = capturedRendererResourceRevision;
	renderFrameSnapshot.sceneRevision = capturedSpatialRevision;

	return {std::move(renderFrameSnapshot), std::move(previousViewRequests), reuseRenderableRecords};
}

// Validate only copied fixture controls against this exact protected input membership.
bool RenderSystem::ValidateDepthFixtureIntent(const TerraEngine::Renderer::RendererEfficiencyFixtureIntent& intent, std::string& reason)
	{
		if (intent.staticShadowControls && intent.correlation.operationId.empty()) {
			reason = "Static shadow fault controls require an owned validation session."; return false;
		}
		if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureShadowBatchControls(
			intent.shadowBatchControls, !intent.correlation.operationId.empty(), reason)) return false;
		if (!intent.depthControls) { reason.clear(); return true; }
		std::array<TerraEngine::Renderer::LightingDepthReduction::Extent,2> extents{};
		if (intent.cameras.size()>extents.size()) { reason="Depth controls exceed the sealed camera capacity."; return false; }
		for (std::size_t index=0;index<intent.cameras.size();++index)
			extents[index]={intent.cameras[index].nativeExtent.x,intent.cameras[index].nativeExtent.y};
		return TerraEngine::Renderer::ValidateRendererEfficiencyFixtureDepthControls(intent.depthControls,
			!intent.correlation.operationId.empty(),std::span(extents.data(),intent.cameras.size()),reason);
	}

RenderSystem::FrameFixtureCapture RenderSystem::CaptureFrameFixtureInputs(
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot, std::span<const RenderableEntry> renderables,
	std::span<const CameraEntry> cameras, std::span<const LightEntry> lights,
	const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources,
	TerraEngine::Renderer::Frame::FrameGeometryInputs& geometryInputs, bool rendererResourceOwnerCohortStable,
	const FrameWorldTransforms& worldCapture, bool hasPendingDedicatedCaptureRender)
{
	const auto* worldTransforms = worldCapture.worldTransforms;
	std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof> fixtureTargets;
	std::vector<TerraEngine::Renderer::RendererEfficiencyResolvedFixtureCamera> sealedFixtureCameras;
	if (!renderFrameSnapshot.efficiencyFixtureIntent.correlation.operationId.empty())
	{
		// Validation alone requests owned cooked range copies. Ordinary frames
		// avoid this allocation; no host can retain live loader pointers.
		fixtureTargets.reserve(renderables.size());
		for (const auto& entry : renderables)
		{
			const auto found = frameMeshResources.find(entry.mesh->meshId);
			const Mesh* mesh = found != frameMeshResources.end() ? found->second : nullptr;
			if (mesh == nullptr) continue;
			TerraEngine::Renderer::RendererEfficiencyFixtureTargetProof proof;
			proof.objectId = entry.objectId; proof.meshId = entry.mesh->meshId;
			if (const auto object = objManager.getObject(entry.entity); object.has_value()) {
				proof.lifetimeGeneration = objManager.getObjectLifetimeGeneration(*object);
				proof.isStatic = objManager.isObjectStatic(*object);
				proof.metadataGeneration = objManager.getObjectMetadataGeneration(*object);
				proof.spatialRevision = objManager.GetObjectSpatialRevision(*object);
			}
			proof.resourceGeneration = rendererResourceOwnerCohortStable ? renderFrameSnapshot.resourceRevision : 0;
			proof.meshletCapacity = mesh->runtimeLayout.meshletCount;
			proof.ownerGeometrySupported = !mesh->hasVirtualGeometry &&
				geometryInputs.Facts(entry.mesh->meshId, *mesh).directUsable;
			for (const auto& detail : mesh->virtualGeometryDetailLevels)
				proof.detailLevels.push_back({detail.lodIndex, detail.meshletOffset, detail.meshletCount});
			fixtureTargets.push_back(std::move(proof));
		}
	}
	{
		auto& intent = renderFrameSnapshot.efficiencyFixtureIntent;
		intent.controlsApplied = !intent.correlation.operationId.empty(); intent.controlReason.clear();
		if(!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureCameras(intent, sealedFixtureCameras, intent.controlReason)) intent.controlsApplied = false;
		if(intent.controlsApplied && !ValidateDepthFixtureIntent(intent,intent.controlReason)) intent.controlsApplied = false;
		if(intent.controlsApplied && !TerraEngine::Renderer::ValidateRendererEfficiencyFixtureCasterSelections(intent,fixtureTargets,intent.controlReason)) intent.controlsApplied = false;
		if (intent.controlsApplied && intent.lightingControls)
		{
			// Capture the complete active input, before camera queries or budgets.
			// Inactive scene lights cannot be introduced by a validation ordering.
			std::vector<TerraEngine::UUID> activeLightIds; activeLightIds.reserve(lights.size());
			for (const auto& light : lights) activeLightIds.push_back(light.lightId);
			if (!TerraEngine::Renderer::ValidateRendererEfficiencyFixtureLightingControls(intent, activeLightIds, intent.controlReason)) intent.controlsApplied = false;
		}
		if (intent.shadowSelection)
		{
			const auto target = std::find_if(fixtureTargets.begin(), fixtureTargets.end(), [&](const auto& row) { return row.objectId == intent.shadowSelection->objectId; });
			TerraEngine::Renderer::RendererEfficiencyFixtureDetailRange range;
			if (target == fixtureTargets.end()) { intent.controlsApplied = false; intent.controlReason = "Requested fixture object is absent from current protected capture."; }
			else if (!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureSelection(*intent.shadowSelection, *target, range, intent.controlReason)) intent.controlsApplied = false;
		}
		std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureViewOrderEntry> actualViews;
		if(!sealedFixtureCameras.empty() && intent.controlsApplied) {for(const auto& camera:sealedFixtureCameras) actualViews.push_back({camera.viewType,camera.outputId});}
		else {
			if (hasSceneViewCamera && (!editorModeEnabled || (sceneViewTargetSize.x && sceneViewTargetSize.y))) actualViews.push_back({RenderViewType::Scene, 1});
			if (!gameViewSuppressed && (!editorModeEnabled || (gameViewTargetSize.x && gameViewTargetSize.y)) && (!gameViewCameras.empty() || !cameras.empty())) actualViews.push_back({RenderViewType::Game, 2});
		}
		std::vector<std::size_t> viewIndices; std::string viewReason;
		if (intent.controlsApplied && intent.lightingControls && intent.lightingControls->snapshotSlot
			&& std::none_of(actualViews.begin(), actualViews.end(), [&](const auto& view) {
				return view.viewType == intent.lightingControls->snapshotViewType && view.outputId == intent.lightingControls->snapshotOutputId;
			}))
		{ intent.controlsApplied = false; intent.controlReason = "Lighting snapshot target is absent from the captured output membership."; }
		if (!TerraEngine::Renderer::ResolveRendererEfficiencyFixtureViewOrder(intent, actualViews, viewIndices, viewReason))
		{ intent.controlsApplied = false; intent.controlReason = viewReason; }
		if (sealedFixtureCameras.empty() && (!intent.viewOrder.empty() || !intent.viewOrigins.empty())
			&& (gameViewCameras.size() > 1 || (gameViewCameras.empty() && cameras.size() > 1)))
		{ intent.controlsApplied = false; intent.controlReason = "Controlled view identity is ambiguous across multiple Game cameras."; }
		if (sealedFixtureCameras.empty() && (intent.referenceOriginDelta || !intent.viewOrigins.empty()))
		{
			// Validate every affected view before changing any reference frame.
			const auto validCamera = [&](const TerraEngine::Scene::WorldPosition& camera, RenderViewType type, std::uint32_t output) {
				const auto delta = TerraEngine::Renderer::RendererEfficiencyFixtureOriginDelta(intent, type, output);
				if (!delta) return true;
				TerraEngine::Renderer::RenderReferenceFrame frame; frame.canonicalOrigin = camera;
				glm::mat4 view(1); glm::vec3 local(0);
				return TerraEngine::Renderer::RebaseRendererEfficiencyFixtureView(camera, *delta, frame, view, local);
			};
			bool valid = !hasSceneViewCamera || validCamera(sceneViewCamera.canonicalPosition, RenderViewType::Scene, 1);
			for (const auto& camera : gameViewCameras) valid = valid && validCamera(camera.canonicalPosition, RenderViewType::Game, 2);
			for (const auto& camera : cameras)
			{
				const auto pose = worldTransforms->find(camera.entity);
				valid = valid && pose != worldTransforms->end() && validCamera(pose->second.position, RenderViewType::Game, 2);
			}
			if (!valid) { intent.controlsApplied = false; intent.controlReason = "Reference-origin delta cannot be safely localized for both views."; }
		}
		// A user's pending dedicated capture retains its ordinary completion path.
		// Reject the fixture controls instead of acknowledging an unexpected view.
		if(!intent.correlation.operationId.empty() && hasPendingDedicatedCaptureRender)
		{ intent.controlsApplied = false; intent.controlReason = "Owned fixture controls rejected because a dedicated capture is pending."; }
	}

	return {std::move(fixtureTargets), std::move(sealedFixtureCameras)};
}
