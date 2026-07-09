#include "TestHarness.h"
#include "ProjectSettingsTestHelpers.h"

namespace TerraEngine::EditorTests
{
	void RegisterProjectSettingsMenuInventoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectSettings.RemovedWindowsAreHiddenFromWindowMenu", [](TestContext& context) {
			const std::vector<std::string> visibleTitles = ProjectSettings::BuildWindowMenuTitlesForContractTest();
			context.Expect(
				!ProjectSettings::ContainsTitle(visibleTitles, TerraEngine::Editor::Preferences::kPreferencesWindowTitle),
				"Preferences should be opened from Edit instead of appearing as an individual Window menu entry.");
			context.Expect(
				!ProjectSettings::ContainsTitle(visibleTitles, TerraEngine::Editor::ProjectSettings::kSkySettingsWindowTitle),
				"Sky Settings should be removed as an individual Window menu entry after consolidation.");
			context.Expect(
				!ProjectSettings::ContainsTitle(visibleTitles, TerraEngine::Editor::ProjectSettings::kRenderSettingsWindowTitle),
				"Render Settings should be removed as an individual Window menu entry after consolidation.");
		});

		registry.Add("Editor.ProjectSettings.BuildProfilesFileMenuContract", [](TestContext& context) {
			const std::vector<std::string> visibleTitles = ProjectSettings::BuildWindowMenuTitlesForContractTest();
			context.Expect(
				!ProjectSettings::ContainsTitle(visibleTitles, TerraEngine::Editor::ProjectSettings::kBuildProfilesWindowTitle),
				"Build Profiles should be hidden from the Window menu.");
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
