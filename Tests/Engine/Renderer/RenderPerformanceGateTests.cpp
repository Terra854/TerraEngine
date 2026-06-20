#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests {

	void RegisterRenderPerformanceGateTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PerformanceGate.FailsRegressionOverThreshold", [](TestContext& context) {
			const auto result = TerraEngine::Renderer::EvaluateRenderPerformanceGate("small", 10.0f, 11.0f, 5.0f);
			context.Expect(!result.passed, "Performance gates should fail when latest cost exceeds the allowed threshold.");
		});
	}

}
