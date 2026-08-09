#include "TestHarness.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace TerraEngine::Tests
{
	void RegisterRenderDynamicPageTests(TestRegistry& registry)
	{
		registry.Add("Renderer.DynamicPages.RequireExactChangedLaneMask", [](TestContext& context) {
			Renderer::RenderDynamicPage page;
			page.generation = 1U;
			page.lanes.resize(2U);
			page.changedLanes = {true};
			Renderer::RenderFramePublication publication;
			publication.frameId = 1U;
			publication.generation = 1U;
			publication.rendererSessionGeneration = 1U;
			auto persistent = std::make_shared<Renderer::RenderPersistentTopologyRoot>();
			persistent->generation = 1U;
			persistent->rendererSessionGeneration = 1U;
			publication.persistent = persistent;
			auto dynamic = std::make_shared<Renderer::RenderDynamicStateRoot>();
			dynamic->generation = 1U;
			dynamic->rendererSessionGeneration = 1U;
			dynamic->pages.push_back(std::make_shared<const Renderer::RenderDynamicPage>(page));
			publication.dynamic = dynamic;
			const auto receipt = Renderer::ValidateRenderFramePublication(publication, 1U);
			context.Expect(!receipt.IsSuccess(), "A changed-lane mask must match its immutable page exactly.");
		});

		registry.Add("Renderer.DynamicPages.PreviousTransformSurvivesPageCopyOnWrite",
			[](TestContext& context) {
			using namespace Renderer;
			auto originalPage = std::make_shared<RenderDynamicPage>();
			originalPage->generation = 1u;
			originalPage->lanes.resize(1u);
			originalPage->lanes.front().key = { 0u, 1u };
			originalPage->lanes.front().occupied = true;
			originalPage->lanes.front().instance.modelMatrix[3][0] = 2.0f;
			originalPage->lanes.front().previousModelMatrix[3][0] = 1.0f;
			originalPage->changedLanes = { false };

			auto replacementPage = std::make_shared<RenderDynamicPage>(*originalPage);
			replacementPage->generation = 2u;
			replacementPage->lanes.front().previousModelMatrix =
				originalPage->lanes.front().instance.modelMatrix;
			replacementPage->lanes.front().instance.modelMatrix[3][0] = 3.0f;
			replacementPage->changedLanes.front() = true;
			context.Expect(originalPage.get() != replacementPage.get()
				&& originalPage->lanes.front().instance.modelMatrix[3][0] == 2.0f
				&& originalPage->lanes.front().previousModelMatrix[3][0] == 1.0f
				&& replacementPage->lanes.front().previousModelMatrix[3][0] == 2.0f
				&& replacementPage->lanes.front().instance.modelMatrix[3][0] == 3.0f
				&& replacementPage->changedLanes.front(),
				"A transform update must retain the prior transform in its copied dynamic page without altering readers of the old page.");
			});

		registry.Add("Renderer.DynamicPages.RecyclePoolWaitsForSealedReaders",
			[](TestContext& context) {
				using namespace Renderer;
				RenderDynamicPageRecyclePool pool;
				auto first = pool.Acquire(2u);
				RenderDynamicPage* const firstAddress = first.get();
				first->lanes.resize(1u);
				std::shared_ptr<const RenderDynamicPage> sealedReader = first;
				first.reset();

				auto whileSealed = pool.Acquire(2u);
				const bool didNotReuseLivePage = whileSealed.get() != firstAddress;
				whileSealed.reset();
				sealedReader.reset();

				auto recycled = pool.Acquire(2u);
				context.Expect(didNotReuseLivePage
					&& recycled.get() == firstAddress
					&& recycled->lanes.capacity() >= 1u,
					"A recycled dynamic page must wait for every sealed reader, then retain its lane allocation for a later immutable publication.");
			});
	}
}
