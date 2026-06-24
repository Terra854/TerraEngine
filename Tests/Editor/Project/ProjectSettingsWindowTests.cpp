#include "TestHarness.h"
#include "ProjectSettingsTestHelpers.h"

namespace TerraEngine::EditorTests
{
	void RegisterProjectSettingsWindowTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectSettings.EditMenuContract", [](TestContext& context) {
			context.ExpectEqual(
				std::string("Edit"),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuName),
				"Project Settings should belong to the Edit menu.");
			context.ExpectEqual(
				std::string("Project Settings..."),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuLabel),
				"Project Settings should use the expected menu label.");
			context.ExpectEqual(
				std::string("Project Settings"),
				std::string(TerraEngine::Editor::ProjectSettings::kProjectSettingsWindowTitle),
				"Project Settings should open the expected window title.");
		});

		registry.Add("Editor.ProjectSettings.CategorySearchContract", [](TestContext& context) {
			const auto allCategories = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("");
			context.ExpectEqual(static_cast<std::size_t>(2), allCategories.size(), "Empty search should show the current Project Settings categories.");

			const auto shadowMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("shadow");
			context.ExpectEqual(static_cast<std::size_t>(1), shadowMatches.size(), "Shadow search should find the graphics category.");
			context.ExpectEqual(
				std::string("graphics"),
				std::string(shadowMatches.front().stableId),
				"Shadow search should route to the graphics category.");

			const auto skyMatches = TerraEngine::Editor::ProjectSettings::FilterProjectSettingsCategories("directional");
			context.ExpectEqual(static_cast<std::size_t>(0), skyMatches.size(), "Directional sky search should not expose removed scene-level sky settings.");
		});
	}
}
