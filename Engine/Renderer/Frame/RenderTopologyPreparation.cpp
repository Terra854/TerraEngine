// Typed direct grouping and persistent topology preparation.
#include "RenderFrameState.h"
#include "RenderSnapshotPreparation.h"
#include "Resource/ResourceManager.h"
#include "Profiling/ProfilerScope.h"
#include "Renderer/Graph/RenderGraph.h"
namespace TerraEngine::Renderer::Frame {
	// Borrow immutable captured meshes while grouping stable instance identities.
	void PrepareFrameDirectMeshletBatches(RenderFrameSnapshot& renderFrameSnapshot,
		const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources,
		GeometryRouteStatsAggregate& geometryRouteStats, bool reuseRenderableRecords)
	{
	TERRAENGINE_PROFILE_SCOPE("RenderSystem: Build Direct Meshlet Batches",
		TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	if(!reuseRenderableRecords)
	{
	std::size_t activeDirectBatchCount = 0U;
	for(const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable :
		renderFrameSnapshot.renderables)
	{
		if(renderable.geometryRouteDecision.selected
			!= TerraEngine::Renderer::GeometryRoute::DirectMeshlet)
		{
			continue;
		}
		const std::uint64_t geometryIdentity =
			renderable.geometryRouteDecision.meshIdentity;
		const std::uint64_t materialIdentity = static_cast<std::uint64_t>(
			std::hash<TerraEngine::UUID>{}(renderable.materialId));
		auto activeBatchEnd = renderFrameSnapshot.directMeshletBatches.begin()
			+ static_cast<std::ptrdiff_t>(activeDirectBatchCount);
		auto batch = std::find_if(renderFrameSnapshot.directMeshletBatches.begin(),
			activeBatchEnd,
			[geometryIdentity, materialIdentity](
				const TerraEngine::Renderer::DirectMeshletBatch& value)
			{
				return value.geometryIdentity == geometryIdentity
					&& value.materialIdentity == materialIdentity;
			});
		if(batch == activeBatchEnd)
		{
			if(activeDirectBatchCount == renderFrameSnapshot.directMeshletBatches.size())
			{
				renderFrameSnapshot.directMeshletBatches.emplace_back();
			}
			batch = renderFrameSnapshot.directMeshletBatches.begin()
				+ static_cast<std::ptrdiff_t>(activeDirectBatchCount++);
			batch->geometryIdentity = geometryIdentity;
			// Direct batches retain meshlet spans, so hot reload must give their
			// immutable snapshot identity a new resource generation as well.
			batch->geometryGeneration = renderFrameSnapshot.resourceRevision;
			batch->materialIdentity = materialIdentity;
			batch->firstMeshlet = 0U;
			batch->meshletCount = 0U;
			const auto cachedDirectMesh = frameMeshResources.find(renderable.meshId);
			const Mesh* directMesh = cachedDirectMesh != frameMeshResources.end()
				? cachedDirectMesh->second : nullptr;
			if(directMesh != nullptr
				&& !directMesh->virtualGeometryDetailLevels.empty())
			{
				const VirtualGeometryDetailLevelRecord& baseDetail =
					directMesh->virtualGeometryDetailLevels.front();
				batch->firstMeshlet = baseDetail.meshletOffset;
				batch->meshletCount = baseDetail.meshletCount;
			}
			else
			{
				batch->meshletCount =
					renderable.geometryRouteInput.packedMeshletCount;
			}
		}
		// A batch shares one immutable geometry allocation across every matching
		// object, while retaining stable identities for per-instance state.
		batch->instanceIdentities.push_back(
			renderable.geometryRouteDecision.objectIdentity);
		++geometryRouteStats.sharedInstances;
	}
	renderFrameSnapshot.directMeshletBatches.resize(activeDirectBatchCount);
	}

	}
}

// Transactionally replace compatible topology/slot roots, preserving old readers on rejection.
bool RenderSystem::PrepareFramePersistentTopology(
	TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	TerraEngine::Renderer::Frame::RetainedRenderTopologyState& retainedRenderTopology,
	const FrameSceneRevisions& sceneRevisions, std::uint64_t snapshotRendererGeneration,
	const TerraEngine::Renderer::BackendCapabilityProfile& backendCapabilities,
	std::vector<TerraEngine::Renderer::RenderPassNode>& renderGraphPasses)
{
	bool renderTopologyRebuilt = false;
	const auto capturedSceneGeneration = sceneRevisions.capturedSceneGeneration;
	const auto capturedComponentMutationRevision = sceneRevisions.capturedComponentMutationRevision;
	const auto capturedRenderBindingRevision = sceneRevisions.capturedRenderBindingRevision;
	const auto capturedHierarchyRevision = sceneRevisions.capturedHierarchyRevision;
	{

		TERRAENGINE_PROFILE_SCOPE("RenderSystem: Lower Render Snapshot",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const auto matchesRetainedOrder = [&]() {
			if (retainedRenderTopology.orderedObjectIds.size()
				!= renderFrameSnapshot.renderables.size())
			{
				return false;
			}
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				if (retainedRenderTopology.orderedObjectIds[index]
					!= renderFrameSnapshot.renderables[index].objectId)
				{
					return false;
				}
			}
			return true;
		};
		// Collectors normally preserve their canonical object order. Verify that
		// proof first and skip an O(n log n) sort for a stable crowd; any change
		// falls back to the existing deterministic sort before publication.
		const bool retainedOrderMatches = matchesRetainedOrder();
		if (!retainedOrderMatches)
		{
			TerraEngine::Renderer::SortSnapshotDeterministically(renderFrameSnapshot);
		}
		const std::uint64_t topologySceneGeneration = capturedSceneGeneration;
		const std::uint64_t topologyComponentMutationRevision =
			capturedComponentMutationRevision;
		const std::uint64_t topologyMeshComponentRevision =
			capturedRenderBindingRevision;
		const std::uint64_t topologyHierarchyRevision = capturedHierarchyRevision;
		// Exact monotonic producers prove whether any immutable input can have
		// changed. On a warm frame, this constant-time gate avoids hashing every
		// mesh/material/group key again. Object order is still checked above because
		// it is the index contract shared with frame-owned dynamic sidecars.
		const bool topologyObservationUnchanged = retainedRenderTopology.topology.valid
			&& retainedRenderTopology.owner == this
			&& retainedOrderMatches
			&& retainedRenderTopology.sceneGeneration == topologySceneGeneration
			&& retainedRenderTopology.componentMutationRevision
				== topologyComponentMutationRevision
			&& retainedRenderTopology.meshComponentRevision == topologyMeshComponentRevision
			&& retainedRenderTopology.hierarchyRevision == topologyHierarchyRevision
			&& retainedRenderTopology.resourceRevision == renderFrameSnapshot.resourceRevision
			&& retainedRenderTopology.rendererGeneration == snapshotRendererGeneration;
		bool topologyInputsChanged = !topologyObservationUnchanged;
		std::uint64_t stableTopologySignature = retainedRenderTopology.stableSignature;
		if (topologyInputsChanged)
		{
			// Fold only membership and immutable compatibility inputs after a producer
			// revision changed. Dynamic transforms, animation samples, visibility, LOD,
			// and page selection remain frame-owned sidecars and never enter this key.
			stableTopologySignature = kRenderHashOffset;
			for (const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable
				: renderFrameSnapshot.renderables)
			{
				const SharedRenderGroupKey key =
					TerraEngine::Renderer::BuildSharedRenderGroupKey(renderable, {});
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(renderable.objectId)));
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(key.meshId)));
				HashCombine(stableTopologySignature,
					static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(key.materialId)));
				HashCombine(stableTopologySignature, key.meshGeneration);
				HashCombine(stableTopologySignature, key.materialGeneration);
				HashCombine(stableTopologySignature, key.pipelineGeneration);
				HashCombine(stableTopologySignature, key.residencyGeneration);
				HashCombine(stableTopologySignature, key.rendererGeneration);
				HashCombine(stableTopologySignature, key.qualityVariant);
			}
		}
		if (topologyInputsChanged)
		{
			PersistentRenderTopology denseTopology =
				TerraEngine::Renderer::BuildPersistentRenderTopology(
					renderFrameSnapshot, stableTopologySignature);
			const std::uint64_t slotSessionGeneration =
				std::max<std::uint64_t>(1u, snapshotRendererGeneration);
			if(retainedRenderTopology.slotAllocator.rendererSessionGeneration
				!= slotSessionGeneration
				|| retainedRenderTopology.slotAllocator.topology == nullptr)
			{
				retainedRenderTopology.slotAllocator =
					TerraEngine::Renderer::CreateRenderSlotAllocatorState(
						slotSessionGeneration, 1u);
				retainedRenderTopology.slotKeysByObject.clear();
			}
			const std::uint64_t nextTopologyGeneration =
				retainedRenderTopology.slotAllocator.topologyGeneration + 1u;
			auto slotTransaction =
				TerraEngine::Renderer::BeginRenderSlotAllocatorTransaction(
					retainedRenderTopology.slotAllocator, nextTopologyGeneration);
			auto candidateSlotKeys = retainedRenderTopology.slotKeysByObject;
			std::unordered_set<TerraEngine::UUID> liveObjects;
			liveObjects.reserve(denseTopology.records.size());
			for(const PersistentRenderTopologyRecord& record : denseTopology.records)
				liveObjects.insert(record.objectId);

			// Release membership first so additions in the same protected snapshot can
			// deterministically reuse the lowest free ordinal with a new lifetime.
			for(auto existing = candidateSlotKeys.begin();
				existing != candidateSlotKeys.end();)
			{
				if(liveObjects.contains(existing->first))
				{
					++existing;
					continue;
				}
				if(!slotTransaction.Release(existing->second))
					throw std::runtime_error(
						"Renderer persistent-slot release rejected the captured topology transaction.");
				existing = candidateSlotKeys.erase(existing);
			}
			const auto resolveAllocatedSlot = [](
				const TerraEngine::Renderer::RenderSlotAllocatorState& state,
				TerraEngine::Renderer::RenderSlotGenerationKey key)
				-> const TerraEngine::Renderer::RenderPersistentSlot*
			{
				if(state.topology == nullptr || !key.IsValid()) return nullptr;
				const std::size_t pageIndex = key.ordinal
					/ TerraEngine::Renderer::kRenderPublicationPageCapacity;
				const std::size_t slotIndex = key.ordinal
					% TerraEngine::Renderer::kRenderPublicationPageCapacity;
				if(pageIndex >= state.topology->pages.size()
					|| state.topology->pages[pageIndex] == nullptr
					|| slotIndex >= state.topology->pages[pageIndex]->slots.size())
					return nullptr;
				const TerraEngine::Renderer::RenderPersistentSlot& slot =
					state.topology->pages[pageIndex]->slots[slotIndex];
				return slot.IsComplete() && slot.key == key ? &slot : nullptr;
			};
			for(PersistentRenderTopologyRecord& record : denseTopology.records)
			{
				if(!record.groupKey.IsComplete())
				{
					const auto stale = candidateSlotKeys.find(record.objectId);
					if(stale != candidateSlotKeys.end())
					{
						if(!slotTransaction.Release(stale->second))
							throw std::runtime_error(
								"Renderer incomplete persistent identity could not release its prior slot.");
						candidateSlotKeys.erase(stale);
					}
					continue;
				}
				std::uint64_t packetTemplateIdentity = kRenderHashOffset;
				HashCombine(packetTemplateIdentity, static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(record.groupKey.meshId)));
				HashCombine(packetTemplateIdentity, static_cast<std::uint64_t>(
					std::hash<TerraEngine::UUID>{}(record.groupKey.materialId)));
				HashCombine(packetTemplateIdentity, record.groupKey.meshGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.materialGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.pipelineGeneration);
				HashCombine(packetTemplateIdentity, record.groupKey.residencyGeneration);
				if(packetTemplateIdentity == 0u) packetTemplateIdentity = 1u;
				TerraEngine::Renderer::RenderSlotAllocationRequest request{};
				request.objectId = record.objectId;
				request.meshId = record.groupKey.meshId;
				request.materialId = record.groupKey.materialId;
				request.meshGeneration = record.groupKey.meshGeneration;
				request.materialGeneration = record.groupKey.materialGeneration;
				request.resourceGeneration = std::max(
					record.groupKey.pipelineGeneration,
					record.groupKey.residencyGeneration);
				request.packetTemplateIdentity = packetTemplateIdentity;
				request.routeTemplate = record.groupTemplateIndex;

				auto current = candidateSlotKeys.find(record.objectId);
				const TerraEngine::Renderer::RenderPersistentSlot* currentSlot =
					current != candidateSlotKeys.end()
					? resolveAllocatedSlot(retainedRenderTopology.slotAllocator,
						current->second) : nullptr;
				const bool persistentInputsMatch = currentSlot != nullptr
					&& currentSlot->meshId == request.meshId
					&& currentSlot->materialId == request.materialId
					&& currentSlot->meshGeneration == request.meshGeneration
					&& currentSlot->materialGeneration == request.materialGeneration
					&& currentSlot->resourceGeneration == request.resourceGeneration
					&& currentSlot->packetTemplateIdentity == request.packetTemplateIdentity
					&& currentSlot->routeTemplate == request.routeTemplate;
				if(persistentInputsMatch)
					continue;
				if(current != candidateSlotKeys.end())
				{
					if(!slotTransaction.Release(current->second))
						throw std::runtime_error(
							"Renderer persistent-slot replacement could not release its old lifetime.");
					candidateSlotKeys.erase(current);
				}
				const std::optional<TerraEngine::Renderer::RenderSlotGenerationKey> allocated =
					slotTransaction.Allocate(request);
				if(!allocated.has_value())
					throw std::runtime_error(
						"Renderer persistent-slot allocation rejected the complete topology transaction.");
				candidateSlotKeys.emplace(record.objectId, *allocated);
			}
			const std::optional<TerraEngine::Renderer::RenderSlotAllocatorState>
				committedSlots =
				slotTransaction.Commit();
			if(!committedSlots.has_value() || committedSlots->topology == nullptr)
				throw std::runtime_error(
					"Renderer persistent-slot transaction did not produce an immutable root.");
			retainedRenderTopology.slotAllocator = *committedSlots;
			retainedRenderTopology.slotKeysByObject = std::move(candidateSlotKeys);

			// Keep the legacy grouping helper as a sparse ordinal-indexed sidecar until
			// its remaining callers move to the publication resolver. Empty records are
			// released slots; live records keep allocator ordinals across mutations.
			PersistentRenderTopology sparseTopology{};
			sparseTopology.generation = committedSlots->topologyGeneration;
			sparseTopology.groupTemplates = denseTopology.groupTemplates;
			sparseTopology.records.resize(committedSlots->nextOrdinal);
			for(PersistentRenderTopologyRecord& record : denseTopology.records)
			{
				const auto key = retainedRenderTopology.slotKeysByObject.find(record.objectId);
				if(key == retainedRenderTopology.slotKeysByObject.end()) continue;
				record.stableOrdinal = key->second.ordinal;
				sparseTopology.records[key->second.ordinal] = record;
			}
			sparseTopology.valid = denseTopology.valid;
			retainedRenderTopology.topology = std::move(sparseTopology);
			retainedRenderTopology.stableSignature = stableTopologySignature;
			retainedRenderTopology.orderedObjectIds.clear();
			retainedRenderTopology.orderedSlotKeys.clear();
			retainedRenderTopology.renderableIndexByOrdinal.assign(
				committedSlots->nextOrdinal,
				TerraEngine::Renderer::RenderSlotGenerationKey::kInvalidOrdinal);
			retainedRenderTopology.conventionalVisibilityIdentityTable.clear();
			retainedRenderTopology.conventionalVisibilityIdentityIndices.clear();
			retainedRenderTopology.orderedObjectIds.reserve(
				renderFrameSnapshot.renderables.size());
			retainedRenderTopology.orderedSlotKeys.reserve(
				renderFrameSnapshot.renderables.size());
			retainedRenderTopology.conventionalVisibilityIdentityTable.resize(
				committedSlots->nextOrdinal);
			retainedRenderTopology.conventionalVisibilityIdentityIndices.reserve(
				renderFrameSnapshot.renderables.size());
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				// Zero remains the typed attachment's clear/no-producer sentinel.
				TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[index];
				const auto key = retainedRenderTopology.slotKeysByObject.find(
					renderable.objectId);
				if(key == retainedRenderTopology.slotKeysByObject.end()) continue;
				const std::uint32_t identityIndex = key->second.ordinal + 1u;
				renderable.conventionalVisibilityIdentityIndex = identityIndex;
				retainedRenderTopology.orderedObjectIds.push_back(renderable.objectId);
				retainedRenderTopology.orderedSlotKeys.push_back(key->second);
				retainedRenderTopology.renderableIndexByOrdinal[key->second.ordinal] =
					static_cast<std::uint32_t>(index);
				retainedRenderTopology.conventionalVisibilityIdentityTable[
					key->second.ordinal] = {
					identityIndex, renderable.objectId, renderable.meshId,
					renderable.materialId };
				retainedRenderTopology.conventionalVisibilityIdentityIndices.emplace(
					renderable.objectId, identityIndex);
			}
			retainedRenderTopology.sceneGeneration = topologySceneGeneration;
			retainedRenderTopology.componentMutationRevision =
				topologyComponentMutationRevision;
			retainedRenderTopology.meshComponentRevision = topologyMeshComponentRevision;
			retainedRenderTopology.hierarchyRevision = topologyHierarchyRevision;
			retainedRenderTopology.resourceRevision = renderFrameSnapshot.resourceRevision;
			retainedRenderTopology.rendererGeneration = snapshotRendererGeneration;
			retainedRenderTopology.owner = this;
			renderTopologyRebuilt = true;
		}
		else
		{
			// The static identity records are already canonical. Only restore the
			// one-based attachment for this independent dynamic snapshot. The retained
			// slot vector is index-aligned with the verified object order, avoiding a
			// UUID hash lookup for every object on every animated frame.
			for (std::size_t index = 0u;
				index < renderFrameSnapshot.renderables.size(); ++index)
			{
				renderFrameSnapshot.renderables[index]
					.conventionalVisibilityIdentityIndex =
					index < retainedRenderTopology.orderedSlotKeys.size()
						? retainedRenderTopology.orderedSlotKeys[index].ordinal + 1u
						: 0u;
			}
		}
		renderFrameSnapshot.conventionalVisibilityIdentityTable =
			retainedRenderTopology.conventionalVisibilityIdentityTable;
		renderGraphPasses = TerraEngine::Renderer::BuildDefaultRenderPasses(renderFrameSnapshot, backendCapabilities);

	}
	return renderTopologyRebuilt;
}
