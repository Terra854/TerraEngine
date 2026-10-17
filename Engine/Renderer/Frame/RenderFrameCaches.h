// Renderer-owned retained caches and typed prepared packets. These declarations
// are internal to frame implementation files; they do not expose mutable state.
#pragma once
#include "Renderer/RenderSystem.h"
#include "Renderer/Terrain/TerrainRenderProxyRegistry.h"
#include "Resource/ResourceStreaming.h"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Renderer::Frame {
	// Typed lane conversions shared by snapshot preparation and cache consumers.
	RenderItemRecord::PublishedAnimationGeneration
	BuildPublishedAnimationGeneration(
		const TerraEngine::Renderer::AnimationRenderBinding& source) noexcept;
	RenderViewCommandLane MakeRenderViewCommandLane(
		const RenderItemRecord& command) noexcept;
	void ApplyRenderViewCommandLane(const RenderViewCommandLane& lane,
		std::uint32_t selectedLod, TerraEngine::Renderer::GeometryRoute route,
		const glm::vec4& editorTint, RenderItemRecord& command) noexcept;

	struct CachedDrawCommandRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		RenderItemRecord command;
	};

	struct DrawCommandCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedDrawCommandRecord> recordsByObjectId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
		// Stale-object pruning is membership work, not frame work. These exact
		// protected revisions let a stable crowd skip rebuilding an alive UUID set
		// and rescanning the complete cache on every rendered frame.
		std::uint64_t observedSceneGeneration = 0U;
		std::uint64_t observedActiveMembershipRevision = 0U;
		std::uint64_t observedComponentRevision = 0U;
		std::uint64_t observedHierarchyRevision = 0U;
		std::uint64_t observedSpatialRevision = 0U;
		std::uint64_t observedMeshComponentRevision = 0U;
		bool baseInputsValid = false;
	};

	struct RenderableFrameIdentityCacheState
	{
		// Visible-cache animation refreshes need object-to-frame indices. Retain the
		// map while the exact deterministic object order is unchanged instead of
		// allocating and hashing thousands of UUIDs every frame.
		std::vector<TerraEngine::UUID> objectIds;
		std::vector<std::uint64_t> drawCommandSignatures;
		std::unordered_map<TerraEngine::UUID, std::size_t> indicesByObjectId;
		std::uint64_t worldSignature = 0U;
		// Static frame caches outlive a project reload. Require the protected scene
		// generation as well as matching UUID order before borrowing retained state.
		std::uint64_t sceneGeneration = 0U;
		bool valid = false;
	};

	struct RenderableDrawWorkingSet
	{
		// Base draw commands deliberately retain no animation publication. That
		// permits these coordinator-owned arrays to reuse capacity across frames
		// without extending pose lifetimes; visible commands attach the current
		// frame binding only when they are materialized below.
		std::vector<RenderItemRecord> commands;
		std::vector<std::uint64_t> signatures;
	};

	struct CachedLightCandidateRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		FrameLightCandidate candidate;
	};

	struct LightCandidateCacheState
	{
		std::unordered_map<TerraEngine::UUID, CachedLightCandidateRecord> recordsByLightId;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandCacheRecord
	{
		std::uint64_t signature = 0u;
		std::uint64_t lastTouchedFrameId = 0u;
		// Stable views retain canonical ordinals plus only their view-specific
		// selectors. Shared identity, material, and transform state is resolved from
		// the current working set and later from the sealed publication roots.
		std::vector<std::uint32_t> stableOrdinals;
		std::vector<std::uint32_t> selectedLods;
		std::vector<std::uint32_t> routeSelectors;
		std::vector<glm::vec4> editorTints;
		std::shared_ptr<const std::vector<RenderViewCommandLane>> commandLanes;
		// These two immutable arrays are populated lazily on the first cache hit,
		// then retained by sealed publications without another crowd-sized rebuild.
		mutable std::shared_ptr<const TerraEngine::Renderer::
			RenderViewSelectionPayload> retainedViewSelection;
		mutable std::shared_ptr<const std::vector<std::uint32_t>>
			retainedCommandSelectionIndices;
		// Cached direct-route commands never contribute a VG view draw. Retaining
		// only the exact contributing command indices prevents a large direct
		// crowd from being rescanned merely to republish an empty VG selection.
		std::vector<std::uint32_t> virtualGeometryCommandIndices;
		std::shared_ptr<const TerraEngine::Renderer::
			VirtualGeometryRetainedSelectionEvidence> retainedSelectionEvidence;
		std::shared_ptr<const VirtualGeometryRetainedGpuEvidence>
			retainedGpuEvidence;
		// View draw ranges are derived entirely from this revision-validated cut.
		// Retaining them avoids reconstructing the same per-command range records on
		// every cache hit, including valid direct-only cuts whose vector is empty.
		std::shared_ptr<const std::vector<
			TerraEngine::Renderer::VirtualGeometryViewDrawRecord>> viewDraws;
		bool hasViewDrawEvidence = false;
		// Page evidence depends only on selection and residency inputs already
		// covered by this record's signature. Cache it with the cut so stable views
		// do not rebuild hashes and diagnostic strings every frame.
		TerraEngine::Renderer::VisibleStreamingTouchSet streamingTouchSet;
		TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot terrainTextureTouches;
		TerraEngine::Renderer::VisibleCommandReuseTelemetry reuseTelemetry;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
	};

	struct VisibleCommandCacheState
	{
		std::unordered_map<std::uint64_t, VisibleCommandCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct ShadowDrawListCacheRecord
	{
		std::uint64_t signature = 0u;
		// Fixture cache selectors retain exact identity beyond the mismatch hash.
		RenderViewType controlledViewType = RenderViewType::Scene;
		std::uint32_t controlledOutputId = 0;
		std::uint64_t fixtureControlGeneration = 0;
		std::vector<TerraEngine::UUID> controlledCasterIds;
		bool controlledCasterSelection = false;
		std::uint64_t lastTouchedFrameId = 0u;
		std::vector<ShadowDrawList> shadowDrawLists;
	};

	struct ShadowDrawListCacheState
	{
		std::unordered_map<std::uint64_t, ShadowDrawListCacheRecord> recordsBySignature;
		std::size_t hitCount = 0u;
		std::size_t missCount = 0u;
	};

	struct VisibleCommandBuildResult
	{
		std::uint64_t packetEvidenceSignature = 0u;
		std::vector<RenderItemRecord> commands;
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
		TerraEngine::Renderer::VisibleStreamingTouchSet streamingTouchSet;
		TerraEngine::Resource::TerrainVisibleTextureTouchSnapshot terrainTextureTouches;
		TerraEngine::Renderer::VisibleCommandReuseTelemetry reuseTelemetry;
		TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics;
		// A cache hit borrows one stable unordered_map value until this result is
		// consumed immediately by the current view. Rehash preserves references,
		// and trimming cannot run again before the caller copies these spans into
		// its protected RenderViewBuildState.
		const VisibleCommandCacheRecord* cachedRecord = nullptr;
		bool cacheHit = false;

		template<typename RenderableContainer, typename CommandContainer>
		void PopulateSelectedItems(
			std::vector<RenderItemRecord>& destination,
			const RenderableContainer& currentRenderables,
			const CommandContainer& baseCommands,
			std::vector<std::uint8_t>* visibleAnimatedFlags,
			const std::unordered_map<TerraEngine::UUID, std::size_t>*
				currentRenderableIndices,
			std::uint64_t* groupingCompatibilitySignature) const
		{
			std::uint64_t animationCompatibilitySignature = 1469598103934665603ULL;
			const auto appendAnimationCompatibility =
				[&animationCompatibilitySignature](const RenderItemRecord& command)
			{
				const std::uint64_t authorityLane =
					static_cast<std::uint32_t>(command.animationBinding.source)
					| (static_cast<std::uint64_t>(
						command.animationBinding.RequiresConservativeCoarseVisibility()) << 32U)
					| (static_cast<std::uint64_t>(
						command.animationBinding.status) << 40U);
				animationCompatibilitySignature ^= authorityLane + 0x9e3779b97f4a7c15ULL
					+ (animationCompatibilitySignature << 6U)
					+ (animationCompatibilitySignature >> 2U);
				// Visibility setup resolves deformation bridge offsets by stable owner.
				// Fold that owner into the proof while the refreshed binding is hot.
				const std::uint64_t stableInstanceId =
					command.animation.stableInstanceId;
				animationCompatibilitySignature ^= stableInstanceId
					+ 0x9e3779b97f4a7c15ULL
					+ (animationCompatibilitySignature << 6U)
					+ (animationCompatibilitySignature >> 2U);
			};
			if(cachedRecord == nullptr || !commands.empty())
			{
				destination = commands;
				for(const RenderItemRecord& command : destination)
				{
					appendAnimationCompatibility(command);
					if(visibleAnimatedFlags == nullptr
						|| currentRenderableIndices == nullptr
						|| command.animationBinding.status
						!= TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
					{
						continue;
					}
					const auto current = currentRenderableIndices->find(command.objectId);
					if(current != currentRenderableIndices->end()
						&& current->second < visibleAnimatedFlags->size())
					{
						(*visibleAnimatedFlags)[current->second] = 1u;
					}
				}
			}
			else
			{
				destination.clear();
				const std::size_t bindingCount = cachedRecord->stableOrdinals.size();
				destination.reserve(bindingCount);
				for(std::size_t index = 0U; index < bindingCount; ++index)
				{
					const std::uint32_t renderableIndex =
						cachedRecord->stableOrdinals[index];
					if(renderableIndex < currentRenderables.size()
						&& renderableIndex < baseCommands.size()
						&& index < cachedRecord->selectedLods.size()
						&& index < cachedRecord->routeSelectors.size()
						&& index < cachedRecord->editorTints.size()
						&& cachedRecord->commandLanes != nullptr
						&& index < cachedRecord->commandLanes->size()) {
						RenderItemRecord command = baseCommands[renderableIndex];
						command.animation = BuildPublishedAnimationGeneration(
							currentRenderables[renderableIndex].animationBinding);
						command.animationBinding =
							TerraEngine::Renderer::PublishedAnimationRenderBinding(
								currentRenderables[renderableIndex].animationBinding);
						ApplyRenderViewCommandLane((*cachedRecord->commandLanes)[index],
							cachedRecord->selectedLods[index],
							static_cast<TerraEngine::Renderer::GeometryRoute>(
								cachedRecord->routeSelectors[index]),
							cachedRecord->editorTints[index], command);
						destination.push_back(std::move(command));
						if(visibleAnimatedFlags != nullptr
							&& renderableIndex < visibleAnimatedFlags->size()
							&& destination.back().animationBinding.status
								== TerraEngine::Renderer::AnimationRenderBindingStatus::Ready)
						{
							(*visibleAnimatedFlags)[renderableIndex] = 1u;
						}
						appendAnimationCompatibility(destination.back());
					}
				}
				if(bindingCount != destination.size())
				{
					// A partial cache-to-renderable mapping cannot prove the omitted
					// commands. Disable downstream reuse so the complete validation path
					// remains authoritative for malformed or stale cache records.
					animationCompatibilitySignature = 0u;
				}
			}
			if(groupingCompatibilitySignature != nullptr)
			{
				// Visibility evidence already covers every static grouping lane. Add
				// only the current animation authority classes while bindings are hot,
				// avoiding a second full command scan in grouped submission planning.
				*groupingCompatibilitySignature = packetEvidenceSignature;
				if(animationCompatibilitySignature == 0u)
				{
					*groupingCompatibilitySignature = 0u;
				}
				else
				{
					*groupingCompatibilitySignature ^= animationCompatibilitySignature
						+ 0x9e3779b97f4a7c15ULL
						+ (*groupingCompatibilitySignature << 6U)
						+ (*groupingCompatibilitySignature >> 2U);
				}
			}
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketRecord>& GpuPacketRecords() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->packetRecords : gpuPacketRecords;
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryBatchDescriptor>& GpuBatchDescriptors() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->batchDescriptors : gpuBatchDescriptors;
		}
		[[nodiscard]] const std::vector<GpuVirtualGeometryPacketFrameSet>& GpuPacketFrameSets() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->packetFrameSets : gpuPacketFrameSets;
		}
		[[nodiscard]] const std::vector<GpuPacketCompactionDispatch>& GpuCompactionDispatches() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->compactionDispatches : gpuCompactionDispatches;
		}
		[[nodiscard]] const std::vector<GeneratedVirtualGeometrySubmissionRecord>& GeneratedSubmissions() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->generatedSubmissions : generatedSubmissions;
		}
		[[nodiscard]] const std::vector<VirtualGeometryCounterReadbackRecord>& GpuCounterReadbacks() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->counterReadbacks : gpuCounterReadbacks;
		}
		[[nodiscard]] const std::vector<VirtualGeometryGpuFallbackRecord>& GpuFallbackRecords() const noexcept
		{
			return cachedRecord ? cachedRecord->retainedGpuEvidence->fallbackRecords : gpuFallbackRecords;
		}
	};

	struct GroupedCompatibilityBucket
	{
		std::uint32_t topologyTemplateIndex =
			PersistentRenderTopologyRecord::kInvalidGroupTemplateIndex;
		std::uint32_t representative = 0u;
		std::vector<std::uint32_t> members;
	};

	struct GroupedSubmissionMembershipCacheRecord
	{
		bool valid = false;
		std::uint64_t compatibilitySignature = 0U;
		std::uint64_t lastTouchedFrameId = 0U;
		std::vector<GroupedCompatibilityBucket> buckets;
		// These descriptors and preparation indices depend only on the stable
		// compatibility membership above. Frame, view, and renderer generations are
		// patched when the cache is consumed, so a warm frame does not rediscover the
		// same group layout or rebuild its fallback coverage.
		std::vector<GroupedRenderSubmission> submissionTemplates;
		std::vector<std::uint32_t> drawResourcePreparationCommandIndices;
	};

	struct GroupedSubmissionMembershipCacheState
	{
		const RenderSystem* owner = nullptr;
		std::unordered_map<std::uint64_t, GroupedSubmissionMembershipCacheRecord>
			recordsByView;
	};

	// Persistent renderer preparation belongs to RenderSystem, but its concrete
	// cache types remain private to this implementation file.
	struct RetainedRenderTopologyState
	{
		std::uint64_t stableSignature = 0u;
		PersistentRenderTopology topology{};
		// Slot ownership survives topology rebuilds. The immutable allocator value
		// is replaced only after a complete membership transaction commits, while
		// compact selections retain the exact lifetime key for recycled ordinals.
		TerraEngine::Renderer::RenderSlotAllocatorState slotAllocator{};
		std::unordered_map<TerraEngine::UUID,
			TerraEngine::Renderer::RenderSlotGenerationKey> slotKeysByObject;
		std::vector<TerraEngine::UUID> orderedObjectIds;
		// These two sidecars share render-frame snapshot order. Stable frames can
		// therefore recover both the one-based identity and the reverse ordinal
		// mapping without repeating thousands of UUID hash-table probes.
		std::vector<TerraEngine::Renderer::RenderSlotGenerationKey> orderedSlotKeys;
		std::vector<std::uint32_t> renderableIndexByOrdinal;
		std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityRecord>
			conventionalVisibilityIdentityTable;
		std::unordered_map<TerraEngine::UUID, std::uint32_t>
			conventionalVisibilityIdentityIndices;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t componentMutationRevision = 0u;
		std::uint64_t meshComponentRevision = 0u;
		std::uint64_t hierarchyRevision = 0u;
		std::uint64_t resourceRevision = 0u;
		std::uint64_t rendererGeneration = 0u;
		const RenderSystem* owner = nullptr;
	};

	struct SpatialPreparationCacheState
	{
		std::uint64_t signature = 0U;
		bool valid = false;
		std::uint64_t observedSceneGeneration = 0U;
		std::uint64_t observedSpatialRevision = 0U;
		std::uint64_t observedMeshComponentRevision = 0U;
		std::uint64_t observedMeshResourceRevision = 0U;
		std::size_t observedRenderableCount = 0U;
		std::vector<TerraEngine::Scene::SpatialObjectRecord> records;
		std::unordered_map<entt::entity, std::size_t> renderableLookup;
		std::vector<bool> conservativeAnimationBounds;
	};

	struct VirtualGeometryViewSelectionPacket
	{
		// RenderSystem prepares these vectors from immutable mesh/runtime data
		// before the protected GPU submit boundary. Streaming and diagnostics
		// consume the copied packet, not ResourceManager's live mesh cache.
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		std::vector<TerraEngine::Renderer::VirtualGeometryPageNeedRecord> pageNeeds;
		std::vector<VirtualGeometryClusterPacket> clusterPackets;
		std::vector<ClusterWorkBatch> clusterBatches;
		std::vector<GpuVirtualGeometryPacketRecord> gpuPacketRecords;
		std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatchDescriptors;
		std::vector<GpuVirtualGeometryPacketFrameSet> gpuPacketFrameSets;
		std::vector<GpuPacketCompactionDispatch> gpuCompactionDispatches;
		std::vector<GeneratedVirtualGeometrySubmissionRecord> generatedSubmissions;
		std::vector<VirtualGeometryCounterReadbackRecord> gpuCounterReadbacks;
		std::vector<VirtualGeometryGpuFallbackRecord> gpuFallbackRecords;
		std::vector<CompactedVisibilitySet> compactionResults;
		std::vector<FallbackCoverageChain> fallbackCoverage;
		std::vector<VirtualGeometryLODTransitionState> lodTransitionStates;
	};

}
