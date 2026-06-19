#include "TestHarness.h"

#include "Runtime/ExportStartupCoordinator.h"

#include <string>

namespace TerraEngine::Tests {
	void RegisterLinuxStartupSmokeTests(TestRegistry& registry) {
		registry.Add("Engine.LinuxStartupSmoke.ActionableFailureForMissingScene", [](TestContext& context) {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = false;
			input.projectName = "Linux Startup Smoke";
			input.version = "1.0.0";
			input.contentSource = "export-archive";
			input.startupScene = "Scenes/Missing.tescene";
			input.requiredSystems = {
				{ "ResourceManager", true, true, "Initialized" },
				{ "Scene", true, false, "Startup scene unreadable" },
				{ "Renderer", true, true, "Deferred during headless smoke" }
			};

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);

			context.Expect(!result.CanEnterRuntime(), "Missing startup scene should block runtime entry.");
			context.ExpectEqual(std::string("One or more startup gates failed."), result.summary.runtimeEntryReason, "Startup smoke failure should be actionable in logs.");
		});
	}
}
