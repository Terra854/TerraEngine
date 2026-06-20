#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameBreakdown.h"

namespace TerraEngine::Tests {

	void RegisterRenderViewSubmitReuseTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ViewSubmitReuse.ReusesCompatibleWork", [](TestContext& context) {
			const auto scene = TerraEngine::Renderer::BuildViewSubmitWorkItem(11u, 1u, RenderViewType::Scene, 123u, 456u, 0.4f, 0.2f, "camera:main");
			const auto game = TerraEngine::Renderer::BuildViewSubmitWorkItem(11u, 2u, RenderViewType::Game, 123u, 456u, 0.3f, 0.2f, "camera:main");
			std::string reason;

			const TerraEngine::Renderer::ViewSubmitReuseDecision decision = TerraEngine::Renderer::CompareViewSubmitWork(scene, game, &reason);
			context.Expect(decision == TerraEngine::Renderer::ViewSubmitReuseDecision::Reused, "Matching backend work keys and pass state should be reusable.");
			context.Expect(reason.find("compatible") != std::string::npos, "Reuse reasons should explain why sharing is safe.");
		});

		registry.Add("Renderer.ViewSubmitReuse.RejectsUnsafeOverlayChanges", [](TestContext& context) {
			const auto scene = TerraEngine::Renderer::BuildViewSubmitWorkItem(12u, 1u, RenderViewType::Scene, 123u, 456u, 0.4f, 0.2f, "camera:main");
			const auto game = TerraEngine::Renderer::BuildViewSubmitWorkItem(12u, 2u, RenderViewType::Game, 123u, 999u, 0.3f, 0.2f, "camera:main");

			context.Expect(
				TerraEngine::Renderer::CompareViewSubmitWork(scene, game, nullptr) == TerraEngine::Renderer::ViewSubmitReuseDecision::Unsafe,
				"Overlay changes should prevent scene/game submit work from being treated as identical.");
		});

		registry.Add("Renderer.ViewSubmitReuse.RequiresSharedWorkKeys", [](TestContext& context) {
			const auto scene = TerraEngine::Renderer::BuildViewSubmitWorkItem(13u, 1u, RenderViewType::Scene, 123u, 456u, 0.4f, 0.2f);
			const auto game = TerraEngine::Renderer::BuildViewSubmitWorkItem(13u, 2u, RenderViewType::Game, 123u, 456u, 0.3f, 0.2f, "camera:main");

			context.Expect(
				TerraEngine::Renderer::CompareViewSubmitWork(scene, game, nullptr) == TerraEngine::Renderer::ViewSubmitReuseDecision::Unavailable,
				"Missing shared work keys should produce an unavailable decision instead of guessing.");
		});
	}

}
