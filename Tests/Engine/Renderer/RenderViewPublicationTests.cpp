#include "TestHarness.h"
#include "Renderer/Graph/RenderFramePublication.h"
#include "RendererThreadingTestFixtures.h"

#include <array>
#include <memory>

namespace TerraEngine::Tests
{
	void RegisterRenderViewPublicationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ViewPublication.RequiresParallelSelectors", [](TestContext& context) {
			Renderer::RenderViewSelection view;
			view.generation = 7U;
			view.visibleOrdinals.push_back({1U, 2U});
			view.selectedLods.push_back(0U);
			context.Expect(!view.HasParallelLengths(),
				"Every view selector array must describe the same stable ordinals.");
		});

		registry.Add("Renderer.ViewPublication.CanonicalOutputsStayGenerationCoherent",
			[](TestContext& context) {
				using namespace Renderer;
				constexpr std::uint64_t generation = 42u;
				// Build one immutable slot/lane pair. Every camera output below points
				// at this exact key, so a mismatched root generation cannot hide behind
				// a stable ordinal during publication validation.
				auto page = std::make_shared<RenderPersistentPage>();
				page->generation = generation;
				RenderPersistentSlot slot{};
				slot.key = { 0u, 7u };
				slot.objectId = GenerateUUID();
				slot.meshId = GenerateUUID();
				slot.materialId = GenerateUUID();
				slot.meshGeneration = generation;
				slot.materialGeneration = generation;
				slot.resourceGeneration = generation;
				slot.packetTemplateIdentity = 99u;
				slot.occupied = true;
				page->slots.push_back(slot);
				auto dynamicPage = std::make_shared<RenderDynamicPage>();
				dynamicPage->generation = generation;
				dynamicPage->changedLanes.push_back(false);
				RenderDynamicLane lane{};
				lane.key = slot.key;
				lane.occupied = true;
				dynamicPage->lanes.push_back(lane);
				auto publication = std::make_shared<RenderFramePublication>();
				publication->frameId = 900u;
				publication->rendererSessionGeneration = 3u;
				publication->generation = generation;
				publication->sceneGeneration = generation;
				publication->persistent = std::make_shared<RenderPersistentTopologyRoot>(
					RenderPersistentTopologyRoot{ generation, 3u, { page } });
				publication->dynamic = std::make_shared<RenderDynamicStateRoot>(
					RenderDynamicStateRoot{ generation, 3u, { dynamicPage } });
				auto backend = std::make_shared<RenderBackendWorkPublication>();
				backend->frameId = publication->frameId;
				backend->animationWork.frameId = publication->frameId;
				backend->animationWork.valid = true;
				backend->valid = true;
				publication->backendWork = std::move(backend);

				struct OutputCase { RenderOutputKind kind; RenderViewType view; };
				const std::array<OutputCase, 7> outputs{{
					{ RenderOutputKind::SceneView, RenderViewType::Scene },
					{ RenderOutputKind::GameView, RenderViewType::Game },
					{ RenderOutputKind::MainRuntimeView, RenderViewType::ExportRuntime },
					{ RenderOutputKind::OffscreenTexture, RenderViewType::Game },
					{ RenderOutputKind::ValidationOnly, RenderViewType::Game },
					{ RenderOutputKind::ValidationOnly, RenderViewType::Shadow },
					{ RenderOutputKind::ValidationOnly, RenderViewType::Scene }
				}};
				for(std::size_t index = 0u; index < outputs.size(); ++index) {
					RenderViewSelection selection{};
					selection.outputKind = outputs[index].kind;
					selection.viewType = outputs[index].view;
					selection.outputId = static_cast<std::uint32_t>(100u + index);
					selection.generation = generation;
					selection.lightIdentity = selection.viewType == RenderViewType::Shadow
						? 1234u : 0u;
					selection.visibleOrdinals.push_back(slot.key);
					selection.selectedLods.push_back(0u);
					selection.routeSelectors.push_back(0u);
					selection.packetTemplateIdentities.push_back(slot.packetTemplateIdentity);
					selection.tintSelectors.push_back(0u);
					selection.editorTints.push_back(glm::vec4(0.0f));
					publication->views.push_back(selection);
					RenderViewPresentationInput input{};
					input.output.kind = selection.outputKind;
					input.output.diagnosticsView = selection.viewType;
					input.output.outputId = selection.outputId;
					const RenderReferenceFrame referenceFrame =
						MakeRendererTestReferenceFrame(publication->frameId, index + 1u, selection.outputId);
					input.referenceFrame = referenceFrame;
					publication->referenceFrames.push_back(referenceFrame);
					publication->presentationInputs.push_back(std::move(input));
					publication->submissionRanges.push_back({ RenderSubmissionRoute::Direct,
						static_cast<std::uint32_t>(index), 0u, 1u,
						slot.packetTemplateIdentity, true });
				}
				const RenderPublicationReceipt receipt = ValidateRenderFramePublication(
					*publication, 3u);
				context.Expect(receipt.IsSuccess() && receipt.viewCount == outputs.size()
					&& receipt.rangeCount == outputs.size(),
					"Scene, Game, runtime, capture, picking, shadow, and visualizer outputs must validate as one generation-coherent publication.");
				for(std::size_t index = 0u; index < outputs.size(); ++index) {
					context.Expect(publication->views[index].generation == generation
						&& publication->views[index].visibleOrdinals.front().lifetimeGeneration == slot.key.lifetimeGeneration
						&& publication->presentationInputs[index].Matches(publication->views[index]),
						"Each canonical output must retain the exact persistent slot generation and matching output identity.");
				}
			});
	}
}
