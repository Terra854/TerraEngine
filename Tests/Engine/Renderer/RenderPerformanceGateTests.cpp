#include "TestHarness.h"
#include "Renderer/Graph/RenderDiagnostics.h"

#include <string>

namespace TerraEngine::Tests {

	void RegisterRenderPerformanceGateTests(TestRegistry& registry)
	{
		registry.Add("Renderer.PerformanceGate.FailsRegressionOverThreshold", [](TestContext& context) {
			const auto result = TerraEngine::Renderer::EvaluateRenderPerformanceGate("small", 10.0f, 11.0f, 5.0f);
			context.Expect(!result.passed, "Performance gates should fail when latest cost exceeds the allowed threshold.");
		});

		registry.Add("Renderer.PerformanceGate.VirtualGeometryThresholdHelpersReportExpectedActual", [](TestContext& context) {
			const PerformanceGateResult gate = TerraEngine::Renderer::EvaluateVirtualGeometryPerformanceGate(
				"dense-cluster-batch-ratio",
				"virtual-geometry-dense-batching",
				"submittedBatchRatio",
				0.1f,
				0.2f,
				"ratio",
				true);

			context.Expect(gate.status == VirtualGeometryPerformanceGateStatus::Fail, "Virtual geometry gates should fail when lower-is-better metrics exceed the threshold.");
			context.ExpectEqual(std::string("virtual-geometry-dense-batching"), gate.fixtureId, "Virtual geometry gates should identify the fixture.");
			context.Expect(gate.diagnostic.find("actual") != std::string::npos, "Virtual geometry gate diagnostics should include the actual value.");
			context.Expect(gate.diagnostic.find("max") != std::string::npos, "Virtual geometry gate diagnostics should explain the threshold direction.");
		});
	}

}
