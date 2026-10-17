#include "RenderPreparationTasks.h"
#include "RenderVirtualGeometrySelection.h"
// Canonical world-space inputs and base-command spatial preparation.
#include "RenderFrameState.h"
#include "RenderFrameCapture.h"
#include "Profiling/ProfilerScope.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace TerraEngine::Renderer::Frame {
	std::uint64_t BuildDrawCommandSignature(
		const TerraEngine::UUID& meshId,
		const TerraEngine::UUID& materialId,
		const TerraEngine::Scene::WorldPose& transform,
		bool taskShaderCullingEnabled)
	{
		// The cache key covers every RenderItemRecord input that affects model-space
		// or resource binding. Scene View tint is applied later to the visible
		// command list, so it intentionally does not invalidate the base cache.
		std::uint64_t seed = kRenderHashOffset;
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(meshId)));
		HashCombine(seed, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(materialId)));
		HashCombine(seed, static_cast<std::uint64_t>(
			TerraEngine::Scene::WorldPositionHash{}(transform.position)));
		HashQuat(seed, transform.rotation);
		HashVec3(seed, transform.scale);
		HashCombine(seed, taskShaderCullingEnabled ? 1u : 0u);
		return seed;
	}
	void PruneStaleDrawCommandCache(DrawCommandCacheState& cache, const std::unordered_set<TerraEngine::UUID>& aliveObjectIds)
	{
		for (auto iterator = cache.recordsByObjectId.begin(); iterator != cache.recordsByObjectId.end();)
		{
			iterator = aliveObjectIds.contains(iterator->first)
				? std::next(iterator)
				: cache.recordsByObjectId.erase(iterator);
		}
	}
	// Localize exact canonical input before constructing one model matrix.
	glm::mat4 MakeFrameModelMatrix(const TerraEngine::Scene::WorldPose& pose,
		const TerraEngine::Scene::WorldPosition& referenceOrigin)
	{
		glm::mat4 model = glm::mat4(1.0f);
		const TerraEngine::Scene::WorldLocalizationResult localized =
			TerraEngine::Scene::LocalizeWorldPosition(
				pose.position,
				referenceOrigin,
				TerraEngine::Scene::kTransformSpaceMaximumRelativeRadiusMetres);
		if (!localized.Succeeded())
		{
			return model;
		}
		model = glm::translate(model, glm::vec3(localized.offsetMetres));
		model = model * glm::mat4_cast(pose.rotation);
		model = glm::scale(model, pose.scale);
		return model;

}
}

// Refresh copied canonical poses against exact protected revisions; workers do not open ECS.
void RenderSystem::RefreshFrameRenderablePoses(std::span<const RenderableEntry> renderables,
	const FrameSceneRevisions& sceneRevisions, const FrameWorldTransforms& worldCapture)
{
	const auto capturedSceneGeneration = sceneRevisions.capturedSceneGeneration;
	const auto capturedSpatialRevision = sceneRevisions.capturedSpatialRevision;
	const auto capturedComponentMutationRevision = sceneRevisions.capturedComponentMutationRevision;
	const auto capturedHierarchyRevision = sceneRevisions.capturedHierarchyRevision;
	const auto* worldTransforms = worldCapture.worldTransforms;
	const auto* transformPropagation = worldCapture.transformPropagation;
	const std::uint64_t modelMatrixSceneGeneration = capturedSceneGeneration;
	const std::uint64_t modelMatrixSpatialRevision = capturedSpatialRevision;
	const std::uint64_t modelMatrixComponentRevision =
		capturedComponentMutationRevision;
	const std::uint64_t modelMatrixHierarchyRevision = capturedHierarchyRevision;
	const bool reuseModelMatrixShape =
		renderableWorldPoseCache.size() == renderables.size()
		&& renderableModelMatrixSceneGeneration == modelMatrixSceneGeneration
		&& renderableModelMatrixComponentRevision == modelMatrixComponentRevision
		&& renderableModelMatrixHierarchyRevision == modelMatrixHierarchyRevision;
	const bool reuseModelMatrices = reuseModelMatrixShape
		&& renderableModelMatrixSpatialRevision == modelMatrixSpatialRevision;
	if(!reuseModelMatrices)
	{
		TERRAENGINE_PROFILE_SCOPE(
			"RenderSystem: Refresh Renderable Model Matrices",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const bool sparseTransformRefresh = reuseModelMatrixShape
			&& transformPropagation != nullptr && transformPropagation->valid
			&& !transformPropagation->records.empty()
			&& transformPropagation->records.size() < renderables.size();
		if(sparseTransformRefresh)
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
					const std::size_t index = static_cast<std::size_t>(
						changedRenderable - renderables.begin());
					renderableWorldPoseCache[index] = changedTransform.worldPose;
				}
			}
		}
		else
		{
			renderableWorldPoseCache.resize(renderables.size());
			for(std::size_t index = 0U; index < renderables.size(); ++index)
			{
				const auto transformIt = worldTransforms->find(renderables[index].entity);
				renderableWorldPoseCache[index] = transformIt != worldTransforms->end()
					? transformIt->second : TerraEngine::Scene::WorldPose{};
			}
		}
		renderableModelMatrixSceneGeneration = modelMatrixSceneGeneration;
		renderableModelMatrixSpatialRevision = modelMatrixSpatialRevision;
		renderableModelMatrixComponentRevision = modelMatrixComponentRevision;
		renderableModelMatrixHierarchyRevision = modelMatrixHierarchyRevision;
	}
}

