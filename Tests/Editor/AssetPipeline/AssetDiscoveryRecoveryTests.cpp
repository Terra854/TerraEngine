// Owned notification bytes exercise the platform pump's production decoder.
// Recovery must discard the entire unsafe batch and consume demand only once.
#include "TestHarness.h"
#include "FileWatcher.h"
#include "AssetDatabase.h"

#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <atomic>
#include <algorithm>
#include <map>
#include <thread>

namespace TerraEngine::EditorTests {
	namespace {
		struct ScanGate {
			std::mutex mutex; std::condition_variable changed; bool entered{}, released{};
			void Hold() { std::unique_lock lock(mutex); entered = true; changed.notify_all(); changed.wait(lock, [&] { return released; }); }
			bool Wait() { std::unique_lock lock(mutex); return changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }); }
			void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
		};
		struct RecoveryFixture {
			std::filesystem::path root = std::filesystem::temp_directory_path() / ("TerraDiscoveryRecovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::shared_ptr<ScanGate> gate = std::make_shared<ScanGate>();
			RecoveryFixture() { AssetDatabase::Instance().Shutdown(); std::filesystem::create_directories(root / "Assets"); }
			~RecoveryFixture() { gate->Release(); auto& database = AssetDatabase::Instance(); database.SetProjectReconcileHookForTests({}); database.Shutdown(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
			std::filesystem::path Source(std::string name) const { return root / "Assets" / name; }
			void Write(std::string name, std::string value = "authored") const { std::ofstream(Source(name), std::ios::binary | std::ios::trunc) << value; }
			void Open() { auto& database = AssetDatabase::Instance(); database.Initialize(root); database.PauseWatcherForTests(); }
		};
		struct PersistenceProof { std::uint64_t updates{}, saves{}, maximumPerUpdate{}; };
		bool ConvergeWithPersistenceProof(AssetDatabase& database, PersistenceProof* proof)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			do {
				const auto before = database.GetAssetIndexSaveAttemptsForTests();
				database.Update(); const auto state = database.GetDiscoveryDiagnostics();
				if (proof) {
					const auto attempts = database.GetAssetIndexSaveAttemptsForTests() - before;
					++proof->updates; proof->saves += attempts;
					proof->maximumPerUpdate = (std::max)(proof->maximumPerUpdate, attempts);
				}
				if (!state.pending && !state.scanning && state.pendingImports == 0U) return true;
				std::this_thread::yield();
			} while (std::chrono::steady_clock::now() < deadline);
			return false;
		}
		bool Converge(AssetDatabase& database) { return ConvergeWithPersistenceProof(database, nullptr); }
		std::vector<std::byte> Notification(std::u16string name, std::uint32_t next = 0U)
		{
			std::vector<std::byte> bytes(12U + name.size() * 2U);
			const std::uint32_t action = 1U, nameBytes = static_cast<std::uint32_t>(name.size() * 2U);
			std::memcpy(bytes.data(), &next, 4U);
			std::memcpy(bytes.data() + 4U, &action, 4U);
			std::memcpy(bytes.data() + 8U, &nameBytes, 4U);
			std::memcpy(bytes.data() + 12U, name.data(), nameBytes);
			return bytes;
		}

		void ExpectRecovery(TestContext& context, FileWatcher& watcher)
		{
			context.Expect(watcher.PopEvents().empty(), "Unsafe completion must publish no trusted partial events.");
			context.Expect(watcher.ConsumeReconcileRequired(), "Lost notification information must request authoritative recovery.");
			context.Expect(!watcher.ConsumeReconcileRequired(), "One recovery flag must be consumed once.");
		}
	}

	void RegisterAssetDiscoveryRecoveryTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetDiscoveryRecovery.ZeroCompletion", [](TestContext& context) {
			FileWatcher watcher;
			watcher.ProcessWindowsCompletionForTests({}, true);
			ExpectRecovery(context, watcher);
		});
		registry.Add("Editor.AssetDiscoveryRecovery.FailedRequest", [](TestContext& context) {
			FileWatcher watcher; const auto stale = Notification(u"stale.txt");
			watcher.ProcessWindowsCompletionForTests(stale, false);
			ExpectRecovery(context, watcher);
		});
		registry.Add("Editor.AssetDiscoveryRecovery.MalformedRecordBounds", [](TestContext& context) {
			// All variants stay inside owned storage: short header/name, overlapping
			// or unaligned offset, and a later malformed record after a valid one.
			std::vector<std::vector<std::byte>> variants;
			variants.emplace_back(11U);
			auto truncated = Notification(u"long-name.txt"); truncated.pop_back(); variants.push_back(std::move(truncated));
			variants.push_back(Notification(u"x", 4U));
			variants.push_back(Notification(u"x", 15U));
			auto later = Notification(u"ok", 16U); later.resize(20U); variants.push_back(std::move(later));
			for (const auto& bytes : variants) {
				FileWatcher watcher; watcher.ProcessWindowsCompletionForTests(bytes, true); ExpectRecovery(context, watcher);
			}
		});
		registry.Add("Editor.AssetDiscoveryRecovery.InvalidUtf16", [](TestContext& context) {
			for (const auto name : {std::u16string{static_cast<char16_t>(0xD800U)}, std::u16string{static_cast<char16_t>(0xDC00U)}, std::u16string{u'x', u'\0', u'y'}}) {
				FileWatcher watcher; const auto bytes = Notification(name);
				watcher.ProcessWindowsCompletionForTests(bytes, true); ExpectRecovery(context, watcher);
			}
			FileWatcher valid; const auto paired = Notification(std::u16string{u'x', static_cast<char16_t>(0xD83DU), static_cast<char16_t>(0xDE00U)});
			valid.ProcessWindowsCompletionForTests(paired, true);
			context.Expect(valid.PopEvents().size() == 1U && !valid.ConsumeReconcileRequired(), "A complete surrogate pair must retain its usable notification.");
		});
		registry.Add("Editor.AssetDiscoveryRecovery.LinuxOverflow", [](TestContext& context) {
			FileWatcher watcher; watcher.ProcessLinuxLossForTests(true, 0x00004000U);
			ExpectRecovery(context, watcher);
			watcher.ProcessLinuxLossForTests(false, 0U); ExpectRecovery(context, watcher);
		});
		registry.Add("Editor.AssetDiscoveryRecovery.ThousandMixedChanges", [](TestContext& context) {
			RecoveryFixture fixture; auto& database = AssetDatabase::Instance();
			for (unsigned index = 0U; index < 750U; ++index) fixture.Write("seed" + std::to_string(index) + ".txt");
			fixture.Open(); context.Expect(Converge(database), "Initial production discovery must finish before notification loss.");
			const auto prior = database.GetAssets();
			for (unsigned index = 0U; index < 250U; ++index) {
				fixture.Write("created" + std::to_string(index) + ".txt");
				fixture.Write("seed" + std::to_string(index) + ".txt", "modified-value");
				const auto oldPath = fixture.Source("seed" + std::to_string(index + 250U) + ".txt");
				const auto newPath = fixture.Source("renamed" + std::to_string(index) + ".txt");
				std::filesystem::rename(oldPath, newPath); std::filesystem::rename(oldPath.string() + ".meta", newPath.string() + ".meta");
				std::filesystem::remove(fixture.Source("seed" + std::to_string(index + 500U) + ".txt"));
			}
			const auto malformed = Notification(u"bad", 4U); database.InjectWatcherCompletionForTests(malformed, true);
			PersistenceProof persistence;
			context.Expect(ConvergeWithPersistenceProof(database, &persistence), "Authoritative recovery must converge after one thousand real lost changes.");
			context.Expect(persistence.saves > 0U && persistence.maximumPerUpdate <= 1U && persistence.saves <= 100U,
				"Recovery must persist at most once per protected sixteen-record application, rather than once per changed record.");
			context.Expect(database.GetAssets().size() == 750U, "Recovered catalog must contain exactly the authoritative source count.");
			for (const auto& [path, metadata] : database.GetAssets()) {
				context.Expect(std::filesystem::is_regular_file(path), "No stale source may remain in the recovered catalog.");
				context.Expect(database.FindAssetByGuid(metadata.guid) == &metadata, "Recovered GUIDs must be unique and resolve their current path.");
			}
			for (unsigned index = 0U; index < 250U; ++index) {
				const auto oldPath = fixture.Source("seed" + std::to_string(index + 250U) + ".txt");
				context.Expect(database.GetAssets().at(fixture.Source("renamed" + std::to_string(index) + ".txt")).guid == prior.at(oldPath).guid,
					"Renamed sidecars must preserve their established GUID.");
				const auto modified = fixture.Source("seed" + std::to_string(index) + ".txt");
				context.Expect(database.GetAssets().at(modified).guid == prior.at(modified).guid, "Modified sources must preserve GUID metadata.");
			}
			// Read the real durable index after the protected flush. Every catalog
			// identity must agree exactly with its saved current source path.
			nlohmann::json index; { std::ifstream input(fixture.root / "Library" / "AssetIndex.json"); input >> index; }
			context.Expect(index.value("version", 0) == 1 && index.at("assets").size() == database.GetAssets().size(), "Durable recovered index must contain the complete catalog.");
			std::map<std::filesystem::path, std::string> durable;
			for (const auto& entry : index.at("assets")) durable.emplace(fixture.root / "Assets" / entry.at("sourcePath").get<std::string>(), entry.at("guid").get<std::string>());
			context.Expect(durable.size() == database.GetAssets().size(), "Durable index paths must be unique.");
			for (const auto& [path, metadata] : database.GetAssets())
				context.Expect(durable.at(path) == TerraEngine::ToHexString(metadata.guid), "Durable index GUID/path agreement must match the recovered catalog exactly.");
		});
		registry.Add("Editor.AssetDiscoveryRecovery.LossDuringScanAndFailureRetry", [](TestContext& context) {
			RecoveryFixture fixture; fixture.Write("first.txt"); fixture.Open(); auto& database = AssetDatabase::Instance();
			context.Expect(Converge(database), "Initial scan must settle.");
			database.SetProjectReconcileHookForTests([gate = fixture.gate](bool after) { if (after) gate->Hold(); });
			database.InjectWatcherCompletionForTests({}, true); database.Update();
			context.Expect(fixture.gate->Wait(), "Production scan must hold after its immutable source snapshot.");
			fixture.Write("during.txt"); database.InjectWatcherCompletionForTests({}, true); database.Update();
			const auto held = database.GetDiscoveryDiagnostics();
			context.Expect(held.requestedGeneration > held.scanningGeneration, "Loss during a scan must retain newer demand.");
			database.SetProjectReconcileHookForTests({}); fixture.gate->Release();
			context.Expect(Converge(database) && database.GetAssets().contains(fixture.Source("during.txt")), "A follow-up must include changes missed by the held snapshot.");
			const auto failures = database.GetDiscoveryDiagnostics().failedScans;
			auto failOnce = std::make_shared<std::atomic_bool>(true);
			database.SetProjectReconcileHookForTests([failOnce](bool after) { if (!after && failOnce->exchange(false)) throw std::runtime_error("Controlled scan failure"); });
			fixture.Write("retry.txt"); database.InjectWatcherCompletionForTests({}, true);
			context.Expect(Converge(database) && database.GetDiscoveryDiagnostics().failedScans == failures + 1U
				&& database.GetAssets().contains(fixture.Source("retry.txt")), "Failed scans must retain demand and recover on a subsequent scan.");
			// A nonempty directory at the index destination deterministically rejects
			// the real atomic writer. Its deferred demand must survive that boundary.
			const auto indexPath = fixture.root / "Library" / "AssetIndex.json";
			std::filesystem::rename(indexPath, fixture.root / "Library" / "prior-index.json");
			std::filesystem::create_directory(indexPath); std::ofstream(indexPath / "blocker") << "retain";
			database.SetProjectReconcileHookForTests({}); database.InjectWatcherCompletionForTests({}, true);
			context.Expect(Converge(database) && std::filesystem::is_directory(indexPath), "Disk-save rejection must not erase the completed live catalog or deferred persistence demand.");
			const auto attempts = database.GetAssetIndexSaveAttemptsForTests();
			std::filesystem::remove_all(indexPath); database.Update();
			context.Expect(database.GetAssetIndexSaveAttemptsForTests() == attempts + 1U && std::filesystem::is_regular_file(indexPath), "Next protected Update must retry a previously failed index save even without new file changes.");
			nlohmann::json recoveredIndex; { std::ifstream input(indexPath); input >> recoveredIndex; }
			context.Expect(recoveredIndex.at("assets").size() == database.GetAssets().size(), "Retried durable index must contain the complete retained catalog.");
		});
		registry.Add("Editor.AssetDiscoveryRecovery.RetiredProjectScanRejected", [](TestContext& context) {
			RecoveryFixture fixture; fixture.Write("old.txt"); fixture.Open(); auto& database = AssetDatabase::Instance();
			context.Expect(Converge(database), "Initial project scan must settle.");
			database.SetProjectReconcileHookForTests([gate = fixture.gate](bool after) { if (after) gate->Hold(); });
			database.InjectWatcherCompletionForTests({}, true); database.Update(); context.Expect(fixture.gate->Wait(), "Old scan must retain physical snapshot ownership.");
			const auto retired = database.GetDiscoveryDiagnostics().retiredScans;
			database.SetProjectReconcileHookForTests({}); fixture.gate->Release();
			const auto newer = fixture.root / "new-owner"; std::filesystem::create_directories(newer / "Assets"); std::ofstream(newer / "Assets" / "new.txt") << "new";
			database.Initialize(newer); database.PauseWatcherForTests();
			context.Expect(Converge(database) && database.GetAssets().size() == 1U && database.GetAssets().contains(newer / "Assets" / "new.txt")
				&& database.GetDiscoveryDiagnostics().retiredScans == retired + 1U, "Drained retired snapshots must never repopulate the replacement project catalog.");
		});
	}
}
