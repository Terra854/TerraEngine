#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/EditorStartupRendererConfiguration.h"

#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	void RegisterEditorStartupRendererConfigurationTests(TestRegistry& registry)
	{
		registry.Add("Editor.StartupRenderer.DirectProjectIsReadOnlyAndSanitized", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / "TerraEngineStartupRendererConfiguration";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "ProjectSettings");
			const auto settingsPath = root / "ProjectSettings" / "RenderSettings.json";
			{
				std::ofstream output(settingsPath);
				output << R"({"validationEnabled":true,"settings":{"shadowResolution":999999,"variableRateShading":1}})";
			}
			const auto before = std::filesystem::last_write_time(settingsPath);
			const auto configuration = TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(root);
			context.Expect(configuration.origin == TerraEngine::Editor::StartupRendererOrigin::DirectProject, "An explicit project must not become a hub launch.");
			context.ExpectEqual(16384u, configuration.renderSettings.shadowResolution, "Unsafe settings should be clamped before Vulkan startup.");
			context.ExpectEqual(before, std::filesystem::last_write_time(settingsPath), "Bootstrap must not write project files.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.StartupRenderer.HubDoesNotDiscoverWorkingDirectory", [](TestContext& context) {
			const auto configuration = TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(std::nullopt);
			context.Expect(configuration.origin == TerraEngine::Editor::StartupRendererOrigin::HubDefaults, "Missing --project must select explicit hub defaults.");
			context.Expect(!configuration.projectRoot.has_value(), "Hub defaults must not infer a cwd project root.");
		});
	}
}