// Refresh retained spatial rows from copied frame inputs; no borrow escapes the protected cache owner.
TerraEngine::Scene::SpatialDiagnosticsSnapshot RenderSystem::PrepareFrameSpatialInputs(
	std::span<const RenderableEntry> renderables, const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	TerraEngine::Renderer::Frame::SpatialPreparationCacheState& cache,
	const FrameSceneRevisions& revisions, const FrameWorldTransforms& worldCapture,
	const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources,
	const std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>& frameMeshLocalBounds,
	std::size_t expectedUniqueMeshCount, bool& spatialIndexRebuilt)
{
	const auto capturedSceneGeneration = revisions.capturedSceneGeneration;
	const auto capturedSpatialRevision = revisions.capturedSpatialRevision;
	const auto capturedRenderBindingRevision = revisions.capturedRenderBindingRevision;
	const auto* transformPropagation = worldCapture.transformPropagation;
	// Conservative deformable bounds cover the full animation envelope, so pose
	// publications do not invalidate spatial records. Rebuild only when object
	// spatial state, renderable identity/order, or a unique mesh bound changes.
	SpatialPreparationCacheState& spatialPreparationCache = cache;
	std::unordered_map<TerraEngine::UUID, TerraEngine::Scene::SpatialAABB>
		localBoundsByMesh;
	const std::uint64_t spatialSceneGeneration = capturedSceneGeneration;
	const std::uint64_t spatialRevision = capturedSpatialRevision;
	const std::uint64_t meshComponentRevision = capturedRenderBindingRevision;
	// This token changes after every successful renderer-resource reload. A mesh
	// replacement can alter bounds without mutating the MeshRenderComponent, so it
	// must invalidate the spatial observation before cached bounds are reused.
	const std::uint64_t meshResourceRevision =
		renderFrameSnapshot.resourceRevision;
	const bool spatialObservationUnchanged = spatialPreparationCache.valid
		&& spatialPreparationCache.observedSceneGeneration == spatialSceneGeneration
		&& spatialPreparationCache.observedSpatialRevision == spatialRevision
		&& spatialPreparationCache.observedMeshComponentRevision
			== meshComponentRevision
		&& spatialPreparationCache.observedMeshResourceRevision
			== meshResourceRevision
		&& spatialPreparationCache.observedRenderableCount == renderables.size();
	std::uint64_t spatialPreparationSignature = spatialPreparationCache.signature;
	if(!spatialObservationUnchanged) {
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Spatial Cache Signature",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		localBoundsByMesh.reserve(expectedUniqueMeshCount);
		spatialPreparationSignature = kRenderHashOffset;
		HashCombine(spatialPreparationSignature, spatialSceneGeneration);
		HashCombine(spatialPreparationSignature, spatialRevision);
		HashCombine(spatialPreparationSignature,
			static_cast<std::uint64_t>(renderables.size()));
		for(const RenderableEntry& entry : renderables)
		{
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(entry.objectId)));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(entry.mesh->meshId)));
			HashCombine(spatialPreparationSignature, entry.isStatic ? 1U : 0U);
			if(localBoundsByMesh.contains(entry.mesh->meshId)) continue;

			const auto capturedBounds = frameMeshLocalBounds.find(entry.mesh->meshId);
			const TerraEngine::Scene::SpatialAABB localBounds =
				capturedBounds != frameMeshLocalBounds.end()
					? capturedBounds->second : TerraEngine::Scene::SpatialAABB{};
			localBoundsByMesh.emplace(entry.mesh->meshId, localBounds);
			const auto hashFloat = [](float value) {
				return static_cast<std::uint64_t>(std::hash<float>{}(value));
			};
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.x));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.y));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.min.z));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.x));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.y));
			HashCombine(spatialPreparationSignature, hashFloat(localBounds.max.z));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(localBounds.state));
			HashCombine(spatialPreparationSignature,
				static_cast<std::uint64_t>(localBounds.source));
			if(const auto mesh = frameMeshResources.find(entry.mesh->meshId);
				mesh != frameMeshResources.end() && mesh->second != nullptr) {
				HashCombine(spatialPreparationSignature,
					mesh->second->deformablePayloadGeneration);
				HashCombine(spatialPreparationSignature,
					mesh->second->runtimeLayout.version);
			}
		}
	}
	spatialIndexRebuilt = false;
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Refresh Spatial Index", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		TerraEngine::Scene::SpatialReferenceFrame spatialFrame{};
		spatialFrame.generation = std::max<std::uint64_t>(1U, capturedSpatialRevision);
		if (!renderables.empty())
		{
			spatialFrame.canonicalOrigin = worldCapture.Get(renderables.front().entity).position;
		}
		const TerraEngine::Scene::SpatialReferenceFrame previousSpatialFrame =
			spatialIndex.GetReferenceFrame();
		const bool spatialFrameChanged =
			previousSpatialFrame.generation != spatialFrame.generation
			|| previousSpatialFrame.canonicalOrigin != spatialFrame.canonicalOrigin;
		const bool spatialFrameAccepted = spatialIndex.SetReferenceFrame(spatialFrame);
		if(!spatialPreparationCache.valid
			|| spatialPreparationCache.signature != spatialPreparationSignature
			|| spatialFrameChanged || !spatialFrameAccepted)
		{
			spatialPreparationCache.records.clear();
			spatialPreparationCache.renderableLookup.clear();
			spatialPreparationCache.conservativeAnimationBounds.assign(
				renderables.size(), false);
			spatialPreparationCache.records.reserve(renderables.size());
			spatialPreparationCache.renderableLookup.reserve(renderables.size());

			for (std::size_t index = 0; index < renderables.size(); ++index)
			{
				const RenderableEntry& entry = renderables[index];
				spatialPreparationCache.renderableLookup.emplace(entry.entity, index);
				const TerraEngine::Scene::WorldPose& transform = worldCapture.Get(entry.entity);
				const glm::mat4 model = TerraEngine::Renderer::Frame::MakeFrameModelMatrix(transform, spatialFrame.canonicalOrigin);
				const auto cachedBounds = localBoundsByMesh.find(entry.mesh->meshId);
				const TerraEngine::Scene::SpatialAABB localBounds =
					cachedBounds != localBoundsByMesh.end()
						? cachedBounds->second : TerraEngine::Scene::SpatialAABB{};
				const auto cachedSpatialMesh = frameMeshResources.find(entry.mesh->meshId);
				const Mesh* spatialMesh = cachedSpatialMesh != frameMeshResources.end()
					? cachedSpatialMesh->second : nullptr;
				// CacheLoadedMeshBounds unions validated TEMF deformation bounds with
				// bind-pose bounds. That whole-animation envelope makes ordinary spatial
				// rejection conservative even while the exact page bounds are produced
				// on the GPU later in the frame.
				spatialPreparationCache.conservativeAnimationBounds[index] = spatialMesh != nullptr
					&& spatialMesh->hasDeformablePayload && localBounds.IsUsable();
				const TerraEngine::Scene::WorldLocalizationResult localizedOwner =
					TerraEngine::Scene::LocalizeWorldPosition(
						transform.position,
						spatialFrame.canonicalOrigin,
						spatialFrame.maximumLocalDistanceMetres);
				TerraEngine::Scene::SpatialAABB localizedBounds = localBounds.IsUsable()
					? TerraEngine::Scene::TransformAABB(localBounds, model)
					: (localizedOwner.Succeeded()
						? TerraEngine::Scene::MakeFallbackBounds(
							glm::vec3(localizedOwner.offsetMetres), 1.0f)
						: TerraEngine::Scene::MakeInvalidBounds(
							TerraEngine::Scene::SpatialBoundsSource::Fallback));

				if (!localizedBounds.IsUsable() && localizedOwner.Succeeded())
				{
					localizedBounds = TerraEngine::Scene::MakeFallbackBounds(
						glm::vec3(localizedOwner.offsetMetres), 1.0f);
				}

				TerraEngine::Scene::SpatialObjectRecord record{};
				record.objectId = entry.objectId;
				record.entity = entry.entity;
				record.active = true;
				record.isStatic = entry.isStatic;
				record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable | TerraEngine::Scene::SpatialCategoryEditorSelectable;
				record.canonicalOwner = transform.position;
				record.localBounds = localizedBounds;
				record.frameGeneration = spatialFrame.generation;
				record.revision = capturedSpatialRevision;
				spatialPreparationCache.records.push_back(record);
			}
			spatialIndex.Refresh(spatialPreparationCache.records);
			spatialPreparationCache.signature = spatialPreparationSignature;
			spatialPreparationCache.valid = true;
			spatialIndexRebuilt = true;
		}
	}
	spatialPreparationCache.observedSceneGeneration = spatialSceneGeneration;
	spatialPreparationCache.observedSpatialRevision = spatialRevision;
	spatialPreparationCache.observedMeshComponentRevision = meshComponentRevision;
	spatialPreparationCache.observedMeshResourceRevision = meshResourceRevision;
	spatialPreparationCache.observedRenderableCount = renderables.size();
	TerraEngine::Scene::SpatialDiagnosticsSnapshot latestSpatialRefreshDiagnostics = spatialIndex.GetDiagnosticsSnapshot();
	latestSpatialRefreshDiagnostics.transformDirtyRootCount = transformPropagation->dirtyRootCount;
	latestSpatialRefreshDiagnostics.transformExpandedObjectCount = transformPropagation->expandedDirtyObjectCount;
	latestSpatialRefreshDiagnostics.transformProcessedObjectCount = transformPropagation->processedObjectCount;
	latestSpatialRefreshDiagnostics.transformSkippedCleanObjectCount = transformPropagation->skippedCleanObjectCount;
	latestSpatialRefreshDiagnostics.transformHierarchyLevelCount = transformPropagation->hierarchyLevelCount;
	latestSpatialRefreshDiagnostics.boundsRecomputedObjectCount = spatialIndexRebuilt
		? static_cast<std::uint32_t>(spatialPreparationCache.records.size()) : 0U;

	return latestSpatialRefreshDiagnostics;
}

