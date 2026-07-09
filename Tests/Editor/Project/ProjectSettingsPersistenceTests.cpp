#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettings.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <optional>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class ScopedEditorPreferenceFile
		{
		public:
			explicit ScopedEditorPreferenceFile(const std::string& name)
				: root_(std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / name)
				, preferencesPath_(root_ / ".terra_editor_prefs.json")
			{
				if (const char* existing = std::getenv("TERRAENGINE_EDITOR_PREFS_PATH"))
				{
					previousOverride_ = existing;
				}

				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
				std::filesystem::create_directories(root_, ignored);
				SetPreferencePath(preferencesPath_.string());
			}

			~ScopedEditorPreferenceFile()
			{
				if (previousOverride_.has_value())
				{
					SetPreferencePath(*previousOverride_);
				}
				else
				{
					ClearPreferencePath();
				}

				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
			}

			const std::filesystem::path& PreferencesPath() const
			{
				return preferencesPath_;
			}

		private:
			static void SetPreferencePath(const std::string& path)
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str());
#else
				setenv("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str(), 1);
#endif
			}

			static void ClearPreferencePath()
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", "");
#else
				unsetenv("TERRAENGINE_EDITOR_PREFS_PATH");
#endif
			}

			std::filesystem::path root_;
			std::filesystem::path preferencesPath_;
			std::optional<std::string> previousOverride_;
		};
	}

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
				context.ExpectEqual(300.0, streamingPolicy.value("streamingDistance", 0.0), "New project manifests should use conservative texture streaming distance.");
				context.ExpectEqual(std::uint32_t{0u}, streamingPolicy.value("textureMinStreamMip", 99u), "New project near texture mip should default to full resolution.");
				context.ExpectEqual(std::uint32_t{8u}, streamingPolicy.value("textureMaxStreamMip", 0u), "New project far texture mip should default to mip 8.");
			}

			std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.ProjectSettings.TempProjectsDoNotPolluteRecents", [](TestContext& context) {
			ScopedEditorPreferenceFile preferences("TempProjectsDoNotPolluteRecents");
			const std::filesystem::path root =
				std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / "TempRecentProject";
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);

			{
				::ProjectManager manager;
				std::string error;
				context.Expect(manager.CreateProject(root, "Temp Recent Project", ::ProjectTemplate::Blank, &error), error.empty() ? "Temporary project should load successfully." : error);
				context.Expect(manager.GetRecentProjects().empty(), "Projects created under the OS temp directory should not appear in Recent Projects.");
			}

			std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.ProjectSettings.TempRecentEntriesArePrunedOnLoad", [](TestContext& context) {
			ScopedEditorPreferenceFile preferences("TempRecentEntriesArePrunedOnLoad");
			const std::filesystem::path disposableProject =
				std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryAutomation" / "Inspect";
			const std::filesystem::path userProject =
				std::filesystem::current_path() / "ExampleUserProject";

			// The preference file can outlive an older test run. Loading it should
			// clean disposable temp projects while preserving normal user paths.
			nlohmann::json json;
			json["recentProjects"] = nlohmann::json::array({
				{
					{"name", "Inspect"},
					{"path", disposableProject.string()},
					{"lastOpened", 1}
				},
				{
					{"name", "ExampleUserProject"},
					{"path", userProject.string()},
					{"lastOpened", 2}
				}
			});
			json["theme"] = "Dark";
			json["autoBuildScripts"] = true;
			std::ofstream(preferences.PreferencesPath()) << json.dump(4);

			::ProjectManager manager;
			const auto& recents = manager.GetRecentProjects();
			context.ExpectEqual(std::size_t{1u}, recents.size(), "Loading editor preferences should prune temp automation project recents.");
			if (!recents.empty())
			{
				context.ExpectEqual(std::string("ExampleUserProject"), recents.front().name, "Pruning should keep normal user project recents.");
			}
		});
	}
}
