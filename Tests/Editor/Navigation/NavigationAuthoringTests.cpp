// Editor hard-break tests: only profile/settings authoring remains.

#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	void RegisterNavigationAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Editor.NavigationAuthoring.RemovedTypesHaveNoInspectorSurface", [](TestContext& context) {
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Navigation/NavigationInspectorSupport.h");
			std::ostringstream text;
			text << input.rdbuf();
			context.Expect(text.str().find("NavigationSourceComponent") == std::string::npos
				&& text.str().find("NavigationConnectionComponent") == std::string::npos,
				"Inspector support must expose no removed navigation component or connection widgets.");
		});
	}
}