// Populate reusable base commands; child hashing settles before these borrowed views return.
void RenderSystem::PrepareFrameBaseCommands(std::span<const RenderableEntry> renderables,
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	TerraEngine::Renderer::Frame::RenderableDrawWorkingSet& workingSet,
	TerraEngine::Renderer::Frame::DrawCommandCacheState& cache,
	const FrameSceneRevisions& revisions, const FrameWorldTransforms& worldCapture,
	std::size_t renderGraphWorkerCount, TerraEngine::Renderer::RenderDiagnosticsSummary& renderGraphDiagnostics)
{
	const auto capturedSceneGeneration = revisions.capturedSceneGeneration;
	const auto capturedActiveMembershipRevision = revisions.capturedActiveMembershipRevision;
	const auto capturedComponentMutationRevision = revisions.capturedComponentMutationRevision;
	const auto capturedHierarchyRevision = revisions.capturedHierarchyRevision;
	const auto capturedSpatialRevision = revisions.capturedSpatialRevision;
	const auto capturedRenderBindingRevision = revisions.capturedRenderBindingRevision;
	RenderableDrawWorkingSet& renderableDrawWorkingSet =
		workingSet;
	// resize preserves the allocated backing storage. Every active element is
	// overwritten below, and base commands never own a pose publication.
	renderableDrawWorkingSet.commands.resize(renderables.size());
	renderableDrawWorkingSet.signatures.resize(renderables.size());
	std::vector<RenderItemRecord>& renderableDrawCommands =
		renderableDrawWorkingSet.commands;
	std::vector<std::uint64_t>& drawCommandSignatures =
		renderableDrawWorkingSet.signatures;

	if (!renderables.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Draw Commands", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		DrawCommandCacheState& drawCommandCache = cache;
		drawCommandCache.hitCount = 0u;
		drawCommandCache.missCount = 0u;
		const std::uint64_t drawSceneGeneration = capturedSceneGeneration;
		const std::uint64_t drawActiveMembershipRevision =
			capturedActiveMembershipRevision;
		const std::uint64_t drawComponentRevision =
			capturedComponentMutationRevision;
		const std::uint64_t drawHierarchyRevision = capturedHierarchyRevision;
		const std::uint64_t drawSpatialRevision = capturedSpatialRevision;
		const std::uint64_t drawMeshComponentRevision =
			capturedRenderBindingRevision;
		const bool reuseOrderedBaseCommands = drawCommandCache.baseInputsValid
			&& drawCommandCache.observedSceneGeneration == drawSceneGeneration
			&& drawCommandCache.observedActiveMembershipRevision
				== drawActiveMembershipRevision
			&& drawCommandCache.observedSpatialRevision == drawSpatialRevision
			&& drawCommandCache.observedMeshComponentRevision
				== drawMeshComponentRevision
			&& drawCommandCache.recordsByObjectId.size() == renderables.size();

		if(!reuseOrderedBaseCommands) {
			// Workers compute cheap signatures from immutable reflected inputs.
			// The cache map itself is updated later on the coordinator thread so
			// frame-to-frame reuse does not add a lock to every renderable.
			RunRenderChunkedWork(
				renderables.size(),
				renderGraphWorkerCount,
				kRenderParallelThreshold,
				"RenderSystem Task: Hash Draw Command Chunk",
				[&](std::size_t start, std::size_t end)
				{
					for (std::size_t index = start; index < end; ++index)
					{
						const RenderableEntry& entry = renderables[index];
						const MeshRenderComponent& meshComp = *entry.mesh;
						drawCommandSignatures[index] = BuildDrawCommandSignature(
							meshComp.meshId,
							meshComp.materialId,
							worldCapture.Get(entry.entity),
							meshComp.taskShaderCullingEnabled);
					}
				});
			for (std::size_t index = 0u; index < renderables.size(); ++index)
			{
				const RenderableEntry& entry = renderables[index];
				const MeshRenderComponent& meshComp = *entry.mesh;
				const std::uint64_t signature = drawCommandSignatures[index];
				auto cacheEntry = drawCommandCache.recordsByObjectId.find(entry.objectId);
				if (cacheEntry != drawCommandCache.recordsByObjectId.end() && cacheEntry->second.signature == signature)
				{
					cacheEntry->second.lastTouchedFrameId = activeRenderFrameId;
					renderableDrawCommands[index] = cacheEntry->second.command;
					// Geometry routing is recalibrated from the current immutable
					// TEMF/runtime facts each frame, so it must not remain cached.
					renderableDrawCommands[index].geometryRoute =
						renderFrameSnapshot.renderables[index].geometryRouteDecision.selected;
					// The visible-command cache later republishes this base value without
					// another scene lookup. Preserve its canonical one-based ordinal so a
					// static cache hit can still build a sealed selector for this draw.
					renderableDrawCommands[index].conventionalVisibilityIdentityIndex =
						renderFrameSnapshot.renderables[index].conventionalVisibilityIdentityIndex;
					// Pose publication changes are intentionally absent from the base draw
					// signature. The binding is attached only to a visible frame command.
					renderableDrawCommands[index].animationBinding = {};
					++drawCommandCache.hitCount;
					continue;
				}

				RenderItemRecord command{};
				command.meshId = meshComp.meshId;
				command.materialId = meshComp.materialId;
				command.objectId = entry.objectId;
				command.geometryRoute =
					renderFrameSnapshot.renderables[index].geometryRouteDecision.selected;
				// Store the stable identity with the reusable base command. The cached
				// visible path needs this value to address immutable publication pages.
				command.conventionalVisibilityIdentityIndex =
					renderFrameSnapshot.renderables[index].conventionalVisibilityIdentityIndex;
				command.taskShaderCullingEnabled = meshComp.taskShaderCullingEnabled;
				const TerraEngine::Scene::WorldPose& canonicalPose =
					worldCapture.Get(entry.entity);
				command.canonicalWorldPose = canonicalPose;
				command.modelMatrix = TerraEngine::Renderer::Frame::MakeFrameModelMatrix(canonicalPose, canonicalPose.position);
				renderableDrawCommands[index] = command;

				CachedDrawCommandRecord record{};
				record.signature = signature;
				record.lastTouchedFrameId = activeRenderFrameId;
				record.command = command;
				// The per-object compatible-pose cache owns the one retained prior pose.
				// Base draw caching must not add another hidden publication owner.
				drawCommandCache.recordsByObjectId[entry.objectId] = std::move(record);
				++drawCommandCache.missCount;
			}
		} else {
			// Exact protected revisions prove the cached mesh/material/model inputs and
			// deterministic renderable order are unchanged. Only route selection is
			// frame-dependent, so update that compact field without hashing transforms
			// or probing the UUID map again.
			for(std::size_t index = 0U; index < renderables.size(); ++index) {
				renderableDrawCommands[index].geometryRoute =
					renderFrameSnapshot.renderables[index]
						.geometryRouteDecision.selected;
				// Static reuse still republishes each command through compact selectors;
				// restore the snapshot's canonical ordinal before a visible cache hit
				// materializes these retained base commands.
				renderableDrawCommands[index].conventionalVisibilityIdentityIndex =
					renderFrameSnapshot.renderables[index]
						.conventionalVisibilityIdentityIndex;
			}
			drawCommandCache.hitCount = renderables.size();
		}
		const bool drawMembershipChanged =
			drawCommandCache.observedSceneGeneration != drawSceneGeneration
			|| drawCommandCache.observedActiveMembershipRevision
				!= drawActiveMembershipRevision
			|| drawCommandCache.observedComponentRevision != drawComponentRevision
			|| drawCommandCache.observedHierarchyRevision != drawHierarchyRevision
			|| drawCommandCache.recordsByObjectId.size() != renderables.size();
		if(drawMembershipChanged) {
			std::unordered_set<TerraEngine::UUID> aliveDrawCommandObjects;
			aliveDrawCommandObjects.reserve(renderables.size());
			for(const RenderableEntry& entry : renderables) {
				aliveDrawCommandObjects.insert(entry.objectId);
			}
			PruneStaleDrawCommandCache(drawCommandCache, aliveDrawCommandObjects);
			PruneStaleVirtualGeometryTransitionCache(
				virtualGeometryTransitionCache, aliveDrawCommandObjects);
			drawCommandCache.observedSceneGeneration = drawSceneGeneration;
			drawCommandCache.observedActiveMembershipRevision =
				drawActiveMembershipRevision;
			drawCommandCache.observedComponentRevision = drawComponentRevision;
			drawCommandCache.observedHierarchyRevision = drawHierarchyRevision;
		}
		drawCommandCache.observedSceneGeneration = drawSceneGeneration;
		drawCommandCache.observedActiveMembershipRevision =
			drawActiveMembershipRevision;
		drawCommandCache.observedSpatialRevision = drawSpatialRevision;
		drawCommandCache.observedMeshComponentRevision =
			drawMeshComponentRevision;
		drawCommandCache.baseInputsValid = true;
		if (TerraEngine::Renderer::RenderDiagnosticsRequested())
		{
			renderGraphDiagnostics.backendFallbacks.push_back(
				"Draw command cache hits: "
				+ std::to_string(drawCommandCache.hitCount)
				+ "/"
				+ std::to_string(drawCommandCache.hitCount + drawCommandCache.missCount));
		}
	}
	// Base draw caches intentionally ignore animation presentation serials. Attach
	// the current immutable semantic owner after every hit/miss path so later
	// candidate and legacy shadow materialization cannot retain an old CPU pose.
	for (std::size_t index = 0; index < renderableDrawCommands.size()
		&& index < renderFrameSnapshot.renderables.size(); ++index)
	{
		auto& command = renderableDrawCommands[index];
		command.shadowCasterInputs = renderFrameSnapshot.renderables[index].shadowCasterInputs;
		command.objectLifetimeGeneration = command.shadowCasterInputs != nullptr
			? command.shadowCasterInputs->lifetimeGeneration : 0U;
	}
}

