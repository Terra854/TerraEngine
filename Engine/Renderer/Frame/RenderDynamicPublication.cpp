// Immutable dynamic pages borrow captured rows only until all page jobs have settled.
#include "RenderFrameSealing.h"
#include "RenderPreparationTasks.h"
#include <cstring>
void RenderSystem::PrepareFrameDynamicPages(
	const std::shared_ptr<TerraEngine::Renderer::RenderFramePublication>& publication,
	const TerraEngine::Renderer::Frame::RetainedRenderTopologyState& retainedRenderTopology,
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const FrameCommandIdentity& commandIdentity, const FrameSceneRevisions& revisions,
	const FrameResourceCohort& resourceCohort, std::span<const std::uint32_t> cpuDeformationRenderableIndices,
	bool hasDenseAnimationPublication, std::size_t renderGraphWorkerCount)
{
	const auto publicationSessionGeneration = publication->rendererSessionGeneration;
	const auto& allocatorTopology = publication->persistent;
	const auto renderableWorldSignature = commandIdentity.world;
	const auto renderableSubmissionSignature = commandIdentity.submission;
	const auto capturedAnimatorComponentRevision = revisions.capturedAnimatorComponentRevision;
	const auto capturedHierarchyRevision = revisions.capturedHierarchyRevision;
	const auto capturedAnimationBindingResourceRevision = resourceCohort.capturedAnimationBindingResourceRevision;
	const auto virtualGeometryResidencyGeneration = resourceCohort.capturedVirtualGeometryResidencyGeneration;
		auto dynamicRoot = std::make_shared<
			TerraEngine::Renderer::RenderDynamicStateRoot>();
		dynamicRoot->generation = std::max<std::uint64_t>(1u, activeRenderFrameId);
		dynamicRoot->rendererSessionGeneration = publicationSessionGeneration;
		// Dense GPU animation is a separate immutable publication root. Its pose and
		// plan generations advance every frame, but the renderer's geometry lanes do
		// not: transforms, resources, packet ordinals, and binding topology remain
		// stable. Keep those pages until a real geometry/binding mutation occurs and
		// resolve the current animation generations from backendWork at consumption.
		std::uint64_t dynamicSourceSignature = hasDenseAnimationPublication
			? renderableWorldSignature : renderableSubmissionSignature;
		if(hasDenseAnimationPublication) {
			HashCombine(dynamicSourceSignature,
				capturedAnimatorComponentRevision);
			HashCombine(dynamicSourceSignature, capturedHierarchyRevision);
			HashCombine(dynamicSourceSignature,
				capturedAnimationBindingResourceRevision);
			HashCombine(dynamicSourceSignature, renderFrameSnapshot.resourceRevision);
		}
		HashCombine(dynamicSourceSignature, allocatorTopology->generation);
		HashCombine(dynamicSourceSignature, virtualGeometryResidencyGeneration);
		HashCombine(dynamicSourceSignature, publicationSessionGeneration);
		const auto lanesEqual = [](const TerraEngine::Renderer::RenderDynamicLane& left,
			const TerraEngine::Renderer::RenderDynamicLane& right) noexcept
		{
			return left.key == right.key && left.transformGeneration == right.transformGeneration
				&& (left.shadowCasterInputs == right.shadowCasterInputs
					|| (left.shadowCasterInputs != nullptr && right.shadowCasterInputs != nullptr
						&& *left.shadowCasterInputs == *right.shadowCasterInputs))
				&& left.hasCanonicalWorldPose == right.hasCanonicalWorldPose
				&& left.canonicalWorldPose.position == right.canonicalWorldPose.position
				&& left.canonicalWorldPose.rotation == right.canonicalWorldPose.rotation
				&& left.canonicalWorldPose.scale == right.canonicalWorldPose.scale
				&& left.animationPresentationGeneration == right.animationPresentationGeneration
				&& left.animationBinding.animationAuthorityGeneration
					== right.animationBinding.animationAuthorityGeneration
				&& left.animationBinding.posePublicationGeneration
					== right.animationBinding.posePublicationGeneration
				&& left.animationBinding.source == right.animationBinding.source
				&& left.animationBinding.status == right.animationBinding.status
				&& left.animationBinding.safePose == right.animationBinding.safePose
				&& left.animationBinding.cpuDeformation
					== right.animationBinding.cpuDeformation
				&& left.animationBinding.cpuDeformationRecordIndex
					== right.animationBinding.cpuDeformationRecordIndex
				&& left.animation == right.animation
				&& left.materialOverrideGeneration == right.materialOverrideGeneration
				&& left.boundsGeneration == right.boundsGeneration
				&& left.lifecycleGeneration == right.lifecycleGeneration
				&& left.resourceGeneration == right.resourceGeneration
				&& left.occupied == right.occupied
				&& std::memcmp(&left.previousModelMatrix, &right.previousModelMatrix,
					sizeof(glm::mat4)) == 0
				&& std::memcmp(&left.instance, &right.instance,
					sizeof(RenderInstanceRecord)) == 0;
		};
		const std::shared_ptr<const TerraEngine::Renderer::RenderDynamicStateRoot>
			previousDynamicRoot = retainedDynamicStateRoot != nullptr
				&& retainedDynamicStateRoot->rendererSessionGeneration
					== publicationSessionGeneration
				? retainedDynamicStateRoot : nullptr;
		const bool reusableDynamicBase = previousDynamicRoot != nullptr
			&& retainedDynamicStateSourceSignature == dynamicSourceSignature
			&& previousDynamicRoot->pages.size() == publication->persistent->pages.size();
		std::vector<std::uint32_t> currentCpuDynamicPageIndices;
		bool cpuPageMappingComplete = true;
		if(hasDenseAnimationPublication) {
			currentCpuDynamicPageIndices.reserve(
				cpuDeformationRenderableIndices.size());
			for(const std::uint32_t renderableIndex :
				cpuDeformationRenderableIndices) {
				if(renderableIndex >= retainedRenderTopology.orderedSlotKeys.size()) {
					cpuPageMappingComplete = false;
					break;
				}
				const TerraEngine::Renderer::RenderSlotGenerationKey key =
					retainedRenderTopology.orderedSlotKeys[renderableIndex];
				if(!key.IsValid()) {
					cpuPageMappingComplete = false;
					break;
				}
				currentCpuDynamicPageIndices.push_back(
					key.ordinal
						/ TerraEngine::Renderer::kRenderPublicationPageCapacity);
			}
			std::sort(currentCpuDynamicPageIndices.begin(),
				currentCpuDynamicPageIndices.end());
			currentCpuDynamicPageIndices.erase(std::unique(
				currentCpuDynamicPageIndices.begin(),
				currentCpuDynamicPageIndices.end()),
				currentCpuDynamicPageIndices.end());
		}
		const bool retainCompleteDynamicRoot = reusableDynamicBase
			&& cpuPageMappingComplete
			&& currentCpuDynamicPageIndices.empty()
			&& retainedCpuDynamicPageIndices.empty();
		std::vector<std::shared_ptr<const TerraEngine::Renderer::RenderDynamicPage>>
			preparedDynamicPages(publication->persistent->pages.size());
		std::vector<std::uint8_t> changedDynamicPages(
			publication->persistent->pages.size(), 0U);
		std::vector<std::uint8_t> pagesRequiringDynamicRefresh(
			publication->persistent->pages.size(), reusableDynamicBase ? 0U : 1U);
		if(reusableDynamicBase) {
			preparedDynamicPages = previousDynamicRoot->pages;
			const auto markCpuPages = [&pagesRequiringDynamicRefresh](
				const std::vector<std::uint32_t>& pageIndices) noexcept {
				for(const std::uint32_t pageIndex : pageIndices) {
					if(pageIndex < pagesRequiringDynamicRefresh.size()) {
						pagesRequiringDynamicRefresh[pageIndex] = 1U;
					}
				}
			};
			markCpuPages(retainedCpuDynamicPageIndices);
			markCpuPages(currentCpuDynamicPageIndices);
		}
		if(!cpuPageMappingComplete) {
			std::fill(pagesRequiringDynamicRefresh.begin(),
				pagesRequiringDynamicRefresh.end(), 1U);
		}
		// Keep a few retired page sets ready for the asynchronous backend without
		// allowing an unusually slow consumer to turn this cache into unbounded
		// retained memory. Acquire itself refuses to hand out any page still owned
		// by a sealed publication.
		const std::size_t dynamicPagePoolLimit = std::max<std::size_t>(
			16u, publication->persistent->pages.size() * 3u);
		// Each immutable page owns an independent 256-lane range. Build those ranges
		// concurrently and join before publishing the root; no worker touches live
		// ECS state or another page, so ordering and atomic visibility remain intact.
		if(!retainCompleteDynamicRoot)
		{
                  RunRenderChunkedWork(
                      publication->persistent->pages.size(),
                      renderGraphWorkerCount, 2U,
                      "RenderSystem Task: Build Dynamic Publication Pages",
                      [&](std::size_t pageBegin, std::size_t pageEnd) {
                        for (std::size_t pageIndex = pageBegin;
                             pageIndex < pageEnd; ++pageIndex) {
						  if(pagesRequiringDynamicRefresh[pageIndex] == 0U)
							continue;
                          const std::shared_ptr<
                              const TerraEngine::Renderer::RenderPersistentPage>
                              &persistentPage =
                                  publication->persistent->pages[pageIndex];
                          if (persistentPage == nullptr)
                            throw std::runtime_error(
                                "Renderer dynamic publication cannot address a "
                                "missing persistent page.");
						  auto page = dynamicPageRecyclePool.Acquire(
						      dynamicPagePoolLimit);
						  // This page has no publication owner when Acquire returns it.
						  // Clearing retains vector capacity, avoiding repeated 256-lane
						  // heap allocations for every stable animated crowd frame.
						  page->lanes.clear();
						  page->changedLanes.clear();
						  page->generation = dynamicRoot->generation;
                          page->firstOrdinal = persistentPage->firstOrdinal;
                          page->lanes.reserve(persistentPage->slots.size());
                          page->changedLanes.reserve(
                              persistentPage->slots.size());
                          const std::shared_ptr<
                              const TerraEngine::Renderer::RenderDynamicPage>
                              previousPage =
                                  previousDynamicRoot != nullptr &&
                                          pageIndex <
                                              previousDynamicRoot->pages.size()
                                      ? previousDynamicRoot->pages[pageIndex]
                                      : nullptr;
                          bool anyLaneChanged =
                              previousPage == nullptr ||
                              previousPage->lanes.size() !=
                                  persistentPage->slots.size();
                          for (std::size_t laneIndex = 0u;
                               laneIndex < persistentPage->slots.size();
                               ++laneIndex) {
                            const TerraEngine::Renderer::RenderPersistentSlot
                                &persistent = persistentPage->slots[laneIndex];
                            TerraEngine::Renderer::RenderDynamicLane lane{};
                            if (!persistent.occupied) {
                              const bool laneChanged =
                                  previousPage == nullptr ||
                                  laneIndex >= previousPage->lanes.size() ||
                                  previousPage->lanes[laneIndex].occupied;
                              page->changedLanes.push_back(laneChanged);
                              page->lanes.push_back(std::move(lane));
                              anyLaneChanged = anyLaneChanged || laneChanged;
                              continue;
                            }
                            const std::uint32_t ordinal =
                                persistent.key.ordinal;
                            const std::uint32_t renderableIndex =
                                ordinal < retainedRenderTopology
                                              .renderableIndexByOrdinal.size()
                                    ? retainedRenderTopology
                                          .renderableIndexByOrdinal[ordinal]
                                    : TerraEngine::Renderer::
                                          RenderSlotGenerationKey::
                                              kInvalidOrdinal;
                            if (renderableIndex == TerraEngine::Renderer::
                                                       RenderSlotGenerationKey::
                                                           kInvalidOrdinal ||
                                renderableIndex >=
                                    renderFrameSnapshot.renderables.size() ||
                                renderFrameSnapshot.renderables[renderableIndex]
                                        .objectId != persistent.objectId)
                              throw std::runtime_error(
                                  "Renderer persistent slot has no matching "
                                  "captured dynamic record.");
                            const TerraEngine::Renderer::
                                RenderFrameRenderableRecord &renderable =
                                    renderFrameSnapshot
                                        .renderables[renderableIndex];
                            lane.key = persistent.key;
							// Retained pages are view-independent. The precise canonical pose
							// below is localized by each sealed view's command resolver.
							lane.instance.modelMatrix = glm::mat4(1.0F);
							lane.canonicalWorldPose = renderable.canonicalWorldPose;
							lane.hasCanonicalWorldPose = true;
                            lane.previousModelMatrix =
                                previousPage != nullptr &&
                                        laneIndex <
                                            previousPage->lanes.size() &&
                                        previousPage->lanes[laneIndex].key ==
                                            lane.key &&
                                        previousPage->lanes[laneIndex].occupied
                                    ? previousPage->lanes[laneIndex]
                                          .instance.modelMatrix
									: glm::mat4(1.0F);
                            lane.instance.objectIdentity = glm::uvec4(
                                renderable.conventionalVisibilityIdentityIndex,
                                0u, 0u, 0u);
                            lane.instance.materialAndTexture = glm::uvec4(
                                0u, 0u, persistent.key.ordinal,
                                renderable.conventionalVisibilityIdentityIndex);
                            lane.instance.animationSelectors = glm::uvec4(
                                static_cast<std::uint32_t>(
                                    renderable.animation.stableInstanceId),
                                static_cast<std::uint32_t>(
                                    renderable.animation
                                        .animationPlanGeneration),
                                renderable.animation.firstGpuWorkPacket,
                                renderable.animation.gpuStageMask);
							lane.shadowCasterInputs = renderable.shadowCasterInputs;
                            lane.animationBinding = TerraEngine::Renderer::
                                PublishedAnimationRenderBinding(
                                    renderable.animationBinding);
                            lane.animation = BuildPublishedAnimationGeneration(
                                renderable.animation);
                            lane.instance.geometrySelectors = glm::uvec4(
                                renderable.virtualGeometry.selectedLodIndex,
                                renderable.virtualGeometry
                                    .payloadDescriptorIndex,
                                renderable.virtualGeometry.meshletOffset,
                                renderable.virtualGeometry.meshletCount);
							lane.transformGeneration =
								renderable.transformGeneration;
                            lane.animationPresentationGeneration =
                                renderable.animation.animationPlanGeneration;
                            lane.materialOverrideGeneration =
                                renderable.invalidation.material;
                            lane.boundsGeneration =
                                renderable.invalidation.asset;
                            lane.lifecycleGeneration =
                                renderable.invalidation.renderer;
                            lane.resourceGeneration =
                                renderable.invalidation.residency;
                            lane.occupied = true;
                            const bool laneChanged =
                                previousPage == nullptr ||
                                laneIndex >= previousPage->lanes.size() ||
                                !lanesEqual(previousPage->lanes[laneIndex],
                                            lane);
                            page->changedLanes.push_back(laneChanged);
                            page->lanes.push_back(std::move(lane));
                            anyLaneChanged = anyLaneChanged || laneChanged;
                          }
                          // The worker has now constructed and compared every
                          // fixed-address lane against this exact immutable
                          // topology page. Seal consumes this proof and retains
                          // the defensive scan for pages built by
                          // compatibility/test callers.
                          page->producerValidatedLaneCount = page->lanes.size();
                          page->producerValidatedPersistentGeneration =
                              persistentPage->generation;
                          if (!anyLaneChanged && previousPage != nullptr) {
                            preparedDynamicPages[pageIndex] = previousPage;
                          } else {
                            preparedDynamicPages[pageIndex] = std::move(page);
                            changedDynamicPages[pageIndex] = 1U;
                          }
                        }
                      });
                  dynamicRoot->pages = std::move(preparedDynamicPages);
                  publication->receipt.changedDynamicPages =
                      static_cast<std::uint32_t>(
                          std::count(changedDynamicPages.begin(),
                                     changedDynamicPages.end(), 1U));
                  retainedDynamicStateRoot = dynamicRoot;
                  retainedDynamicStateSourceSignature = dynamicSourceSignature;
				  retainedCpuDynamicPageIndices =
					  std::move(currentCpuDynamicPageIndices);
                  publication->dynamic = std::move(dynamicRoot);
                } else {
                  // The source signature includes topology, transforms,
                  // animation/resource generations, and VG residency.
                  // Camera-dependent selections are separate roots, so moving a
                  // camera does not invalidate scene-dynamic data.
                  publication->receipt.changedDynamicPages = 0U;
                  publication->dynamic = previousDynamicRoot;
                  retainedDynamicStateRoot = previousDynamicRoot;
				  retainedCpuDynamicPageIndices =
					  std::move(currentCpuDynamicPageIndices);
                }
}
