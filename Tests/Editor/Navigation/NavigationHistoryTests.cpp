// Editor history hard-break contract: profile/settings history has no removed
// source or authored-connection payload.

#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	void RegisterNavigationHistoryTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationHistory.RetainsOnlyProfileAndSettingsState", [](TestContext& context) {
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Navigation/NavigationAuthoringTransactions.h");
			std::ostringstream text;
			text << input.rdbuf();
			const std::string source = text.str();
			context.Expect(source.find("NavigationSourceComponent") == std::string::npos
				&& source.find("NavigationConnectionComponent") == std::string::npos,
				"Navigation undo/save history must retain profile/settings state only.");
		});
	}
}
