#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettings.h"

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	void RegisterProjectSettingsPersistenceTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectSettings.ValidProjectDisplayNameRoundTrips", [](TestContext& context) {
			::ProjectSettings settings;
			settings.projectDisplayName = "Sample Project";

			nlohmann::json json = settings;
			const ::ProjectSettings restored = json.get<::ProjectSettings>();

			context.ExpectEqual(
				std::string("Sample Project"),
				restored.projectDisplayName,
				"Project display name should persist through project settings JSON.");
		});

		registry.Add("Editor.ProjectSettings.LegacySettingsUseDefaultDisplayName", [](TestContext& context) {
			const nlohmann::json legacyJson = nlohmann::json::object({ { "_placeholder", 0 } });
			const ::ProjectSettings restored = legacyJson.get<::ProjectSettings>();

			context.ExpectEqual(
				std::string("TerraEngine Project"),
				restored.projectDisplayName,
				"Legacy ProjectSettings files should load with a safe default display name.");
		});
	}
}
