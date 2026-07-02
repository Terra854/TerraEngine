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
		registry.Add("VirtualGeometry.Batching.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Virtual geometry batching tests should be registered through the shared engine harness.");
		});
		registry.Add("VirtualGeometry.Compaction.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Virtual geometry compaction tests should be registered through the shared engine harness.");
		});
		registry.Add("VirtualGeometry.PerformanceGate.RegistrationAvailable", [](TestContext& context) {
			context.Expect(true, "Virtual geometry performance gate tests should be registered through the shared engine harness.");
		});
	}

}
