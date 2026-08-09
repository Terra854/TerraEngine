#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/Graph/RenderFramePublication.h"

#include <memory>

namespace TerraEngine::Tests
{
	namespace
	{
		std::shared_ptr<TerraEngine::Renderer::RenderFramePublication>
			MakeLifecyclePublication(std::uint64_t session, std::uint64_t generation)
		{
			using namespace TerraEngine::Renderer;
			auto page = std::make_shared<RenderPersistentPage>();
			page->generation = generation;
			RenderPersistentSlot slot{};
			slot.key = { 0u, generation };
			slot.objectId = GenerateUUID(); slot.meshId = GenerateUUID();
			slot.materialId = GenerateUUID(); slot.meshGeneration = generation;
			slot.materialGeneration = generation; slot.resourceGeneration = generation;
			slot.packetTemplateIdentity = generation; slot.occupied = true;
			page->slots.push_back(slot);
			auto dynamicPage = std::make_shared<RenderDynamicPage>();
			dynamicPage->generation = generation; dynamicPage->changedLanes.push_back(false);
			RenderDynamicLane lane{}; lane.key = slot.key; lane.occupied = true;
			dynamicPage->lanes.push_back(lane);
			auto publication = std::make_shared<RenderFramePublication>();
			publication->frameId = generation; publication->generation = generation;
			publication->rendererSessionGeneration = session;
			publication->sceneGeneration = generation;
			publication->persistent = std::make_shared<RenderPersistentTopologyRoot>(
				RenderPersistentTopologyRoot{ generation, session, { page } });
			publication->dynamic = std::make_shared<RenderDynamicStateRoot>(
				RenderDynamicStateRoot{ generation, session, { dynamicPage } });
			auto backend = std::make_shared<RenderBackendWorkPublication>();
			backend->frameId = generation;
			backend->animationWork.frameId = generation;
			backend->animationWork.valid = true;
			backend->valid = true;
			publication->backendWork = std::move(backend);
			RenderViewSelection view{}; view.outputId = 1u; view.generation = generation;
			view.visibleOrdinals.push_back(slot.key); view.selectedLods.push_back(0u);
			view.routeSelectors.push_back(0u); view.packetTemplateIdentities.push_back(generation);
			view.tintSelectors.push_back(0u); view.editorTints.push_back(glm::vec4(0.0f));
			publication->views.push_back(view);
			RenderViewPresentationInput input{}; input.output.outputId = 1u;
			input.output.kind = view.outputKind; input.output.diagnosticsView = view.viewType;
			publication->presentationInputs.push_back(std::move(input));
			publication->submissionRanges.push_back({ RenderSubmissionRoute::Direct, 0u, 0u, 1u, generation, true });
			return publication;
		}
	}

	void RegisterRendererViewLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ViewLifecycle.HandlesEmptyAndZeroSizedViews", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderFrameSnapshot empty;
			TerraEngine::Renderer::SortSnapshotDeterministically(empty);
			context.Expect(empty.viewRequests.empty(), "A frame with no active view must remain an explicit empty snapshot.");
			TerraEngine::Renderer::RenderFrameSnapshot zero;
			zero.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(0u), 1u });
			TerraEngine::Renderer::SortSnapshotDeterministically(zero);
			context.ExpectEqual(0u, zero.viewRequests.front().targetSize.x, "A zero-sized target must not inherit a prior view extent.");
		});

		registry.Add("Renderer.ViewLifecycle.MultipleViewsRemainDeterministicAcrossRecovery", [](TestContext& context)
		{
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 9u;
			snapshot.sceneRevision = 3u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, glm::uvec2(1920u, 1080u), 2u });
			snapshot.viewRequests.push_back({ RenderOutputKind::SceneView, RenderViewType::Scene, glm::uvec2(1280u, 720u), 1u });
			TerraEngine::Renderer::SortSnapshotDeterministically(snapshot);
			context.ExpectEqual(static_cast<std::size_t>(2), snapshot.viewRequests.size(), "Scene and Game views must remain separate during recovery and replay.");
			context.Expect(snapshot.viewRequests[0].outputId != snapshot.viewRequests[1].outputId, "Each view must retain an independent output identity.");
		});

		registry.Add("Renderer.ViewLifecycle.RejectsStaleSessionAndAllocationRollback", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const auto candidate = MakeLifecyclePublication(7u, 4u);
			context.Expect(!SealRenderFramePublication(candidate, 8u).IsValid(),
				"A publication from an old renderer session must never become visible after recovery.");
			context.Expect(!SealRenderFramePublication(candidate, 7u, { true, false, false }).IsValid(),
				"Allocation failure injection must reject atomically instead of exposing a partial root.");
		});

		registry.Add("Renderer.ViewLifecycle.CaptureReadbackRetainsFrameGeneration", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			const auto candidate = MakeLifecyclePublication(3u, 11u);
			const SealedRenderFramePublication sealed = SealRenderFramePublication(candidate, 3u);
			context.Expect(sealed.IsValid() && sealed.Publication()->frameId == 11u
				&& sealed.Publication()->generation == 11u
				&& sealed.Publication()->backendWork->frameId == 11u,
				"Capture/readback consumers must retain the exact frame and publication generation ownership token.");
		});

		registry.Add("Renderer.ViewLifecycle.ViewIdentityRemovalAndReuseRequiresNewGeneration", [](TestContext& context)
		{
			using namespace TerraEngine::Renderer;
			RenderFrameSnapshot snapshot;
			snapshot.frameId = 20u;
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, { 640u, 480u }, 4u });
			SortSnapshotDeterministically(snapshot);
			const std::uint32_t retiredId = snapshot.viewRequests.front().outputId;
			snapshot.viewRequests.clear();
			snapshot.viewRequests.push_back({ RenderOutputKind::GameView, RenderViewType::Game, { 640u, 480u }, retiredId, 21u });
			SortSnapshotDeterministically(snapshot);
			context.Expect(snapshot.viewRequests.front().outputId == retiredId
				&& snapshot.viewRequests.front().viewGeneration == 21u,
				"Reusing a view identity after removal must carry a new generation token rather than stale capture state.");
		});
	}
}
