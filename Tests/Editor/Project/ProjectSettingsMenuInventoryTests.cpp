#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadWindowMenuSource(const char* relativePath)
		{
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream file(sourcePath, std::ios::binary);
			if (!file)
			{
				return {};
			}

			std::ostringstream buffer;
			buffer << file.rdbuf();
			return buffer.str();
		}

		struct NativeWindowMenuContract
		{
			const char* sourcePath;
			const char* title;
			const char* categoryPath;
		};

		constexpr std::array<NativeWindowMenuContract, 9> kNativeWindowMenuContracts = {{
			{"Editor/TerraEngine-Editor/Windows/HierarchyWindow.cpp", "Scene Hierarchy", "General"},
			{"Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp", "Inspector", "General"},
			{"Editor/TerraEngine-Editor/Windows/ProjectBrowserWindow.cpp", "Project Browser", "General"},
			{"Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp", "Game View", "Rendering"},
			{"Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp", "Scene View", "Rendering"},
			{"Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp", "Profiler", "Rendering"},
			{"Editor/TerraEngine-Editor/Windows/ResourceResidencyWindow.cpp", "Resource Residency", "Rendering"},
			{"Editor/TerraEngine-Editor/Windows/SchedulerDiagnosticsWindow.cpp", "Scheduler Diagnostics", "Diagnostics"},
			{"Editor/TerraEngine-Editor/Windows/UIBuilderWindow.cpp", "UI Builder", "Tools"}
		}};
	}

	void RegisterProjectSettingsMenuInventoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.WindowMenu.UsesExplicitPerWindowPaths", [](TestContext& context) {
			const std::string editorWindowHeader = ReadWindowMenuSource("Editor/TerraEngine-Editor/Framework/EditorWindow.h");
			const std::string mainEditorSource = ReadWindowMenuSource("Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			context.Expect(!editorWindowHeader.empty(), "EditorWindow header should be readable for menu ownership checks.");
			context.Expect(!mainEditorSource.empty(), "Main editor source should be readable for menu ownership checks.");
			context.Expect(editorWindowHeader.find("GetWindowMenuCategoryPath") != std::string::npos, "Native windows should expose their optional menu category path.");
			context.Expect(mainEditorSource.find("SplitWindowMenuPath") != std::string::npos, "The Windows menu should support arbitrary slash-separated subcategories.");
			context.Expect(mainEditorSource.find("BuildWindowMenuTree") != std::string::npos, "The Windows menu should build its hierarchy from registered path declarations.");
			context.Expect(mainEditorSource.find("ShouldShowWindowInWindowMenu") == std::string::npos, "The Windows menu should not use a centralized title blacklist.");
		});

		registry.Add("Editor.WindowMenu.NativeCategoriesAreWindowOwned", [](TestContext& context) {
			for (const NativeWindowMenuContract& contract : kNativeWindowMenuContracts)
			{
				const std::string source = ReadWindowMenuSource(contract.sourcePath);
				context.Expect(!source.empty(), std::string("Window source should be readable: ") + contract.sourcePath);
				context.Expect(source.find(std::string("EditorWindow(\"") + contract.title + "\"") != std::string::npos, std::string("Window constructor should retain its title: ") + contract.title);
				context.Expect(
					source.find(std::string("\"") + contract.categoryPath + "\"") != std::string::npos,
					std::string("Window should declare its own Windows menu category: ") + contract.categoryPath);
			}
		});

		registry.Add("Editor.WindowMenu.DedicatedCommandsDoNotOptIn", [](TestContext& context) {
			const std::string buildProfiles = ReadWindowMenuSource("Editor/TerraEngine-Editor/Windows/BuildProfilesWindow.cpp");
			const std::string projectSettings = ReadWindowMenuSource("Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp");
			const std::string preferences = ReadWindowMenuSource("Editor/TerraEngine-Editor/Windows/PreferencesWindow.cpp");
			context.Expect(buildProfiles.find("EditorWindow(\"Build Profiles\", false,") == std::string::npos, "Build Profiles should rely on its File menu command instead of a Windows menu path.");
			context.Expect(projectSettings.find("kProjectSettingsWindowTitle), false,") == std::string::npos, "Project Settings should rely on its Edit menu command instead of a Windows menu path.");
			context.Expect(preferences.find("kPreferencesWindowTitle), false,") == std::string::npos, "Preferences should rely on its Edit menu command instead of a Windows menu path.");
		});

		registry.Add("Editor.WindowMenu.ManagedWindowsUseTheSameOptInModel", [](TestContext& context) {
			const std::string managedWindow = ReadWindowMenuSource("Editor/Managed/TerraEngine.Editor/Source/Framework/EditorWindow.cs");
			const std::string managedApplication = ReadWindowMenuSource("Editor/Managed/TerraEngine.Editor/Source/EditorApplication.cs");
			const std::string statusWindow = ReadWindowMenuSource("Editor/Managed/TerraEngine.Editor/Source/StatusWindow.cs");
			context.Expect(managedWindow.find("WindowMenuCategoryPath") != std::string::npos, "Managed windows should expose an optional menu category path.");
			context.Expect(managedApplication.find("window.WindowMenuCategoryPath") != std::string::npos, "Managed menu rendering should ignore windows without a declared category.");
			context.Expect(managedApplication.find("Split('/', StringSplitOptions.RemoveEmptyEntries)") != std::string::npos, "Managed menu rendering should build arbitrary nested subcategories.");
			context.Expect(statusWindow.find("Managed/Diagnostics") != std::string::npos, "Engine Status should own its managed menu category.");
		});

		registry.Add("Editor.ProjectSettings.BuildProfilesFileMenuContract", [](TestContext& context) {
			context.ExpectEqual(
				std::string("File"),
				std::string(TerraEngine::Editor::ProjectSettings::kBuildProfilesMenuName),
				"Build Profiles should remain a File menu workflow.");
		});

		registry.Add("Editor.ProjectSettings.SceneSkyCategoryIsRemoved", [](TestContext& context) {
			context.Expect(
				TerraEngine::Editor::ProjectSettings::FindProjectSettingsCategory(
					TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Graphics) != nullptr,
				"Graphics settings should be available as a Project Settings category.");
			context.Expect(
				TerraEngine::Editor::ProjectSettings::FindProjectSettingsCategory(
					TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Resources) != nullptr,
				"Resource residency and streaming policy should be available as a Project Settings category.");
			context.Expect(
				TerraEngine::Editor::ProjectSettings::FindProjectSettingsCategory(
					TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId::Sky) == nullptr,
				"Sky settings should be authored through SkyComponent instead of a scene-level Project Settings category.");
		});
	}
}
