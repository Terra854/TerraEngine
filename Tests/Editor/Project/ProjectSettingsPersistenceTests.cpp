#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettings.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"

#include <filesystem>
#include <fstream>
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

		registry.Add("Editor.ProjectSettings.NewProjectsPersistTextureMipStreamingDefaults", [](TestContext& context) {
			const std::filesystem::path root =
				std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / "TextureMipStreamingDefaults";
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::create_directories(root.parent_path(), ignored);

			{
				::ProjectManager manager;
				std::string error;
				context.Expect(manager.CreateProject(root, "Texture Mip Streaming Defaults", ::ProjectTemplate::Blank, &error), "New project creation should succeed before checking default streaming policy.");

				std::ifstream input(root / "game.json");
				nlohmann::json manifest;
				input >> manifest;
				context.Expect(manifest.contains("streamingPolicy"), "New project manifests should persist explicit streaming defaults.");
				const nlohmann::json& streamingPolicy = manifest["streamingPolicy"];
				context.Expect(streamingPolicy.value("textureMipStreamingEnabled", false), "New project manifests should enable texture mip streaming.");
				context.ExpectEqual(std::uint32_t{0u}, streamingPolicy.value("textureMinStreamMip", 99u), "New project near texture mip should default to full resolution.");
				context.ExpectEqual(std::uint32_t{13u}, streamingPolicy.value("textureMaxStreamMip", 0u), "New project far texture mip should default to mip 13.");
			}

			std::filesystem::remove_all(root, ignored);
		});
	}
}
