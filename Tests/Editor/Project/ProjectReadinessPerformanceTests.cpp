#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		// Each test owns one unique root so cleanup cannot remove a user project or
		// another test's fixture.  The counter also avoids collisions when separate
		// editor-test processes begin during the same clock tick.
		std::atomic_uint64_t g_NextFixtureId{ 0U };

		class ProjectReadinessFixture
		{
		public:
			explicit ProjectReadinessFixture(const char* scenario)
			{
				const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
				const std::uint64_t identifier = g_NextFixtureId.fetch_add(1U, std::memory_order_relaxed);
				m_Root = std::filesystem::temp_directory_path()
					/ (std::string("TerraEngineProjectReadiness_") + scenario + "_" + std::to_string(tick) + "_" + std::to_string(identifier));
				m_PreferencesPath = m_Root / ".terra_editor_prefs.json";

				if (const char* previous = std::getenv("TERRAENGINE_EDITOR_PREFS_PATH"))
				{
					m_PreviousPreferencesPath = previous;
				}
				std::error_code ignored;
				std::filesystem::remove_all(m_Root, ignored);
				std::filesystem::create_directories(m_Root, ignored);
				SetPreferencesPath(m_PreferencesPath.string());
			}

			~ProjectReadinessFixture()
			{
				// ProjectManager writes editor preferences during destruction.  Restore
				// the caller's process setting before deleting only this fixture root.
				if (m_PreviousPreferencesPath.has_value())
				{
					SetPreferencesPath(*m_PreviousPreferencesPath);
				}
				else
				{
					ClearPreferencesPath();
				}
				std::error_code ignored;
				std::filesystem::remove_all(m_Root, ignored);
			}

			[[nodiscard]] bool CreateProject(std::size_t objectCount, std::string& outError) const
			{
				std::error_code error;
				std::filesystem::create_directories(SourcePath().parent_path(), error);
				if (error)
				{
					outError = "Could not create the disposable Assets/Scenes directory: " + error.message();
					return false;
				}
				std::filesystem::create_directories(m_Root / "Library", error);
				if (error)
				{
					outError = "Could not create the disposable Library directory: " + error.message();
					return false;
				}
				std::filesystem::create_directories(m_Root / "ProjectSettings", error);
				if (error)
				{
					outError = "Could not create the disposable ProjectSettings directory: " + error.message();
					return false;
				}

				if (!WriteText(m_Root / "ProjectSettings" / "ProjectSettings.terra", "{\"projectDisplayName\":\"Readiness Test\"}\n", outError))
				{
					return false;
				}
				return WriteScene(objectCount, outError);
			}

			[[nodiscard]] bool ReplaceSource(std::size_t objectCount, std::string& outError) const
			{
				return WriteScene(objectCount, outError);
			}

			[[nodiscard]] bool CorruptDerivedArtifact(std::string& outError) const
			{
				return WriteText(DerivedPath(), "this is intentionally not a derived-scene envelope", outError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const
			{
				return m_Root;
			}

			[[nodiscard]] std::filesystem::path SourcePath() const
			{
				return m_Root / "Assets" / "Scenes" / "Scene.tescene";
			}

			[[nodiscard]] std::filesystem::path DerivedPath() const
			{
				// ProjectManager derives this deterministic location from the source
				// relative to Assets, then appends .bin without changing source bytes.
				return m_Root / "Library" / "DerivedScenes" / "Scenes" / "Scene.tescene.bin";
			}

		private:
			static void SetPreferencesPath(const std::string& path)
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str());
#else
				setenv("TERRAENGINE_EDITOR_PREFS_PATH", path.c_str(), 1);
#endif
			}

			static void ClearPreferencesPath()
			{
#ifdef _WIN32
				_putenv_s("TERRAENGINE_EDITOR_PREFS_PATH", "");
#else
				unsetenv("TERRAENGINE_EDITOR_PREFS_PATH");
#endif
			}

			[[nodiscard]] bool WriteScene(std::size_t objectCount, std::string& outError) const
			{
				std::ofstream output(SourcePath(), std::ios::binary | std::ios::trunc);
				if (!output)
				{
					outError = "Could not create the disposable authored scene.";
					return false;
				}

				// This intentionally uses only the minimum accepted authored fields.
				// It makes the 10,000-object timing input deterministic and keeps the
				// gate focused on project readiness instead of renderer or physics work.
				output << "{\"name\":\"Readiness Fixture\",\"objects\":[";
				for (std::size_t index = 0U; index < objectCount; ++index)
				{
					if (index != 0U)
					{
						output << ',';
					}
					output << "{\"name\":\"Object-" << index << "\",\"components\":{},\"children\":[]}";
				}
				output << "]}";
				output.flush();
				if (!output)
				{
					outError = "Could not finish writing the disposable authored scene.";
					return false;
				}
				return true;
			}

			[[nodiscard]] static bool WriteText(const std::filesystem::path& path, const std::string& text, std::string& outError)
			{
				std::error_code error;
				std::filesystem::create_directories(path.parent_path(), error);
				if (error)
				{
					outError = "Could not create a disposable artifact directory: " + error.message();
					return false;
				}
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				if (!output)
				{
					outError = "Could not write a disposable readiness fixture file.";
					return false;
				}
				output << text;
				output.flush();
				if (!output)
				{
					outError = "Could not flush a disposable readiness fixture file.";
					return false;
				}
				return true;
			}

			std::filesystem::path m_Root;
			std::filesystem::path m_PreferencesPath;
			std::optional<std::string> m_PreviousPreferencesPath;
		};

		[[nodiscard]] bool LoadAndPrepare(ProjectManager& manager, const ProjectReadinessFixture& fixture, ProjectSceneLoadDecision& decision, std::string& outError)
		{
			if (!manager.LoadProject(fixture.Root(), &outError))
			{
				return false;
			}
			return manager.PrepareSceneForLoading("Scenes/Scene.tescene", decision, &outError);
		}
	}

	void RegisterProjectReadinessPerformanceTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProjectReadiness.ColdSourceFallbackRegeneratesDisposableDerivedArtifact", [](TestContext& context) {
			ProjectReadinessFixture fixture("cold");
			std::string error;
			context.Expect(fixture.CreateProject(3U, error), error);
			context.Expect(!std::filesystem::exists(fixture.DerivedPath()), "A cold fixture must begin without a derived scene artifact.");

			ProjectManager manager;
			ProjectSceneLoadDecision decision;
			context.Expect(LoadAndPrepare(manager, fixture, decision, error), error);
			context.Expect(decision.prepared && decision.usedAuthoritativeSource && !decision.cacheHit,
				"A missing derived artifact must load the authoritative source rather than invent a cache hit.");
			context.Expect(decision.regenerationPublished && std::filesystem::exists(fixture.DerivedPath()),
				"A successfully prepared source scene should atomically regenerate its disposable derived artifact.");
			context.ExpectEqual(std::uint64_t(3U), decision.preparedScene.objectCount,
				"Cold preparation must preserve every authored object in the prepared packet.");
		});

		registry.Add("Editor.ProjectReadiness.WarmOpenReusesValidatedDerivedArtifact", [](TestContext& context) {
			ProjectReadinessFixture fixture("warm");
			std::string error;
			context.Expect(fixture.CreateProject(4U, error), error);

			ProjectManager manager;
			ProjectSceneLoadDecision cold;
			context.Expect(LoadAndPrepare(manager, fixture, cold, error), error);
			context.Expect(cold.regenerationPublished, "The warm fixture needs a valid derived artifact produced by the cold pass.");
			manager.CloseProject();

			ProjectSceneLoadDecision warm;
			context.Expect(LoadAndPrepare(manager, fixture, warm, error), error);
			context.Expect(warm.prepared && warm.cacheHit && !warm.usedAuthoritativeSource,
				"An unchanged reopen must reuse the checksummed derived artifact instead of decoding source again.");
			context.ExpectEqual(std::uint64_t(4U), warm.preparedScene.objectCount,
				"Warm preparation must retain the complete source object count.");
		});

		registry.Add("Editor.ProjectReadiness.StaleSourceMissesAndRebuildsDerivedArtifact", [](TestContext& context) {
			ProjectReadinessFixture fixture("stale");
			std::string error;
			context.Expect(fixture.CreateProject(1U, error), error);

			ProjectManager manager;
			ProjectSceneLoadDecision cold;
			context.Expect(LoadAndPrepare(manager, fixture, cold, error), error);
			context.Expect(fixture.ReplaceSource(2U, error), error);

			ProjectSceneLoadDecision stale;
			context.Expect(manager.PrepareSceneForLoading("Scenes/Scene.tescene", stale, &error), error);
			context.Expect(stale.prepared && stale.usedAuthoritativeSource && !stale.cacheHit,
				"Changing authored bytes must reject the old derived artifact and use the authoritative source.");
			context.Expect(stale.regenerationPublished,
				"A stale source miss should replace the disposable derived artifact only after source preparation succeeds.");
			context.ExpectEqual(std::uint64_t(2U), stale.preparedScene.objectCount,
				"The stale fallback must prepare the changed source rather than retain cached object records.");

			ProjectSceneLoadDecision rebuiltWarm;
			context.Expect(manager.PrepareSceneForLoading("Scenes/Scene.tescene", rebuiltWarm, &error), error);
			context.Expect(rebuiltWarm.cacheHit,
				"The regenerated artifact must be valid for the unchanged source on the next preparation pass.");
		});

		registry.Add("Editor.ProjectReadiness.CorruptArtifactFallsBackAndRecovers", [](TestContext& context) {
			ProjectReadinessFixture fixture("corrupt");
			std::string error;
			context.Expect(fixture.CreateProject(2U, error), error);

			ProjectManager manager;
			ProjectSceneLoadDecision cold;
			context.Expect(LoadAndPrepare(manager, fixture, cold, error), error);
			context.Expect(fixture.CorruptDerivedArtifact(error), error);

			ProjectSceneLoadDecision recovered;
			context.Expect(manager.PrepareSceneForLoading("Scenes/Scene.tescene", recovered, &error), error);
			context.Expect(recovered.prepared && recovered.usedAuthoritativeSource && !recovered.cacheHit,
				"A malformed derived file must never become scene authority or a false warm hit.");
			context.Expect(recovered.regenerationPublished,
				"Recovery must atomically replace the corrupt disposable artifact after source preparation succeeds.");

			ProjectSceneLoadDecision warm;
			context.Expect(manager.PrepareSceneForLoading("Scenes/Scene.tescene", warm, &error), error);
			context.Expect(warm.cacheHit && warm.prepared,
				"The artifact written during corrupt-cache recovery must be usable on the following warm pass.");
		});

		registry.Add("Editor.ProjectReadiness.ProjectCloseInvalidatesPreparedGeneration", [](TestContext& context) {
			ProjectReadinessFixture fixture("close");
			std::string error;
			context.Expect(fixture.CreateProject(32U, error), error);

			ProjectManager manager;
			ProjectSceneLoadDecision prepared;
			context.Expect(LoadAndPrepare(manager, fixture, prepared, error), error);
			context.Expect(manager.IsProjectLoadGenerationCurrent(prepared.projectGeneration),
				"A prepared packet must begin with the active project generation.");

			manager.CloseProject();
			context.Expect(!manager.HasActiveProject(), "Closing a project must remove its active-project state.");
			context.Expect(!manager.IsProjectLoadGenerationCurrent(prepared.projectGeneration),
				"Project close must invalidate the generation captured by a prepared packet before it can publish.");
			context.Expect(manager.GetReadinessState().projectGeneration != prepared.projectGeneration,
				"Readiness state must advance with close so a late worker result cannot be mistaken for the new project.");

			ProjectSceneLoadDecision afterClose;
			context.Expect(!manager.PrepareSceneForLoading("Scenes/Scene.tescene", afterClose, &error),
				"No synchronous scene preparation may begin after close; worker callers use the invalidated generation to cancel late results.");
		});

		registry.Add("Editor.ProjectReadiness.LargeSceneColdWarmTimingGate", [](TestContext& context) {
			constexpr std::size_t kLargeSceneObjectCount = 10000U;
			// These fixed limits intentionally avoid percentile sampling and machine-
			// relative comparisons. Cold is a generous stall detector (10 seconds),
			// while the 2.5-second warm limit is the US4 acceptance requirement from
			// quickstart.md. The fixture uses only deterministic JSON/decode/cache I/O.
			constexpr auto kColdReadinessMaximum = std::chrono::seconds(10);
			constexpr auto kWarmReadinessMaximum = std::chrono::milliseconds(2500);

			ProjectReadinessFixture fixture("large_timing");
			std::string error;
			context.Expect(fixture.CreateProject(kLargeSceneObjectCount, error), error);

			ProjectManager manager;
			ProjectSceneLoadDecision cold;
			const auto coldStart = std::chrono::steady_clock::now();
			context.Expect(LoadAndPrepare(manager, fixture, cold, error), error);
			const auto coldElapsed = std::chrono::steady_clock::now() - coldStart;
			context.Expect(cold.prepared && cold.usedAuthoritativeSource && cold.regenerationPublished,
				"The large cold pass must prepare authoritative content and publish a validated disposable cache.");
			context.ExpectEqual(static_cast<std::uint64_t>(kLargeSceneObjectCount), cold.preparedScene.objectCount,
				"The large cold pass may not reduce the deterministic ten-thousand-object workload.");
			context.Expect(coldElapsed <= kColdReadinessMaximum,
				"Cold large-scene readiness exceeded the documented ten-second pathological-stall guard.");

			manager.CloseProject();
			ProjectSceneLoadDecision warm;
			const auto warmStart = std::chrono::steady_clock::now();
			context.Expect(LoadAndPrepare(manager, fixture, warm, error), error);
			const auto warmElapsed = std::chrono::steady_clock::now() - warmStart;
			context.Expect(warm.prepared && warm.cacheHit && !warm.usedAuthoritativeSource,
				"The unchanged large-scene warm pass must reuse derived data rather than fall back to source.");
			context.ExpectEqual(static_cast<std::uint64_t>(kLargeSceneObjectCount), warm.preparedScene.objectCount,
				"Warm readiness must retain the same complete deterministic scene content as cold readiness.");
			context.Expect(warmElapsed <= kWarmReadinessMaximum,
				"Warm large-scene readiness exceeded the documented 2.5-second US4 acceptance limit.");
		});
	}
}
