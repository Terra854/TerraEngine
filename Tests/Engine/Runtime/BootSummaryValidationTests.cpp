#include "TestHarness.h"

#include "Runtime/BootSummary.h"
#include "Runtime/ExportStartupCoordinator.h"

namespace TerraEngine::Tests {
	void RegisterBootSummaryValidationTests(TestRegistry& registry) {
		registry.Add("Engine.BootSummary.ValidationListsFailedRequiredSystems", [](TestContext& context) {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = true;
			input.requiredSystems = {
				{ "Renderer", true, false, "Render device unavailable" },
				{ "UI", true, true, "Initialized" }
			};

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);
			const std::string formatted = TerraEngine::Runtime::FormatBootSummary(result.summary);

			context.Expect(!result.CanEnterRuntime(), "Failed required systems should block runtime entry.");
			context.Expect(formatted.find("system[Renderer]=fail") != std::string::npos, "Boot summary should name the failed system.");
			context.Expect(formatted.find("Render device unavailable") != std::string::npos, "Boot summary should include the system failure detail.");
		});

		registry.Add("Engine.BootSummary.OptionalSystemsWarnWithoutBlocking", [](TestContext& context) {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = true;
			input.requiredSystems = {
				{ "DebugOverlay", false, false, "Disabled" }
			};

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);

			context.Expect(result.CanEnterRuntime(), "Optional warning systems should not block runtime entry.");
			context.Expect(result.summary.requiredSystems.back().status == TerraEngine::Runtime::BootStepStatus::Warning, "Optional missing systems should report warnings.");
		});
	}
}
