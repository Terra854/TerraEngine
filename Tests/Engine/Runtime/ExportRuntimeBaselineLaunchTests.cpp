#include "TestHarness.h"

#include "Runtime/ExportStartupCoordinator.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		std::filesystem::path SampleRoot() {
			return (std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "Fixtures" / "ExportRuntimeBaseline").lexically_normal();
		}

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}
	}

	void RegisterExportRuntimeBaselineLaunchTests(TestRegistry& registry) {
		registry.Add("Engine.ExportRuntimeBaseline.LaunchSignalsArePresent", [](TestContext& context) {
			const std::filesystem::path root = SampleRoot();
			const std::string ui = ReadText(root / "UI" / "Baseline.ui");
			const std::string script = ReadText(root / "Scripts" / "BaselineScript.cs");

			context.Expect(ui.find("Export Runtime Baseline Ready") != std::string::npos, "Baseline UI should contain the manual launch signal.");
			context.Expect(script.find("ExportRuntimeBaseline.ScriptStarted") != std::string::npos, "Baseline script should contain the runtime signal.");
		});

		registry.Add("Engine.ExportRuntimeBaseline.CoordinatorAcceptsSampleFacts", [](TestContext& context) {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = std::filesystem::exists(SampleRoot() / "Scenes" / "Startup.tescene");
			input.projectName = "Export Runtime Baseline";
			input.version = "1.0.0";
			input.contentSource = "export-archive";
			input.startupScene = "Scenes/Startup.tescene";
			input.renderProfile = "profile=ExportRuntimeBaseline shadows=medium ao=medium vrs=disabled textureBudgetMb=256 resolutionScale=1 validation=on debug=on";
			input.requiredSystems = {
				{ "Renderer", true, true, "Initialized" },
				{ "UI", true, true, "Initialized" },
				{ "Scripting", true, true, "Initialized" },
				{ "Scene", true, true, "Initialized" }
			};

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);

			context.Expect(result.CanEnterRuntime(), "Sample startup facts should pass coordinator validation.");
			context.ExpectEqual(std::string("Export Runtime Baseline"), result.summary.projectName, "Sample boot summary should preserve project name.");
		});
	}
}
