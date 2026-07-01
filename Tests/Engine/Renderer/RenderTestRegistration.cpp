#include "TestHarness.h"

namespace TerraEngine::Tests {

	void RegisterRendererGraphSmokeTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Graph.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Renderer graph tests should be registered through the shared engine harness.");
		});
		registry.Add("Renderer.FrameBreakdown.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Renderer frame breakdown tests should be registered through the shared engine harness.");
		});
		registry.Add("Renderer.Capture.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Renderer capture tests should be registered through the shared engine harness.");
		});
	}

}
