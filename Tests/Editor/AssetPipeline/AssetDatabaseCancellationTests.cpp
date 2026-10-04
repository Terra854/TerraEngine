// Regression coverage for the AssetDatabase worker cancellation lifecycle.
// These tests hold a reimport inside a test-only worker hook so shutdown and
// cancellation always happen while the asynchronous job is genuinely active.
#include "TestHarness.h"

#include "AssetDatabase.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		struct BlockedImportGate
		{
			std::mutex mutex;
			std::condition_variable condition;
			bool workerEntered = false;
			bool allowCompletion = false;
		};

		const std::uint64_t gCancellationFixtureNonce = static_cast<std::uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		std::atomic_uint64_t gCancellationFixtureSequence = 0u;

		std::string ReadFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}

		class AssetDatabaseCancellationFixture
		{
		public:
			AssetDatabaseCancellationFixture()
				: root(std::filesystem::temp_directory_path()
					/ ("TerraEngineAssetDatabaseCancellation_"
						+ std::to_string(gCancellationFixtureNonce) + "_"
						+ std::to_string(gCancellationFixtureSequence.fetch_add(1u)))),
				  sourcePath(root / "Assets" / "Colliders" / "Startup.tecollider"),
				  metadataPath(sourcePath.string() + ".meta"),
				  compiledPath(root / "Build" / "Assets" / "Colliders" / "Startup.tepcoll"),
				  assetGuid(TerraEngine::GenerateUUID()),
				  gate(std::make_shared<BlockedImportGate>())
			{
				// The database is a process-wide singleton. Start every test from a
				// fully drained state before assigning it a disposable project.
				AssetDatabase::Instance().Shutdown();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(sourcePath.parent_path());
				std::filesystem::create_directories(compiledPath.parent_path());

				std::ofstream(sourcePath, std::ios::binary | std::ios::trunc) << "authored-collider-recipe";
				std::ofstream(compiledPath, std::ios::binary | std::ios::trunc) << kLastKnownGoodBytes;
				WriteMetadata();

				AssetDatabase::Instance().Initialize(root);
				// A synchronous unchanged import registers the existing GUID and
				// published collider without launching the compiler.
				AssetDatabase::Instance().ImportAsset(sourcePath, false);
			}

			~AssetDatabaseCancellationFixture()
			{
				ReleaseWorker();
				// If an assertion interrupted a test, cooperative cancellation makes
				// the fixture teardown bounded before the hook's captured state dies.
				AssetDatabase::Instance().CancelAllImportsAndWait(false);
				AssetDatabase::Instance().SetAsyncImportCompileHookForTests({});
				AssetDatabase::Instance().Shutdown();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			void StartBlockedReimport(bool retainAfterCancellation = false)
			{
				const std::shared_ptr<BlockedImportGate> sharedGate = gate;
				AssetDatabase::Instance().SetAsyncImportCompileHookForTests(
					[sharedGate, retainAfterCancellation](const AssetMetadata&, const std::atomic_bool& cancellationRequested) {
						std::unique_lock<std::mutex> lock(sharedGate->mutex);
						sharedGate->workerEntered = true;
						sharedGate->condition.notify_all();
						// Cancellation owns a separate atomic token, so use a short timed
						// wait to observe it without needing a production callback into the test.
						while (!sharedGate->allowCompletion && (retainAfterCancellation || !cancellationRequested.load())) {
							sharedGate->condition.wait_for(lock, std::chrono::milliseconds(5));
						}
						return !cancellationRequested.load();
					});

				AssetDatabase::Instance().ImportAssetWithProgress(sourcePath, true);
			}

			bool WaitForWorker() const
			{
				std::unique_lock<std::mutex> lock(gate->mutex);
				return gate->condition.wait_for(lock, std::chrono::seconds(3), [this]() {
					return gate->workerEntered;
				});
			}

			void ReleaseWorker() const
			{
				{
					std::lock_guard<std::mutex> lock(gate->mutex);
					gate->allowCompletion = true;
				}
				gate->condition.notify_all();
			}

			void ExpectPublishedStateSurvives(TestContext& context) const
			{
				context.Expect(std::filesystem::exists(sourcePath), "Authored collider source must survive non-destructive worker termination.");
				context.Expect(std::filesystem::exists(metadataPath), "Stable collider metadata must survive non-destructive worker termination.");
				context.Expect(std::filesystem::exists(compiledPath), "Last-known-good collider output must survive non-destructive worker termination.");
				context.ExpectEqual(std::string(kLastKnownGoodBytes), ReadFile(compiledPath), "Cancellation must not replace or remove the last-known-good collider bytes.");
			}

			void ExpectRegistrationSurvives(TestContext& context) const
			{
				const AssetMetadata* registered = AssetDatabase::Instance().FindAssetByGuid(assetGuid);
				context.Expect(registered != nullptr, "Non-destructive cancellation must retain the live asset registration.");
				if (registered != nullptr) {
					context.ExpectEqual(std::filesystem::path("Colliders/Startup.tepcoll"), registered->compiledPath, "Retained registration must still resolve the published collider output.");
				}
			}

			std::filesystem::path root;
			std::filesystem::path sourcePath;
			std::filesystem::path metadataPath;
			std::filesystem::path compiledPath;
			TerraEngine::UUID assetGuid;

		private:
			void WriteMetadata() const
			{
				nlohmann::json metadata;
				metadata["guid"] = TerraEngine::ToHexString(assetGuid);
				metadata["importerName"] = "PhysicsColliderRecipeImporter";
				metadata["lastModified"] = std::filesystem::last_write_time(sourcePath).time_since_epoch().count();
				metadata["sourcePath"] = "Colliders/Startup.tecollider";
				metadata["compiledPath"] = "Colliders/Startup.tepcoll";
				metadata["settings"] = nlohmann::json::object();

				std::ofstream output(metadataPath, std::ios::trunc);
				output << metadata.dump(4);
			}

			static constexpr std::string_view kLastKnownGoodBytes = "last-known-good-collider";
			std::shared_ptr<BlockedImportGate> gate;
		};
	}

	void RegisterAssetDatabaseCancellationTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.AssetDatabaseCancellation.CanceledImportRetainsPhysicalOwnership", [](TestContext& context) {
			AssetDatabaseCancellationFixture fixture;
			fixture.StartBlockedReimport(true);
			context.Expect(fixture.WaitForWorker(), "Cancellation ownership requires a real held production import callback.");
			context.Expect(AssetDatabase::Instance().CancelImport(fixture.sourcePath, false), "Active cancellation must revoke pending publication.");
			context.Expect(AssetDatabase::Instance().GetCompileJobsCount() == 1U, "Logical cancellation must retain its unsettled physical job.");
			auto drain = std::async(std::launch::async, [] { AssetDatabase::Instance().CancelAllImportsAndWait(false); });
			const bool retained = drain.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout;
			fixture.ReleaseWorker(); drain.get();
			context.Expect(retained && AssetDatabase::Instance().GetCompileJobsCount() == 0U, "Drain must wait until the held worker actually exits, then release its ownership.");
			fixture.ExpectPublishedStateSurvives(context);
			fixture.ExpectRegistrationSurvives(context);
		});
		registry.Add("Editor.AssetPipeline.AssetDatabaseCancellation.ShutdownDrainsActiveImport", [](TestContext& context) {
			AssetDatabaseCancellationFixture fixture;
			fixture.StartBlockedReimport();
			context.Expect(fixture.WaitForWorker(), "Shutdown regression requires an active asynchronous import worker.");

			std::future<void> shutdown = std::async(std::launch::async, []() {
				AssetDatabase::Instance().Shutdown();
			});
			fixture.ReleaseWorker();
			context.Expect(shutdown.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Shutdown must drain a successful active import without hanging.");
			shutdown.get();

			fixture.ExpectPublishedStateSurvives(context);
			// Reopen the disposable project to prove shutdown left enough durable
			// state for startup registration, not merely three orphaned files.
			AssetDatabase::Instance().Initialize(fixture.root);
			AssetDatabase::Instance().ImportAsset(fixture.sourcePath, false);
			fixture.ExpectRegistrationSurvives(context);
		});

		registry.Add("Editor.AssetPipeline.AssetDatabaseCancellation.NonDestructiveCancelIsIdempotent", [](TestContext& context) {
			AssetDatabaseCancellationFixture fixture;
			fixture.StartBlockedReimport();
			context.Expect(fixture.WaitForWorker(), "Cancellation regression requires an active asynchronous import worker.");
			context.Expect(AssetDatabase::Instance().CancelImport(fixture.sourcePath, false), "Non-destructive cancellation should accept an active import.");

			AssetDatabase::Instance().CancelAllImportsAndWait(false);
			// A second drain exercises the repeated cleanup path seen in the
			// startup logs and must remain a no-op for durable asset state.
			AssetDatabase::Instance().CancelAllImportsAndWait(false);
			fixture.ExpectPublishedStateSurvives(context);
			fixture.ExpectRegistrationSurvives(context);
		});

		registry.Add("Editor.AssetPipeline.AssetDatabaseCancellation.ExplicitDiscardRemainsDestructive", [](TestContext& context) {
			AssetDatabaseCancellationFixture fixture;
			fixture.StartBlockedReimport();
			context.Expect(fixture.WaitForWorker(), "Destructive cancellation regression requires an active asynchronous import worker.");
			context.Expect(AssetDatabase::Instance().CancelImport(fixture.sourcePath, true), "Explicit discard should accept an active import.");
			AssetDatabase::Instance().CancelAllImportsAndWait(true);

			context.Expect(!std::filesystem::exists(fixture.sourcePath), "Explicit discard must remove the authored source selected by the user.");
			context.Expect(!std::filesystem::exists(fixture.metadataPath), "Explicit discard must remove metadata for the discarded asset.");
			context.Expect(!std::filesystem::exists(fixture.compiledPath), "Explicit discard must remove the compiled output for the discarded asset.");
			context.Expect(AssetDatabase::Instance().FindAssetByGuid(fixture.assetGuid) == nullptr, "Explicit discard must unregister the discarded asset.");
		});
	}
}