// Produce compact revision signatures without retaining additional scene or animation owners.
RenderSystem::FrameCommandIdentity RenderSystem::PrepareFrameCommandIdentity(
	std::span<const RenderableEntry> renderables, const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	TerraEngine::Renderer::Frame::RenderableFrameIdentityCacheState& cache,
	const std::vector<std::uint64_t>& drawCommandSignatures, const std::vector<bool>& conservativeAnimationBounds,
	const FrameSceneRevisions& revisions, const FrameResourceCohort& resourceCohort,
	const std::shared_ptr<const TerraEngine::Animation::AnimationPresentationFrame>& animationPresentationFrame)
{
	const auto capturedSceneGeneration = revisions.capturedSceneGeneration;
	const auto capturedAnimatorComponentRevision = revisions.capturedAnimatorComponentRevision;
	const auto capturedHierarchyRevision = revisions.capturedHierarchyRevision;
	const auto capturedAnimationBindingResourceRevision = resourceCohort.capturedAnimationBindingResourceRevision;
	// Visibility and LOD selection depend on stable geometry inputs, not the
	// animation publication produced for the same object this frame. Keep a
	// second submission signature for caches that still retain complete draw
	// commands; the visible cache refreshes animation bindings explicitly below.
	RenderableFrameIdentityCacheState& renderableFrameIdentityCache =
		cache;
	bool frameIdentityMatches = renderableFrameIdentityCache.valid
		&& renderableFrameIdentityCache.sceneGeneration
			== capturedSceneGeneration
		&& renderableFrameIdentityCache.objectIds.size() == renderables.size();
	for(std::size_t index = 0U; frameIdentityMatches && index < renderables.size(); ++index) {
		frameIdentityMatches = renderableFrameIdentityCache.objectIds[index]
			== renderables[index].objectId;
	}
	if(!frameIdentityMatches) {
		renderableFrameIdentityCache.objectIds.clear();
		renderableFrameIdentityCache.indicesByObjectId.clear();
		renderableFrameIdentityCache.objectIds.reserve(renderables.size());
		renderableFrameIdentityCache.indicesByObjectId.reserve(renderables.size());
		for(std::size_t index = 0U; index < renderables.size(); ++index) {
			renderableFrameIdentityCache.objectIds.push_back(renderables[index].objectId);
			renderableFrameIdentityCache.indicesByObjectId.emplace(
				renderables[index].objectId, index);
		}
		renderableFrameIdentityCache.drawCommandSignatures.clear();
		renderableFrameIdentityCache.sceneGeneration = capturedSceneGeneration;
		renderableFrameIdentityCache.valid = true;
	}
	std::uint64_t renderableWorldSignature =
		renderableFrameIdentityCache.worldSignature;
	if(renderableFrameIdentityCache.drawCommandSignatures != drawCommandSignatures) {
		renderableWorldSignature = kRenderHashOffset;
		HashCombine(renderableWorldSignature,
			static_cast<std::uint64_t>(drawCommandSignatures.size()));
		for(std::size_t index = 0U; index < drawCommandSignatures.size(); ++index) {
			HashCombine(renderableWorldSignature,
				static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(renderables[index].objectId)));
			HashCombine(renderableWorldSignature, drawCommandSignatures[index]);
		}
		renderableFrameIdentityCache.drawCommandSignatures = drawCommandSignatures;
		renderableFrameIdentityCache.worldSignature = renderableWorldSignature;
	}
	std::uint64_t renderableSubmissionSignature = renderableWorldSignature;
	std::uint64_t renderableVisibilitySignature = renderableWorldSignature;
	// A pose publication normally changes only dynamic deformation data. Hash the
	// bounded admission decisions instead of the per-frame authority generation so
	// stable animated crowds reuse their expensive frustum and VG selection work.
	// Readiness, page-fallback, or conservative-bounds transitions still change the
	// key immediately and therefore cannot retain an empty pre-ready selection.
	for(std::size_t index = 0U; index < renderables.size(); ++index) {
		const Mesh* mesh = renderables[index].runtimeMesh;
		const bool usesVirtualGeometry = mesh != nullptr
			&& mesh->hasVirtualGeometry;
		const TerraEngine::Renderer::AnimationRenderBinding& binding =
			renderFrameSnapshot.renderables[index].animationBinding;
		const bool requiresUnboundedCoarseVisibility =
			binding.RequiresUnboundedCoarseVisibility(
				conservativeAnimationBounds[index]);
		std::uint64_t admissionFlags = 0U;
		admissionFlags |= requiresUnboundedCoarseVisibility ? 1U : 0U;
		admissionFlags |= binding.RequiresVirtualGeometryOwnerFallbackVisibility(
			usesVirtualGeometry) ? 2U : 0U;
		admissionFlags |= binding.IsComputeRasterDeformationSafe() ? 4U : 0U;
		HashCombine(renderableVisibilitySignature, admissionFlags);
		// An asset without a validated whole-animation envelope deliberately
		// bypasses CPU broadphase culling. Its exact deformed bounds can change
		// which virtual-geometry clusters are selected, so retain the pose token
		// only for that correctness fallback. Cooked envelopes keep this token out
		// of the view key and therefore reuse their stable per-view evidence.
		if(requiresUnboundedCoarseVisibility) {
			HashCombine(renderableVisibilitySignature,
				binding.posePublicationGeneration);
		}
	}
	// Shadow draw lists retain complete bindings, so any authority publication,
	// authored Animator edit, hierarchy rebind, or animation/deformable resource
	// publication must invalidate them. These monotonic protected tokens cover the
	// same changes without rehashing every binding field for every crowd member.
	// The presentation root is the sole animation publication acquired for this
	// frame. Its authority generation invalidates retained selections without a
	// second load from AnimationSystem that could race presentation publication.
	HashCombine(renderableSubmissionSignature,
		animationPresentationFrame != nullptr && animationPresentationFrame->IsValid()
			? animationPresentationFrame->authority->authorityGeneration : 0U);
	HashCombine(renderableSubmissionSignature, capturedAnimatorComponentRevision);
	HashCombine(renderableSubmissionSignature, capturedHierarchyRevision);
	HashCombine(renderableSubmissionSignature,
		capturedAnimationBindingResourceRevision);
	// Mesh, material, and texture reloads do not alter animation bindings. Include
	// their independent publication generation so retained visible/shadow command
	// evidence cannot outlive the payload that supplied its selectors.
	HashCombine(renderableSubmissionSignature, renderFrameSnapshot.resourceRevision);

	return {renderableWorldSignature, renderableSubmissionSignature, renderableVisibilitySignature};
}
