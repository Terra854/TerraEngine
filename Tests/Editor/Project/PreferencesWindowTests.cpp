#include "TestHarness.h"
#include "ProjectSettingsTestHelpers.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferences.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferencesMetadata.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadPreferencesSourceFile(const char* relativePath)
		{
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream file(sourcePath, std::ios::binary);
			if (!file)
			{
				return {};
			}

			std::ostringstream buffer;
			buffer << file.rdbuf();
			return buffer.str();
		}
	}

	void RegisterPreferencesWindowTests(TestRegistry& registry)
	{
		registry.Add("Editor.Preferences.EditMenuContract", [](TestContext& context) {
			context.ExpectEqual(
				std::string("Edit"),
				std::string(TerraEngine::Editor::Preferences::kPreferencesMenuName),
				"Preferences should belong to the Edit menu.");
			context.ExpectEqual(
				std::string("Preferences..."),
				std::string(TerraEngine::Editor::Preferences::kPreferencesMenuLabel),
				"Preferences should use the expected menu label.");
			context.ExpectEqual(
				std::string("Preferences"),
				std::string(TerraEngine::Editor::Preferences::kPreferencesWindowTitle),
				"Preferences should open the expected window title.");
		});

		registry.Add("Editor.Preferences.CategorySearchContract", [](TestContext& context) {
			const auto allCategories = TerraEngine::Editor::Preferences::FilterPreferencesCategories("");
			context.ExpectEqual(static_cast<std::size_t>(1), allCategories.size(), "Empty search should show the current Preferences categories.");

			const auto validationMatches = TerraEngine::Editor::Preferences::FilterPreferencesCategories("validation");
			context.ExpectEqual(static_cast<std::size_t>(1), validationMatches.size(), "Validation search should find the diagnostics category.");
			context.ExpectEqual(
				std::string("diagnostics"),
				std::string(validationMatches.front().stableId),
				"Validation search should route to diagnostics.");
		});

		registry.Add("Editor.Preferences.RendererDiagnosticsMovedFromProjectSettings", [](TestContext& context) {
			const std::string preferencesSource =
				ReadPreferencesSourceFile("Editor/TerraEngine-Editor/Windows/PreferencesWindow.cpp");
			const std::string projectSettingsSource =
				ReadPreferencesSourceFile("Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp");
			const std::string mainEditorSource =
				ReadPreferencesSourceFile("Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			context.Expect(!preferencesSource.empty(), "Preferences source should be readable for diagnostics ownership checks.");
			context.Expect(!projectSettingsSource.empty(), "Project Settings source should be readable for diagnostics ownership checks.");
			context.Expect(!mainEditorSource.empty(), "Main editor source should be readable for menu ownership checks.");

			context.Expect(preferencesSource.find("DrawRendererDiagnosticsCategory") != std::string::npos, "Preferences should own the renderer diagnostics category UI.");
			context.Expect(preferencesSource.find("Vulkan Validation Layers") != std::string::npos, "Preferences should expose the Vulkan validation toggle.");
			context.Expect(preferencesSource.find("Render Debug Diagnostics") != std::string::npos, "Preferences should expose the render debug toggle.");
			context.Expect(preferencesSource.find("SetRendererDiagnosticPreferences") != std::string::npos, "Preferences should persist renderer diagnostics through editor preferences.");
			context.Expect(preferencesSource.find("ApplyTransientRenderDiagnostics") != std::string::npos, "Preferences should apply diagnostics without saving project render settings.");
			context.Expect(preferencesSource.find("SetRenderSettings(settings)") == std::string::npos, "Preferences should not route renderer diagnostics through the project RenderSettings save path.");
			context.Expect(preferencesSource.find("SaveSettings") == std::string::npos, "Preferences should not save RenderSettings.json when local diagnostics change.");
			context.Expect(mainEditorSource.find("kPreferencesMenuLabel") != std::string::npos, "Main editor Edit menu should expose Preferences.");
			context.Expect(mainEditorSource.find("ApplyStoredPreferencesToRenderSystem") != std::string::npos, "Project open should apply stored editor diagnostic preferences.");

			context.Expect(projectSettingsSource.find("Renderer Diagnostics") == std::string::npos, "Project Settings should no longer own renderer diagnostics.");
			context.Expect(projectSettingsSource.find("Vulkan Validation Layers") == std::string::npos, "Project Settings should not expose the validation toggle after the move.");
			context.Expect(projectSettingsSource.find("Render Debug Diagnostics") == std::string::npos, "Project Settings should not expose the render debug toggle after the move.");
		});

		registry.Add("Editor.Preferences.RendererDiagnosticsRoundTrip", [](TestContext& context) {
			EditorPreferences preferences;
			preferences.vulkanValidationLayers = true;
			preferences.renderDebugDiagnostics = true;
			preferences.rendererDiagnosticsPreferencesInitialized = true;

			nlohmann::json json = preferences;
			const EditorPreferences restored = json.get<EditorPreferences>();
			context.Expect(restored.vulkanValidationLayers, "Vulkan validation preference should round-trip.");
			context.Expect(restored.renderDebugDiagnostics, "Render debug diagnostics preference should round-trip.");
			context.Expect(restored.rendererDiagnosticsPreferencesInitialized, "Renderer diagnostics preferences should remain initialized after round-trip.");

			const EditorPreferences legacy = nlohmann::json::object().get<EditorPreferences>();
			context.Expect(!legacy.rendererDiagnosticsPreferencesInitialized, "Legacy preference files should let Project Settings render flags seed the first editor preference value.");
		});
	}
}
