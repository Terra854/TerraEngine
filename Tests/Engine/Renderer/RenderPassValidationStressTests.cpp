#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"

namespace TerraEngine::Tests {

	void RegisterRenderPassValidationStressTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PassValidationStress.CatchesWriteConflicts", [](TestContext& context) {
			auto first = MakeRendererPass("first", 55u, TerraEngine::Renderer::RenderAccessMode::Write);
			auto second = MakeRendererPass("second", 55u, TerraEngine::Renderer::RenderAccessMode::Write);
			const auto result = TerraEngine::Renderer::ValidateRenderGraph({ first, second });
			context.Expect(!result.valid, "Injected write/write conflicts should be rejected.");
		});
	}

}
