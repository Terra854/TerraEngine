#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Core/EditorStartupRendererConfiguration.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferencesStorage.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class ScopedEditorPreferenceSessionPath
		{
		public:
			explicit ScopedEditorPreferenceSessionPath(const std::filesystem::path& path)
			{
				// Preserve the parent process exactly because the editor test executable
				// contains many preference owners that run sequentially.
				if (const char* existing = std::getenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment))
				{
					previousPath_ = existing;
				}
				Set(path.string());
			}

			~ScopedEditorPreferenceSessionPath()
			{
				if (previousPath_.has_value())
				{
					Set(*previousPath_);
				}
				else
				{
					Clear();
				}
			}

		private:
			static void Set(const std::string& path)
			{
#ifdef _WIN32
				_putenv_s(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str());
#else
				setenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str(), 1);
#endif
			}

			static void Clear()
			{
#ifdef _WIN32
				_putenv_s(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, "");
#else
				unsetenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment);
#endif
			}

			std::optional<std::string> previousPath_;
		};
	}

	void RegisterEditorStartupRendererConfigurationTests(TestRegistry& registry)
	{
		registry.Add("Editor.Preferences.GlobalPathIsIndependentFromSessionOverride", [](TestContext& context) {
			const std::filesystem::path globalPath = TerraEngine::Editor::ResolveGlobalEditorPreferencesPath();
			const std::filesystem::path sessionPath = std::filesystem::temp_directory_path()
				/ "TerraEnginePreferences" / "startup-session.json";
			ScopedEditorPreferenceSessionPath session(sessionPath);
			const auto location = TerraEngine::Editor::ResolveEditorPreferencesStorageLocation();

			context.Expect(location.isolatedSession, "An explicit automation path must be classified as an isolated session store.");
			context.ExpectEqual(sessionPath.string(), location.path.string(), "The active session should use only its explicit preference document.");
			context.ExpectEqual(
				globalPath.string(),
				TerraEngine::Editor::ResolveGlobalEditorPreferencesPath().string(),
				"A session override must not redirect the canonical global preference source.");
		});

		registry.Add("Editor.StartupRenderer.DirectProjectIsReadOnlyAndSanitized", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / "TerraEngineStartupRendererConfiguration";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "ProjectSettings");
			const auto settingsPath = root / "ProjectSettings" / "RenderSettings.json";
			{
				std::ofstream output(settingsPath);
				output << R"({"validationEnabled":true,"settings":{"shadowResolution":999999,"variableRateShading":1}})";
			}
			const auto before = std::filesystem::last_write_time(settingsPath);
			const auto configuration = TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(root);
			context.Expect(configuration.origin == TerraEngine::Editor::StartupRendererOrigin::DirectProject, "An explicit project must not become a hub launch.");
			context.ExpectEqual(16384u, configuration.renderSettings.shadowResolution, "Unsafe settings should be clamped before Vulkan startup.");
			context.ExpectEqual(before, std::filesystem::last_write_time(settingsPath), "Bootstrap must not write project files.");
			std::filesystem::remove_all(root);
		});

		registry.Add("Editor.StartupRenderer.HubDoesNotDiscoverWorkingDirectory", [](TestContext& context) {
			const auto configuration = TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(std::nullopt);
			context.Expect(configuration.origin == TerraEngine::Editor::StartupRendererOrigin::HubDefaults, "Missing --project must select explicit hub defaults.");
			context.Expect(!configuration.projectRoot.has_value(), "Hub defaults must not infer a cwd project root.");
		});
	}
}
