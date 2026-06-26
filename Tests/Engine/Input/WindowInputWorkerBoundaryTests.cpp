#include "../TestHarness.h"

#include "Input/InputSystem.h"
#include "Window/WindowSystem.h"

namespace TerraEngine::Tests {
	void RegisterWindowInputWorkerBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.WindowInputWorkerBoundary.PollingIsProtectedAndSnapshotsAreWorkerReadable", [](TestContext& context) {
			const std::string summary = InputSystem::Instance().GetPollingBoundarySummary();
			const WindowSystem::WindowPollingBoundaryDiagnostics diagnostics = WindowSystem::Instance().GetPollingBoundaryDiagnostics();

			context.Expect(summary.find("protected") != std::string::npos, "Input summary should state that polling is protected.");
			context.Expect(summary.find("snapshots") != std::string::npos, "Input summary should state that workers read snapshots.");
			context.Expect(diagnostics.classification.empty() || diagnostics.classification == "protected", "Window polling diagnostics should stay protected when available.");
		});
	}
}
