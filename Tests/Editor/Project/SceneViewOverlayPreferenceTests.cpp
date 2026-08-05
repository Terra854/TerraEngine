#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferences.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOrientationWidget.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class ScopedSceneViewPreferenceFile
		{
		public:
			ScopedSceneViewPreferenceFile()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineSceneViewPreferenceTests")
				, preferencesPath_(root_ / ".terra_editor_prefs.json")
			{
				if (const char* existing = std::getenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment))
				{
					previousOverride_ = existing;
				}
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
				std::filesystem::create_directories(root_, ignored);
				SetPath(preferencesPath_.string());
			}

			~ScopedSceneViewPreferenceFile()
			{
				if (previousOverride_.has_value())
				{
					SetPath(*previousOverride_);
				}
				else
				{
					ClearPath();
				}
				std::error_code ignored;
				std::filesystem::remove_all(root_, ignored);
			}

			const std::filesystem::path& Path() const
			{
				return preferencesPath_;
			}

		private:
			static void SetPath(const std::string& path)
			{
#ifdef _WIN32
				_putenv_s(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str());
#else
				setenv(TerraEngine::Editor::kEditorPreferencesSessionPathEnvironment, path.c_str(), 1);
#endif
			}

			static void ClearPath()
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

		std::string ReadBytes(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}
	}

	void RegisterSceneViewOverlayPreferenceTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Preferences.RoundTripAndFallback", [](TestContext& context) {
			EditorPreferences preferences;
			preferences.sceneViewOverlay.iconsVisible = false;
			preferences.sceneViewOverlay.projectionMode = TerraEngine::Editor::SceneViewOverlay::ProjectionMode::Orthographic;
			preferences.sceneViewOverlay.translationSnap = 2.0f;
			preferences.sceneViewOverlay.largeSceneMode = TerraEngine::Editor::SceneViewOverlay::LargeSceneMode::On;
			preferences.sceneViewOverlay.largeSceneObjectThreshold = 3456u;
			preferences.sceneViewOverlay.visibleMarkerBudget = 4096u;
			preferences.sceneViewOverlay.markerPresentationBatchSize = 1024u;
			preferences.sceneViewOverlay.densityRadiusPixels = 24.0f;
			preferences.sceneViewOverlay.densityThreshold = 8u;
			preferences.sceneViewOverlay.maximumMarkerDistance = 5000.0f;
			preferences.sceneViewOverlay.minimumProjectedSizePixels = 2.0f;
			preferences.sceneViewOverlay.physicsMarkerMode = TerraEngine::Editor::SceneViewOverlay::PhysicsMarkerMode::All;
			preferences.sceneViewOverlay.densePrimitiveTier = TerraEngine::Editor::SceneViewOverlay::DensePrimitiveTier::Point;

			nlohmann::json json = preferences;
			const EditorPreferences restored = json.get<EditorPreferences>();
			context.Expect(!restored.sceneViewOverlay.iconsVisible, "Icon visibility should persist.");
			context.ExpectEqual(std::string("orthographic"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(restored.sceneViewOverlay.projectionMode)), "Projection mode should persist.");
			context.ExpectEqual(2.0f, restored.sceneViewOverlay.translationSnap, "Valid snap values should persist.");
			context.ExpectEqual(std::string("on"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(restored.sceneViewOverlay.largeSceneMode)), "Large-scene mode should persist.");
			context.ExpectEqual(static_cast<std::uint32_t>(4096u), restored.sceneViewOverlay.visibleMarkerBudget, "Marker budget should persist.");
			context.ExpectEqual(std::string("all"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(restored.sceneViewOverlay.physicsMarkerMode)), "Physics marker mode should persist.");
			context.ExpectEqual(std::string("point"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(restored.sceneViewOverlay.densePrimitiveTier)), "Dense primitive tier should persist.");

			nlohmann::json invalid = nlohmann::json::object({{"sceneViewOverlay", nlohmann::json::object({{"translationSnap", -1.0f}})}});
			const EditorPreferences fallback = invalid.get<EditorPreferences>();
			context.ExpectEqual(0.5f, fallback.sceneViewOverlay.translationSnap, "Invalid snap values should fall back during preference load.");

			const nlohmann::json malformedPerformance = {
				{"sceneViewOverlay", {
					{"largeSceneMode", 17},
					{"visibleMarkerBudget", "many"},
					{"densityRadiusPixels", false},
					{"physicsMarkerMode", nlohmann::json::array()},
					{"densePrimitiveTier", nullptr}
				}}
			};
			const EditorPreferences migrated = malformedPerformance.get<EditorPreferences>();
			context.ExpectEqual(std::string("auto"), std::string(TerraEngine::Editor::SceneViewOverlay::ToString(migrated.sceneViewOverlay.largeSceneMode)), "Malformed large-scene strings should migrate to automatic mode.");
			context.ExpectEqual(static_cast<std::uint32_t>(2048u), migrated.sceneViewOverlay.visibleMarkerBudget, "Malformed marker settings should fall back independently.");
		});

		registry.Add("SceneViewOverlay.Preferences.LargeSceneDefaultsAndValidation", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			const SceneOverlayPreference defaults;
			context.ExpectEqual(std::string("auto"), std::string(ToString(defaults.largeSceneMode)), "Large-scene mode should default to automatic.");
			context.ExpectEqual(static_cast<std::uint32_t>(2000u), defaults.largeSceneObjectThreshold, "Large-scene threshold should match the schema default.");
			context.ExpectEqual(static_cast<std::uint32_t>(2048u), defaults.visibleMarkerBudget, "Marker budget should match the schema default.");
			context.ExpectEqual(static_cast<std::uint32_t>(512u), defaults.markerPresentationBatchSize, "Presentation batch size should match the schema default.");
			context.ExpectEqual(18.0f, defaults.densityRadiusPixels, "Density radius should match the schema default.");
			context.ExpectEqual(static_cast<std::uint32_t>(4u), defaults.densityThreshold, "Density threshold should match the schema default.");
			context.ExpectEqual(0.0f, defaults.maximumMarkerDistance, "Zero should preserve the viewport query distance limit by default.");
			context.ExpectEqual(1.0f, defaults.minimumProjectedSizePixels, "Sub-pixel markers should be rejected by default.");
			context.ExpectEqual(std::string("clustered"), std::string(ToString(defaults.physicsMarkerMode)), "Physics markers should default to clustered presentation.");
			context.ExpectEqual(std::string("simple"), std::string(ToString(defaults.densePrimitiveTier)), "Dense markers should default to simple primitives.");
			context.Expect(ValidatePreference(defaults), "Documented defaults should pass strict validation.");

			SceneOverlayPreference invalid = defaults;
			invalid.visibleMarkerBudget = 63u;
			std::string error;
			context.Expect(!ValidatePreference(invalid, &error), "A budget below the schema minimum should be rejected.");
			context.Expect(!error.empty(), "Strict validation should explain the rejected setting.");
			context.ExpectEqual(static_cast<std::uint32_t>(2048u), SanitizePreference(invalid).visibleMarkerBudget, "Loading an invalid budget should use the documented default.");

			const auto expectRejected = [&context, &defaults](const auto& mutate, const char* message) {
				SceneOverlayPreference candidate = defaults;
				mutate(candidate);
				context.Expect(!ValidatePreference(candidate), message);
			};
			expectRejected([](SceneOverlayPreference& value) { value.largeSceneObjectThreshold = 0u; }, "A zero object threshold should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.markerPresentationBatchSize = 31u; }, "A presentation batch below the schema minimum should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.densityRadiusPixels = 129.0f; }, "A density radius above the schema maximum should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.densityThreshold = 1u; }, "A density threshold below the schema minimum should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.maximumMarkerDistance = -1.0f; }, "A negative marker distance should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.minimumProjectedSizePixels = 65.0f; }, "A projected-size limit above the schema maximum should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.largeSceneMode = static_cast<LargeSceneMode>(99); }, "An unknown large-scene mode should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.physicsMarkerMode = static_cast<PhysicsMarkerMode>(99); }, "An unknown physics marker mode should be rejected.");
			expectRejected([](SceneOverlayPreference& value) { value.densePrimitiveTier = static_cast<DensePrimitiveTier>(99); }, "An unknown dense primitive tier should be rejected.");
		});

		registry.Add("SceneViewOverlay.Preferences.AtomicCandidateAndEffectiveState", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			EditorPreferences stored;
			stored.sceneViewOverlay.visibleMarkerBudget = 4096u;
			SceneOverlayPreference invalid = stored.sceneViewOverlay;
			invalid.densityThreshold = 1u;
			std::string error;
			context.Expect(!TerraEngine::Editor::Settings::TryReplaceSceneViewOverlayPreference(stored, invalid, error), "An invalid complete candidate should be rejected.");
			context.ExpectEqual(static_cast<std::uint32_t>(4096u), stored.sceneViewOverlay.visibleMarkerBudget, "A rejected candidate should leave every stored field unchanged.");
			context.ExpectEqual(static_cast<std::uint32_t>(4u), stored.sceneViewOverlay.densityThreshold, "A rejected candidate should not partially apply its invalid density value.");

			SceneOverlayPreference automatic;
			const SceneViewLargeSceneState small = ResolveLargeSceneState(automatic, 1999u);
			const SceneViewLargeSceneState large = ResolveLargeSceneState(automatic, 2000u);
			context.Expect(!small.effective, "Automatic safeguards should stay inactive below the threshold.");
			context.Expect(large.effective, "Automatic safeguards should activate at the configured threshold.");
			automatic.largeSceneMode = LargeSceneMode::Off;
			context.Expect(!ResolveLargeSceneState(automatic, 10000u).effective, "Configured off should remain distinct from the large scene's effective state.");
			automatic.largeSceneMode = LargeSceneMode::On;
			context.Expect(ResolveLargeSceneState(automatic, 1u).effective, "Configured on should activate independently of object count.");
		});

		registry.Add("SceneViewOverlay.Preferences.AtomicFileReplacement", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			ScopedSceneViewPreferenceFile scopedFile;
			ProjectManager manager;
			SceneOverlayPreference candidate;
			candidate.visibleMarkerBudget = 4096u;
			std::string error;
			context.Expect(manager.SetSceneViewOverlayPreference(candidate, &error), error.empty() ? "A valid preference should persist." : error);
			context.Expect(std::filesystem::exists(scopedFile.Path()), "Atomic preference publication should create the final file.");
			context.Expect(!std::filesystem::exists(scopedFile.Path().string() + ".tmp"), "Successful publication should not leave a staged file.");
			const std::string firstDocument = ReadBytes(scopedFile.Path());

			candidate.visibleMarkerBudget = 8192u;
			context.Expect(manager.SetSceneViewOverlayPreference(candidate, &error), error.empty() ? "A second valid preference should replace the first file." : error);
			const nlohmann::json replaced = nlohmann::json::parse(ReadBytes(scopedFile.Path()));
			context.ExpectEqual(static_cast<std::uint32_t>(8192u), replaced["sceneViewOverlay"].value("visibleMarkerBudget", 0u), "Atomic replacement should publish the complete second document.");

			const std::string validDocument = ReadBytes(scopedFile.Path());
			candidate.visibleMarkerBudget = 63u;
			context.Expect(!manager.SetSceneViewOverlayPreference(candidate, &error), "An invalid update should be rejected before staging.");
			context.ExpectEqual(validDocument, ReadBytes(scopedFile.Path()), "A rejected update should leave the saved document byte-for-byte unchanged.");
			context.ExpectEqual(static_cast<std::uint32_t>(8192u), manager.GetEditorPreferences().sceneViewOverlay.visibleMarkerBudget, "A rejected update should leave the live preference unchanged.");
			context.Expect(firstDocument != validDocument, "The successful replacement should have changed the saved document.");
		});

		registry.Add("SceneViewOverlay.Preferences.OverlayGenerationCacheKey", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			SceneViewOverlayCacheKey key{1u, 2u, 3u, 4u, 5u};
			OrientationWidgetCache cache;
			const OrientationWidgetState& first = BuildOrientationWidgetStateCached(cache, key, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f), ProjectionMode::Perspective, glm::vec4(0.0f, 0.0f, 100.0f, 100.0f));
			context.ExpectEqual(std::string("Persp"), first.label, "The first cache request should build orientation geometry.");
			const OrientationWidgetState& reused = BuildOrientationWidgetStateCached(cache, key, glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), ProjectionMode::Orthographic, glm::vec4(10.0f));
			context.ExpectEqual(static_cast<std::uint64_t>(1u), cache.reuseCount, "Identical generations should reuse cached overlay geometry.");
			context.ExpectEqual(std::string("Persp"), reused.label, "A reused cache should retain the geometry built for its key.");

			++key.simulationGeneration;
			const OrientationWidgetState& rebuilt = BuildOrientationWidgetStateCached(cache, key, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f), ProjectionMode::Orthographic, glm::vec4(0.0f, 0.0f, 100.0f, 100.0f));
			context.ExpectEqual(static_cast<std::uint64_t>(2u), cache.rebuildCount, "A simulation-generation change should invalidate cached overlay geometry.");
			context.ExpectEqual(std::string("Ortho"), rebuilt.label, "An invalidated cache should publish rebuilt geometry.");
		});
	}
}
