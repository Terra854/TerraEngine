#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/ProjectSettings.h"
#include "Editor/TerraEngine-Editor/Project/ProjectPhysicsSettingsIO.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewCameraState.h"

#include <filesystem>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <optional>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadFileBytes(const std::filesystem::path& path)
		{
			// Layout tests compare raw bytes because ImGui docking identifiers and
			// formatting are part of the approved new-project workspace contract.
			std::ifstream input(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		class ScopedEditorPreferenceFile
		{
		public:
			explicit ScopedEditorPreferenceFile(const std::string& name)
				: root_(std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / name)
				, preferencesPath_(root_ / ".terra_editor_prefs.json")
			{
				if (const char* existing = std::getenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment))
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
				_putenv_s(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str());
#else
				setenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str(), 1);
#endif
			}

			static void ClearPreferencePath()
			{
#ifdef _WIN32
				_putenv_s(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, "");
#else
				unsetenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment);
#endif
			}

			std::filesystem::path root_;
			std::filesystem::path preferencesPath_;
			std::optional<std::string> previousOverride_;
		};
	}

	void RegisterProjectSettingsPersistenceTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectSettings.SceneViewCameraWorkspaceRoundTrips", [](TestContext& context) {
			TerraEngine::Editor::SceneView::CameraWorkspaceState expected;
			expected.position.cell = {1'000'000, -7, -2'000'000};
			expected.position.offsetMetres = {12.5, 31.25, -42.75};
			expected.rotationRadians = {-0.61F, 1.42F, 0.0F};
			expected.focus.cell = expected.position.cell;
			expected.focus.offsetMetres = {120.0, 18.0, -240.0};
			expected.focusDistance = 287.5F;
			expected.focusInitialized = true;
			expected.fieldOfViewDegrees = 72.0F;
			expected.orthographicHeight = 144.0F;
			expected.projectionMode = TerraEngine::Editor::SceneViewOverlay::ProjectionMode::Orthographic;

			const nlohmann::json encoded = TerraEngine::Editor::SceneView::SerializeCameraWorkspaceState(expected);
			std::string error;
			const auto restored = TerraEngine::Editor::SceneView::DeserializeCameraWorkspaceState(encoded, &error);
			context.Expect(restored.has_value(), error.empty() ? "Scene View camera state should round-trip." : error);
			context.Expect(restored && restored->position == expected.position && restored->focus == expected.focus
				&& restored->rotationRadians == expected.rotationRadians
				&& std::abs(restored->focusDistance - expected.focusDistance) < 0.001F
				&& std::abs(restored->fieldOfViewDegrees - expected.fieldOfViewDegrees) < 0.001F
				&& std::abs(restored->orthographicHeight - expected.orthographicHeight) < 0.001F
				&& restored->focusInitialized
				&& restored->projectionMode == expected.projectionMode,
				"Workspace persistence must preserve the exact canonical camera composition.");
		});

		registry.Add("Editor.ProjectSettings.SceneViewCameraWorkspaceRejectsPartialState", [](TestContext& context) {
			nlohmann::json invalid = TerraEngine::Editor::SceneView::SerializeCameraWorkspaceState({});
			invalid["rotationRadians"] = {0.0F, "not-a-number", 0.0F};
			std::string error;
			const auto restored = TerraEngine::Editor::SceneView::DeserializeCameraWorkspaceState(invalid, &error);
			context.Expect(!restored.has_value() && !error.empty(),
				"Malformed camera workspace input must be rejected as one atomic record.");
		});

		registry.Add("Editor.ProjectSettings.SceneViewCameraUsesProjectLayoutLifecycle", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor" / "TerraEngine-Editor" / "Core" / "EditorSystem.cpp";
			const std::string source = ReadFileBytes(sourcePath);
			context.Expect(source.find("layout[\"sceneViewCamera\"] = sceneView->SerializeWorkspaceState()") != std::string::npos
				&& source.find("sceneView->RestoreWorkspaceState(layout[\"sceneViewCamera\"], cameraError)") != std::string::npos,
				"Project close and reopen must save and restore Scene View camera state through EditorLayout.terra.");
		});

		registry.Add("Editor.ProjectSettings.PhysicsQualityLoadsForPlayMode", [](TestContext& context) {
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineProjectSettings" / "PhysicsQualityLoadsForPlayMode";
			const std::filesystem::path settingsDirectory = root / "ProjectSettings";
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::create_directories(settingsDirectory, ignored);

			TerraEngine::Physics::PhysicsSettings expected =
				TerraEngine::Physics::PhysicsSettings::Defaults();
			nlohmann::json document = {
				{"schemaVersion", expected.schemaVersion},
				{"gravity", {expected.gravity.x, expected.gravity.y, expected.gravity.z}},
				{"velocityIterations", 20U},
				{"positionIterations", 4U},
				{"layers", nlohmann::json::array()}
			};
			for (const auto& layer : expected.layers)
			{
				document["layers"].push_back({
					{"id", layer.id}, {"name", layer.name},
					{"collisionMask", layer.collisionMask},
					{"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}
				});
			}
			std::ofstream(settingsDirectory / "PhysicsSettings.json") << document.dump(2);

			TerraEngine::Physics::PhysicsSettings loaded;
			std::string error;
			context.Expect(TerraEngine::Editor::ProjectSettings::ReadProjectPhysicsSettings(
				root, loaded, error), error.empty() ? "Play Mode should load valid project physics settings." : error);
			context.ExpectEqual(20U, loaded.velocityIterations,
				"Play Mode must preserve the authored velocity solver quality.");
			context.ExpectEqual(4U, loaded.positionIterations,
				"Play Mode must preserve the authored position solver quality.");

			std::filesystem::remove_all(root, ignored);
		});

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

		registry.Add("Editor.ProjectSettings.NewProjectsCopyDefaultEditorLayout", [](TestContext& context) {
			const std::filesystem::path root =
				std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / "DefaultEditorLayout";
			const std::filesystem::path templateRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor" / "TerraEngine-Editor" / "Project" / "Templates" / "UserSettings";
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);
			std::filesystem::create_directories(root.parent_path(), ignored);

			{
				::ProjectManager manager;
				std::string error;
				context.Expect(manager.CreateProject(root, "Default Editor Layout", ::ProjectTemplate::Blank, &error), error.empty() ? "New project creation should install the default editor layout." : error);
				for (const std::string_view fileName : { "EditorLayout.terra", "EditorLayout.ini" })
				{
					const std::filesystem::path source = templateRoot / fileName;
					const std::filesystem::path generated = root / "UserSettings" / fileName;
					context.Expect(std::filesystem::exists(generated), "New projects should contain both default layout files.");
					context.ExpectEqual(ReadFileBytes(source), ReadFileBytes(generated), "New-project layouts should match the approved templates byte-for-byte.");
				}
			}

			std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.ProjectSettings.NewProjectsCreateLoadableDefaultScene", [](TestContext& context) {
			const std::filesystem::path root =
				std::filesystem::temp_directory_path() / "TerraEngineProjectSettings" / "LoadableDefaultScene";
			std::error_code ignored;
			std::filesystem::remove_all(root, ignored);

			{
				::ProjectManager manager;
				std::string error;
				context.Expect(manager.CreateProject(root, "Loadable Default Scene", ::ProjectTemplate::Blank, &error),
					error.empty() ? "New project creation should succeed." : error);

				::ProjectSceneLoadDecision decision;
				context.Expect(manager.PrepareSceneForLoading("Scenes/Scene.tescene", decision, &error),
					error.empty() ? "A new project's default scene should pass the production scene preparation path." : error);
				context.Expect(decision.prepared, "The default scene should produce a prepared scene packet.");
				context.ExpectEqual(std::size_t{2U}, decision.preparedScene.objectCount,
					"The default scene should contain its sky and camera objects.");
				context.Expect(decision.preparedScene.objects.size() == 2U
					&& decision.preparedScene.objects[0].object.value("uuid", std::string{}).size() == 32U
					&& decision.preparedScene.objects[1].object.value("uuid", std::string{}).size() == 32U,
					"Every generated default object should carry a stable scene identity.");
				context.Expect(decision.preparedScene.objects[0].object.value("uuid", std::string{})
					!= decision.preparedScene.objects[1].object.value("uuid", std::string{}),
					"Generated default object identities should be unique within the scene.");
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

		registry.Add("Editor.Preferences.ManagerKeepsConstructionStore", [](TestContext& context) {
			ScopedEditorPreferenceFile firstStore("ManagerKeepsConstructionStoreA");
			::ProjectManager manager;
			const std::filesystem::path firstPath = firstStore.PreferencesPath();

			{
				// A later environment change must not redirect an already-running
				// editor. This prevents test runners from leaking a changing MCP port
				// into the user's global preference document.
				ScopedEditorPreferenceFile secondStore("ManagerKeepsConstructionStoreB");
				manager.SetAutoBuildScripts(false);
				context.Expect(std::filesystem::exists(firstPath), "The manager should save to the store selected at construction.");
				context.Expect(!std::filesystem::exists(secondStore.PreferencesPath()), "A later session path must not receive writes from an existing manager.");
			}
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
