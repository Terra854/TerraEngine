#include "TestHarness.h"

#include <fstream>
#include <filesystem>
#include <iterator>
#include <string>

namespace TerraEngine::Tests
{
	void RegisterRenderPublicationSourceGuardTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PublicationSourceGuard.RemovesLegacySubmission", [](TestContext& context) {
			const auto readSource = [](const char* relativePath) {
				std::ifstream source(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath);
				return std::string((std::istreambuf_iterator<char>(source)), {});
			};
			const std::string renderSystem = readSource("Engine/Renderer/RenderSystem.cpp");
			const std::string publication = readSource(
				"Engine/Renderer/Graph/RenderFramePublication.h");
			const std::string publicationImplementation = readSource(
				"Engine/Renderer/Graph/RenderFramePublication.cpp");
			const std::string vulkan = readSource(
				"Engine/Renderer/RHI/Vulkan/VulkanContext.cpp");
			context.Expect(!renderSystem.empty() && !publication.empty()
				&& !publicationImplementation.empty() && !vulkan.empty(),
				"The renderer source guard must inspect every command-publication boundary.");
			context.Expect(renderSystem.find("MaterializeCommands") == std::string::npos,
				"Renderer publication must not reconstruct legacy command collections.");
			const std::string renderTypes = readSource("Engine/Renderer/RenderTypes.h");
			const std::size_t shadowType = renderTypes.find("struct ShadowDrawList");
			const std::size_t shadowTypeEnd = renderTypes.find("};", shadowType);
			context.Expect(shadowType != std::string::npos
				&& shadowTypeEnd != std::string::npos
				&& renderTypes.substr(shadowType, shadowTypeEnd - shadowType)
					.find("RenderItemRecord") == std::string::npos
				&& renderTypes.substr(shadowType, shadowTypeEnd - shadowType)
					.find("commands") == std::string::npos,
				"Shadow views must retain compact ordinals and selectors, never copied full command arrays.");
			const std::size_t viewBuildType = renderTypes.find(
				"struct RenderViewBuildState");
			const std::size_t viewBuildTypeEnd = renderTypes.find(
				"struct RenderOutputRequest", viewBuildType);
			context.Expect(viewBuildType != std::string::npos
				&& viewBuildTypeEnd != std::string::npos
				&& renderTypes.substr(viewBuildType,
					viewBuildTypeEnd - viewBuildType).find(
						"std::vector<RenderItemRecord>") == std::string::npos,
				"A prepared view must carry compact selectors and lanes, not a second full command collection.");
			context.Expect(renderSystem.find("selectedCommands") == std::string::npos
				&& publication.find("selectedCommands") == std::string::npos
				&& vulkan.find("selectedCommands") == std::string::npos,
				"Renderer publication must not restore a copied full per-view command payload.");
			context.Expect(renderSystem.find(
				"input.groupedInstanceRecords = frame.groupedInstanceRecords")
					== std::string::npos
				&& renderSystem.find(
					"input.groupedInstanceRecords = std::move(frame.groupedInstanceRecords)")
					!= std::string::npos,
				"Queueing a prepared view must transfer the grouped instance stream instead of copying the crowd-sized vector.");
			const std::size_t preparedSubmission = renderSystem.find(
				"struct PreparedRenderSubmission");
			const std::size_t makePresentation = renderSystem.find(
				"makePresentationInput", preparedSubmission);
			context.Expect(preparedSubmission != std::string::npos
				&& makePresentation != std::string::npos
				&& renderSystem.substr(preparedSubmission,
					makePresentation - preparedSubmission).find(
						"RenderViewBuildState frame") == std::string::npos,
				"A queued submission must retain only its immutable presentation, not a duplicate build-state owner.");
			const std::size_t visibleCacheHit = renderSystem.find(
				"RenderSystem: Reuse Visible Draw Commands");
			const std::size_t visibleDiagnostics = renderSystem.find(
				"RenderSystem: Reuse Visible Diagnostics", visibleCacheHit);
			const std::string visibleCacheHitBody = visibleCacheHit != std::string::npos
				&& visibleDiagnostics != std::string::npos
				? renderSystem.substr(visibleCacheHit,
					visibleDiagnostics - visibleCacheHit) : std::string{};
			context.Expect(!visibleCacheHitBody.empty()
				&& visibleCacheHitBody.find("std::vector<RenderItemRecord> cachedCommands;")
					!= std::string::npos
				&& visibleCacheHitBody.find("!existing->second.hasViewDrawEvidence")
					!= std::string::npos
				&& visibleCacheHitBody.find(
					"std::vector<RenderItemRecord> cachedCommands =") == std::string::npos,
				"A complete visible-cache hit must defer command materialization to the one mandatory publication pass.");
			const std::size_t queueSubmission = renderSystem.find(
				"auto queuePreparedSubmission");
			const std::size_t queueSubmissionEnd = renderSystem.find(
				"preparedRenderSubmissions.push_back", queueSubmission);
			const std::string queueSubmissionBody = queueSubmission != std::string::npos
				&& queueSubmissionEnd != std::string::npos
					? renderSystem.substr(queueSubmission,
						queueSubmissionEnd - queueSubmission) : std::string{};
			context.Expect(!queueSubmissionBody.empty()
				&& queueSubmissionBody.find("reuseCachedViewLanes") != std::string::npos
				&& queueSubmissionBody.find(
					"prepared.presentation.retainedCommandLanes = cachedRecord->commandLanes")
					!= std::string::npos
				&& queueSubmissionBody.find(
					"prepared.selection.retainedPayload = cachedRecord->retainedViewSelection")
					!= std::string::npos
				&& queueSubmissionBody.find(
					"prepared.presentation.retainedCommandSelectionIndices")
					!= std::string::npos
				&& queueSubmissionBody.find(
					"cachedRecord->stableOrdinals[index]") != std::string::npos,
				"A stable cached view must queue compact lanes and ordinals without rescanning full commands.");
			context.Expect(renderSystem.find("backendWorkPlan") == std::string::npos
				&& publication.find("backendWorkPlan") == std::string::npos
				&& vulkan.find("backendWorkPlan") == std::string::npos,
				"A sealed frame must carry only compact backend work, not a copied render-graph plan.");
			context.Expect(renderSystem.find(
					"snapshotRenderable.animationBinding = entry.animationBinding")
						!= std::string::npos
				&& renderSystem.find("if(!reuseRenderableRecords)")
					!= std::string::npos
				&& renderSystem.find(
					"renderFrameSnapshot.renderables[index].animationBinding")
						!= std::string::npos,
				"A rebuilt snapshot must retain complete bindings, while stable frames skip the entire record-copy loop and consume the immutable owner.");
			context.Expect(renderSystem.find("const bool applyDelta =")
					!= std::string::npos
				&& renderSystem.find("protectedSceneChanges->ComponentChanged()")
					!= std::string::npos
				&& renderSystem.find("renderableMembershipCache.erase(existing)")
					!= std::string::npos,
				"Validated scene-change packets must patch render membership instead of forcing a complete ECS scan for every edit.");
			context.Expect(renderSystem.find(
					"lane.transformGeneration =") != std::string::npos
				&& renderSystem.find("renderable.transformGeneration")
					!= std::string::npos
				&& renderSystem.find("sparseTransformRefresh")
					!= std::string::npos,
				"Dynamic pages must use object-local propagated transform generations and sparse matrix refreshes.");
			context.Expect(renderSystem.find(
					"taskIndex = 1u; taskIndex < workerCount") != std::string::npos
				&& renderSystem.find(
					"work(0U, std::min(itemCount, chunkSize))") != std::string::npos,
				"Chunked renderer work must use the current scheduler worker for one range rather than queueing every range back into the same pool.");
			const std::string suspendMarker = "co_await std::suspend_always{};";
			const std::size_t firstWorkerBoundary = renderSystem.find(suspendMarker);
			const std::size_t secondWorkerBoundary = renderSystem.find(
				suspendMarker, firstWorkerBoundary + suspendMarker.size());
			const std::size_t protectedExchangeBoundary = renderSystem.find(
				suspendMarker, secondWorkerBoundary + suspendMarker.size());
			const std::string workerPreparation = firstWorkerBoundary != std::string::npos
				&& protectedExchangeBoundary != std::string::npos
				? renderSystem.substr(firstWorkerBoundary,
					protectedExchangeBoundary - firstWorkerBoundary) : std::string{};
			context.Expect(!workerPreparation.empty()
				&& workerPreparation.find("objManager.") == std::string::npos
				&& workerPreparation.find("objectManager.") == std::string::npos
				&& workerPreparation.find("resourceManager.") == std::string::npos
				&& workerPreparation.find("ResourceManager::Instance()")
					== std::string::npos
				&& workerPreparation.find("rhi->") == std::string::npos
				&& workerPreparation.find("::Instance()") == std::string::npos,
				"Worker renderer phases must consume captured immutable roots without reopening live engine singletons.");
			context.Expect(renderSystem.find(
					"resourceManager.PublishVisibleStreamingTouches")
					> protectedExchangeBoundary,
				"Visible streaming mutations must publish only after the protected exchange boundary.");
			const std::size_t consumePublication = vulkan.find(
				"VulkanContext::ConsumeRenderFramePublication");
			const std::size_t consumeWork = vulkan.find(
				"VulkanContext::ConsumePublishedWorkPlans", consumePublication);
			context.Expect(consumePublication != std::string::npos
				&& consumeWork != std::string::npos
				&& vulkan.substr(consumePublication,
					consumeWork - consumePublication).find(
						"ValidateRenderFramePublication") == std::string::npos,
				"The backend boundary must not repeat crowd-sized publication validation.");
			context.Expect(renderSystem.find("DX12") == std::string::npos,
				"The current renderer refactor must not add DX12 behavior.");
			const std::size_t tryPublish = publicationImplementation.find(
				"RenderFramePublicationStore::TryPublish");
			const std::size_t resetForSession = publicationImplementation.find(
				"RenderFramePublicationStore::ResetForSession");
			context.Expect(tryPublish != std::string::npos && resetForSession != std::string::npos
				&& publicationImplementation.substr(tryPublish,
					resetForSession - tryPublish).find("ValidateRenderFramePublication")
					== std::string::npos,
				"The protected publication mutex must exchange only an already sealed root, never rescan crowd-sized validation data.");
		});
	}
}
