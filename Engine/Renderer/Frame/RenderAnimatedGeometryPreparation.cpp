// Build owned deformation demand only after all view visibility has been resolved.
#include "RenderFrameState.h"
#include "RenderFrameCapture.h"
#include "Profiling/ProfilerScope.h"
#include "File Formats/TerraEngineMeshFile.h"

RenderSystem::AnimatedGeometryBuildResult RenderSystem::BuildFrameAnimatedGeometryPlan(
	const TerraEngine::Renderer::RenderFrameSnapshot& renderFrameSnapshot,
	const std::unordered_map<TerraEngine::UUID, const Mesh*>& frameMeshResources,
	std::uint64_t snapshotRendererGeneration, std::uint64_t protectedSceneGeneration,
	std::span<const std::uint8_t> visibleAnimatedRenderables,
	const std::unordered_map<TerraEngine::UUID, std::size_t>& renderableIndicesByObjectId)
{
		struct DeformablePageShape
		{
			std::uint32_t pageId = 0u;
			std::uint64_t contentGeneration = 0u;
			std::uint32_t vertexCount = 0u;
			std::uint32_t meshletCount = 0u;
		};
		struct DeformablePageFamily
		{
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
			std::vector<DeformablePageShape> pages;
		};
		struct AnimatedPageDemandSource
		{
			std::size_t renderableIndex = 0u;
			std::size_t pageFamilyIndex = 0u;
			std::size_t exactPageMaskOffset = 0u;
			std::uint64_t fallbackViewMask = 0u;
		};
		AnimatedGeometryBuildResult result;
		// A heterogeneous crowd still contains only a small number of exact mesh
		// families. Parse each immutable demanded-page directory once per frame,
		// then stamp the instance-specific generations onto the retained shape.
		std::vector<DeformablePageFamily> pageFamilies;
		pageFamilies.reserve(16u);
		thread_local std::vector<AnimatedPageDemandSource> demandSources;
		thread_local std::vector<std::size_t> sourceIndicesByRenderable;
		thread_local std::vector<std::uint64_t> resolvedExactPageMasks;
		demandSources.clear();
		demandSources.reserve(renderFrameSnapshot.renderables.size());
		sourceIndicesByRenderable.assign(renderFrameSnapshot.renderables.size(),
			std::numeric_limits<std::size_t>::max());
		resolvedExactPageMasks.clear();
		resolvedExactPageMasks.reserve(renderFrameSnapshot.renderables.size() * 3u);
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Gather Animated Page Sources",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(std::size_t renderableIndex = 0u;
				renderableIndex < renderFrameSnapshot.renderables.size(); ++renderableIndex)
			{
				const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[renderableIndex];
				if(renderableIndex >= visibleAnimatedRenderables.size()
					|| visibleAnimatedRenderables[renderableIndex] == 0u
					|| !TerraEngine::Renderer::
						CanScheduleGpuAnimatedPageDeformation(renderable.animation))
				{
					continue;
				}
				auto family = std::find_if(pageFamilies.begin(), pageFamilies.end(),
					[&renderable](const DeformablePageFamily& candidate)
					{
						return candidate.meshId == renderable.meshId;
					});
				if(family == pageFamilies.end())
				{
					DeformablePageFamily candidate;
					candidate.meshId = renderable.meshId;
					const auto capturedMesh = frameMeshResources.find(renderable.meshId);
					const Mesh* mesh = capturedMesh != frameMeshResources.end()
						? capturedMesh->second : nullptr;
					if(mesh != nullptr && mesh->hasDeformableDemandedPages
						&& mesh->deformableDemandedPageEntries.size()
							== mesh->deformableDemandedPageSections.size())
					{
						candidate.pages.reserve(mesh->deformableDemandedPageEntries.size());
						for(std::size_t pageIndex = 0u;
							pageIndex < mesh->deformableDemandedPageEntries.size(); ++pageIndex)
						{
							DeformablePageShape pageShape{};
							pageShape.pageId = mesh->deformableDemandedPageEntries[pageIndex].pageId;
							pageShape.contentGeneration =
								mesh->deformableDemandedPageEntries[pageIndex].contentGeneration;
							for(const TemfDeformablePageSection& section :
								mesh->deformableDemandedPageSections[pageIndex])
							{
								if(section.kind == TemfDeformablePageSectionKind::PackedVertices)
								{
									pageShape.vertexCount = section.elementCount;
								}
								else if(section.kind == TemfDeformablePageSectionKind::Meshlets)
								{
									pageShape.meshletCount = section.elementCount;
								}
							}
							if(pageShape.vertexCount != 0u && pageShape.meshletCount != 0u)
							{
								candidate.pages.push_back(pageShape);
							}
						}
					}
					pageFamilies.push_back(std::move(candidate));
					family = std::prev(pageFamilies.end());
				}
				const std::size_t sourceIndex = demandSources.size();
				const std::size_t exactPageMaskOffset = resolvedExactPageMasks.size();
				sourceIndicesByRenderable[renderableIndex] = sourceIndex;
				demandSources.push_back({
					renderableIndex,
					static_cast<std::size_t>(family - pageFamilies.begin()),
					exactPageMaskOffset,
					0u
				});
				resolvedExactPageMasks.resize(exactPageMaskOffset + family->pages.size(), 0u);
			}
		}
		thread_local std::vector<std::uint64_t> fallbackViewMasks;
		fallbackViewMasks.assign(renderFrameSnapshot.renderables.size(), 0U);
		const auto findExactPageMask = [&](std::size_t renderableIndex,
			std::uint32_t pageId) -> std::uint64_t*
		{
			if(renderableIndex >= sourceIndicesByRenderable.size())
			{
				return nullptr;
			}
			const std::size_t sourceIndex = sourceIndicesByRenderable[renderableIndex];
			if(sourceIndex >= demandSources.size())
			{
				return nullptr;
			}
			const AnimatedPageDemandSource& source = demandSources[sourceIndex];
			const std::vector<DeformablePageShape>& pages =
				pageFamilies[source.pageFamilyIndex].pages;
			for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
			{
				if(pages[pageIndex].pageId == pageId)
				{
					return &resolvedExactPageMasks[source.exactPageMaskOffset + pageIndex];
				}
			}
			return nullptr;
		};
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Accumulate Deformation View Masks",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// The canonical renderable index addresses a compact per-family mask range.
			// Exact cluster or packet pages win per view, while unmatched visible draws
			// safely retain every validated page owned by that animator.
			for(std::size_t viewIndex = 0u;
				viewIndex < renderFrameSnapshot.virtualGeometryViewSelections.size(); ++viewIndex)
			{
				const std::uint64_t viewMask = std::uint64_t{1u}
					<< std::min<std::size_t>(viewIndex, 63u);
				const TerraEngine::Renderer::VirtualGeometryViewSelectionState& view =
					renderFrameSnapshot.virtualGeometryViewSelections[viewIndex];
				for(const TerraEngine::Renderer::VirtualGeometryViewDrawRecord& draw :
					view.DrawRecords())
				{
					std::size_t renderableIndex = draw.renderableIndex;
					if(renderableIndex >= renderFrameSnapshot.renderables.size())
					{
						const auto renderable = renderableIndicesByObjectId.find(draw.objectId);
						if(renderable == renderableIndicesByObjectId.end())
						{
							continue;
						}
						renderableIndex = renderable->second;
					}
					if(renderableIndex < sourceIndicesByRenderable.size()
						&& sourceIndicesByRenderable[renderableIndex] < demandSources.size()
						&& draw.range.meshletCount != 0u
						&& (draw.range.virtualGeometryActive || draw.range.failOpen))
					{
						fallbackViewMasks[renderableIndex] |= viewMask;
					}
					const std::size_t firstCluster = std::min<std::size_t>(
						draw.range.selectedClusterOffset, view.SelectedClusters().size());
					const std::size_t clusterCount = std::min<std::size_t>(
						draw.range.selectedClusterCount,
						view.SelectedClusters().size() - firstCluster);
					for(std::size_t clusterIndex = firstCluster;
						clusterIndex < firstCluster + clusterCount; ++clusterIndex)
					{
						const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& cluster =
							view.SelectedClusters()[clusterIndex];
						if(cluster.objectId == draw.objectId)
						{
							if(std::uint64_t* exactMask = findExactPageMask(
								renderableIndex, cluster.pageId))
							{
								*exactMask |= viewMask;
							}
						}
					}
				}
				for(const GpuVirtualGeometryPacketRecord& packet : view.GpuPacketRecords())
				{
					const bool exactPage = packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentPagePayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentFallbackPayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::EmbeddedMeshPayload
						|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
					if(exactPage)
					{
						std::size_t renderableIndex = packet.renderableIndex;
						if(renderableIndex >= renderFrameSnapshot.renderables.size()
							|| renderFrameSnapshot.renderables[renderableIndex].objectId
								!= packet.objectId)
						{
							const auto renderable = renderableIndicesByObjectId.find(
								packet.objectId);
							if(renderable == renderableIndicesByObjectId.end())
							{
								continue;
							}
							renderableIndex = renderable->second;
						}
						if(std::uint64_t* exactMask = findExactPageMask(
							renderableIndex, packet.pageId))
						{
							*exactMask |= viewMask;
						}
					}
				}
			}
			for(AnimatedPageDemandSource& source : demandSources)
			{
				const std::vector<DeformablePageShape>& pages =
					pageFamilies[source.pageFamilyIndex].pages;
				std::uint64_t matchedViewMask = 0u;
				for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
				{
					matchedViewMask |= resolvedExactPageMasks[
						source.exactPageMaskOffset + pageIndex];
				}
				source.fallbackViewMask = fallbackViewMasks[source.renderableIndex]
					& ~matchedViewMask;
			}
		}
		TerraEngine::Renderer::GpuDeformationPlanAccumulator accumulator(
			renderFrameSnapshot.frameId,
			snapshotRendererGeneration,
			std::min(visibleAnimatedRenderables.size(),
				renderFrameSnapshot.renderables.size()));
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Accumulate Animated Page Demands",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(const AnimatedPageDemandSource& source : demandSources)
			{
				const TerraEngine::Renderer::RenderFrameRenderableRecord& renderable =
					renderFrameSnapshot.renderables[source.renderableIndex];
				const std::vector<DeformablePageShape>& pages =
					pageFamilies[source.pageFamilyIndex].pages;
				for(std::size_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
				{
					const DeformablePageShape& page = pages[pageIndex];
					const std::uint64_t exactViewMask = resolvedExactPageMasks[
						source.exactPageMaskOffset + pageIndex];
					const std::uint64_t requestedViewMask =
						source.fallbackViewMask | exactViewMask;
					if(requestedViewMask == 0u)
					{
						continue;
					}
					TerraEngine::Renderer::DeformationPageDemand demand;
					demand.key.page.meshId = renderable.meshId;
					demand.key.page.meshPayloadGeneration =
						renderable.animation.deformablePayloadGeneration;
					demand.key.page.pageId = page.pageId;
					demand.key.page.pageContentGeneration = page.contentGeneration;
					demand.key.page.residencyGeneration = page.contentGeneration;
					demand.key.page.layoutVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					// The runtime page currently uses the expanded production profile.
					// Other profiles publish distinct values and cannot alias this output.
					demand.key.page.codecProfile = 1u;
					demand.key.page.codecVersion = 1u;
					demand.key.deformation.stableInstanceId =
						renderable.animation.stableInstanceId;
					demand.key.deformation.objectGeneration =
						std::max<std::uint64_t>(1u, renderable.invalidation.asset);
					demand.key.deformation.animationPublicationGeneration =
						renderable.animation.poseGeneration;
					demand.key.deformation.skeletonGeneration =
						std::max<std::uint64_t>(1u, renderable.animation.programGeneration);
					demand.key.deformation.morphGeneration =
						renderable.animation.morphGeneration;
					demand.key.deformation.meshPayloadGeneration =
						renderable.animation.deformablePayloadGeneration;
					demand.key.deformation.rendererGeneration = snapshotRendererGeneration;
					demand.key.deformation.sceneGeneration =
						std::max<std::uint64_t>(1u, protectedSceneGeneration);
					demand.key.deformation.contentIdLow =
						renderable.animation.deformationContentIdLow;
					demand.key.deformation.contentIdHigh =
						renderable.animation.deformationContentIdHigh;
					demand.key.inputLayoutVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					demand.key.animationLod = 0u;
					demand.key.animationLodVersion =
						TerraEngine::Renderer::kAnimatedGeometryAbiVersion;
					demand.sourceRecordIndex = renderable.animation.firstGpuWorkPacket;
					demand.ownerStableInstanceId = renderable.animation.stableInstanceId;
					demand.sourceStableInstanceId = renderable.animation.stableInstanceId;
					demand.vertexCount = page.vertexCount;
					demand.meshletCount = page.meshletCount;
					// Each canonical demanded-page entry owns exactly one cluster.
					demand.clusterCount = 1u;
					demand.requestedViewMask = requestedViewMask;
					demand.priorHistoryCompatible =
						renderable.invalidation.pose == renderable.animation.poseGeneration
						&& renderable.invalidation.morph
							== renderable.animation.morphGeneration;
					++result.demandCount;
					(void)accumulator.Add(demand);
				}
			}
		}
		if(result.demandCount != 0u)
		{
			TERRAENGINE_PROFILE_SCOPE("RenderSystem: Finalize Deformation Plan",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			result.immutablePlan =
				std::make_shared<const TerraEngine::Renderer::GpuDeformationPlan>(
					std::move(accumulator).Finish());
		}
		return result;
	}
