#include "TestHarness.h"

#include "Runtime/ExportStartupCoordinator.h"

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Runtime::ExportStartupCoordinatorInput MakeValidExportInput() {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = true;
			input.projectName = "Startup Flow";
			input.version = "1.0.0";
			input.contentSource = "export-archive";
			input.startupScene = "Scenes/Startup.tescene";
			input.renderProfile = "profile=Baseline shadows=high ao=medium vrs=disabled textureBudgetMb=512 resolutionScale=1 validation=off debug=off";
			input.requiredSystems = {
				{ "Renderer", true, true, "Initialized" },
				{ "UI", true, true, "Initialized" },
				{ "Scripting", true, true, "Initialized" },
				{ "Scene", true, true, "Initialized" }
			};
			return input;
		}
	}

	void RegisterExportStartupFlowTests(TestRegistry& registry) {
		registry.Add("Engine.ExportStartup.ValidExportCanEnterRuntime", [](TestContext& context) {
			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(MakeValidExportInput());

			context.Expect(result.CanEnterRuntime(), "A complete export startup should be allowed to enter runtime.");
			context.Expect(result.summary.runtimeEntryAllowed, "Boot summary should mirror allowed runtime entry.");
			context.ExpectEqual(std::string("Startup Flow"), result.summary.projectName, "Boot summary should preserve project metadata.");
		});

		registry.Add("Engine.ExportStartup.MissingSceneBlocksRuntime", [](TestContext& context) {
			auto input = MakeValidExportInput();
			input.startupSceneLoaded = false;

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);

			context.Expect(!result.CanEnterRuntime(), "A configured but unloaded startup scene should block runtime entry.");
			context.ExpectEqual(std::string("One or more startup gates failed."), result.summary.runtimeEntryReason, "Failure reason should be clear for launcher and CI logs.");
		});
	}
}
