// Real sidecars and production import dispatch expose worker admission order,
// retained cancellation ownership, and independence from frame worker capacity.
#include "TestHarness.h"
#include "AssetDatabase.h"
#include "AssetImportResult.h"
#include "Editor/AssetCompilers/AudioClipCompiler.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <Windows.h>
#else
#include <signal.h>
#endif

namespace TerraEngine::EditorTests {
	namespace {
		// Before the admission API lands, the same source still compiles and drives
		// the legacy dispatcher. Its observed excess workers provide the red proof.
		template<class Database> bool ConfigureLimit(Database& database, std::size_t limit)
		{
			if constexpr (requires { database.SetMaxActiveImports(limit); }) return database.SetMaxActiveImports(limit);
			else { (void)database; (void)limit; return true; }
		}
		struct WorkerGate {
			std::mutex mutex; std::condition_variable changed;
			std::vector<AssetMetadata> started; unsigned active{}, peak{}; bool released{}, retainCanceled{};
			bool Compile(const AssetMetadata& metadata, const std::atomic_bool& canceled)
			{
				std::unique_lock lock(mutex); started.push_back(metadata); ++active; peak = (std::max)(peak, active); changed.notify_all();
				while (!released && (retainCanceled || !canceled.load())) changed.wait_for(lock, std::chrono::milliseconds(5));
				--active; changed.notify_all(); return !canceled.load();
			}
			bool Wait(unsigned count) { std::unique_lock lock(mutex); return changed.wait_for(lock, std::chrono::seconds(10), [&] { return started.size() >= count; }); }
			void Release() { std::lock_guard lock(mutex); released = true; changed.notify_all(); }
			std::vector<AssetMetadata> Entries() { std::lock_guard lock(mutex); return started; }
			unsigned Peak() { std::lock_guard lock(mutex); return peak; }
		};
		struct AdmissionFixture {
			std::filesystem::path root = std::filesystem::temp_directory_path() / ("TerraImportAdmission-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::shared_ptr<WorkerGate> gate = std::make_shared<WorkerGate>();
			std::vector<TerraEngine::UUID> ids;
			AdmissionFixture(std::size_t limit, unsigned sources)
			{
				auto& database = AssetDatabase::Instance(); database.Shutdown();
				if (!ConfigureLimit(database, limit)) throw std::runtime_error("Quiescent import capacity configuration failed.");
				std::filesystem::create_directories(root / "Assets"); std::filesystem::create_directories(root / "Build" / "Assets");
				for (unsigned index = 0U; index < sources; ++index) {
					ids.push_back(TerraEngine::GenerateUUID()); std::ofstream(Source(index)) << "authored-collider";
					std::ofstream(root / "Build" / "Assets" / ("source" + std::to_string(index) + ".tepcoll")) << "last-good";
					WriteMetadata(index, 0U);
				}
				database.Initialize(root); database.PauseWatcherForTests();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
				for (;;) {
					database.Update(); const auto discovery = database.GetDiscoveryDiagnostics();
					if (!discovery.pending && !discovery.scanning && database.GetCompileJobsCount() == 0U) break;
					if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Initial fixture discovery did not settle.");
					std::this_thread::yield();
				}
				database.SetAsyncImportCompileHookForTests([owned = gate](const AssetMetadata& metadata, const std::atomic_bool& canceled) { return owned->Compile(metadata, canceled); });
			}
			~AdmissionFixture()
			{
				gate->Release(); auto& database = AssetDatabase::Instance(); database.SetImportAdmissionHookForTests({}); database.CancelAllImportsAndWait(false);
				database.SetAsyncImportCompileHookForTests({}); database.Shutdown(); (void)ConfigureLimit(database, 4U);
				std::error_code ignored; std::filesystem::remove_all(root, ignored);
			}
			std::filesystem::path Source(unsigned index) const { return root / "Assets" / ("source" + std::to_string(index) + ".tecollider"); }
			void WriteMetadata(unsigned index, unsigned revision) const
			{
				nlohmann::json metadata{{"guid", TerraEngine::ToHexString(ids.at(index))}, {"importerName", "PhysicsColliderRecipeImporter"},
					{"lastModified", std::filesystem::last_write_time(Source(index)).time_since_epoch().count()},
					{"sourcePath", Source(index).filename().string()}, {"compiledPath", "source" + std::to_string(index) + ".tepcoll"},
					{"settings", {{"revision", revision}}}};
				std::ofstream(Source(index).string() + ".meta", std::ios::trunc) << metadata.dump();
			}
			void Import(unsigned index) { AssetDatabase::Instance().ImportAssetWithProgress(Source(index), true); }
			bool Settle()
			{
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
				do { AssetDatabase::Instance().Update(); if (AssetDatabase::Instance().GetCompileJobsCount() == 0U) return true; std::this_thread::yield(); }
				while (std::chrono::steady_clock::now() < deadline);
				return false;
			}
		};
		std::string Quote(const std::filesystem::path& path) { return "\"" + path.string() + "\""; }
		bool WaitForFile(const std::filesystem::path& path)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (std::chrono::steady_clock::now() < deadline) { if (std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) != 0U) return true; std::this_thread::yield(); }
			return false;
		}
	}

	void RegisterAssetImportAdmissionTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetImportAdmission.LimitOne", [](TestContext& context) {
			AdmissionFixture fixture(1U, 100U); for (unsigned index = 0U; index < 100U; ++index) fixture.Import(index);
			context.Expect(fixture.gate->Wait(1U), "At least one real compiler callback must enter."); fixture.gate->Release();
			context.Expect(!ConfigureLimit(AssetDatabase::Instance(), 0U) && !ConfigureLimit(AssetDatabase::Instance(), 2U),
				"Zero capacity and active reconfiguration must be rejected without canceling owned requests.");
			context.Expect(fixture.Settle() && fixture.gate->Entries().size() == 100U && fixture.gate->Peak() == 1U, "One configured slot must process all one hundred sources with peak one.");
		});
		registry.Add("Editor.AssetImportAdmission.LimitFourWithHundredSources", [](TestContext& context) {
			AdmissionFixture fixture(4U, 100U); for (unsigned index = 0U; index < 100U; ++index) fixture.Import(index);
			context.Expect(fixture.gate->Wait(4U), "Four independent compiler callbacks must enter concurrently.");
			const auto admission = AssetDatabase::Instance().GetImportAdmissionDiagnostics();
			context.Expect(admission.configuredCapacity == 4U && admission.active == 4U && admission.queued == 96U,
				"Default capacity and every queued source must remain visible before execution.");
			fixture.gate->Release();
			context.Expect(fixture.Settle() && fixture.gate->Entries().size() == 100U && fixture.gate->Peak() == 4U, "Four slots must process all one hundred sources without exceeding capacity.");
		});
		registry.Add("Editor.AssetImportAdmission.DistinctFifo", [](TestContext& context) {
			AdmissionFixture fixture(1U, 12U); fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "First compiler must hold the sole slot.");
			for (unsigned index = 1U; index < 12U; ++index) fixture.Import(index); fixture.gate->Release(); context.Expect(fixture.Settle(), "All queued distinct imports must finish.");
			const auto entries = fixture.gate->Entries(); context.Expect(entries.size() == 12U, "Distinct imports execute once.");
			for (unsigned index = 0U; index < entries.size(); ++index) context.Expect(entries[index].guid == fixture.ids[index], "Eligible distinct sources must retain FIFO admission order.");
			context.Expect(fixture.gate->Peak() == 1U, "FIFO callbacks must use the configured sole slot.");
		});
		registry.Add("Editor.AssetImportAdmission.QueuedDuplicateRefresh", [](TestContext& context) {
			AdmissionFixture fixture(1U, 2U); fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "An earlier source must occupy capacity.");
			fixture.Import(1U); fixture.WriteMetadata(1U, 7U); fixture.Import(1U); fixture.gate->Release(); context.Expect(fixture.Settle(), "Queued metadata refresh must settle.");
			unsigned matches = 0U; for (const auto& entry : fixture.gate->Entries()) if (entry.guid == fixture.ids[1]) { ++matches; context.Expect(entry.settings.value("revision", 0U) == 7U, "Queued work must use the refreshed sidecar settings."); }
			context.Expect(matches == 1U, "Queued duplicate edits must update one request without launching a stale compiler.");
		});
		registry.Add("Editor.AssetImportAdmission.ActiveDuplicateFollowup", [](TestContext& context) {
			AdmissionFixture fixture(1U, 1U); fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "The first source generation must be active.");
			fixture.WriteMetadata(0U, 3U); fixture.Import(0U); fixture.WriteMetadata(0U, 9U); fixture.Import(0U);
			fixture.gate->Release(); context.Expect(fixture.Settle(), "Active latest-source follow-up must finish.");
			const auto entries = fixture.gate->Entries(); context.Expect(entries.size() == 2U && entries.back().settings.value("revision", 0U) == 9U, "Active duplicates must coalesce exactly one refreshed follow-up.");
		});
		registry.Add("Editor.AssetImportAdmission.CanceledSourceRetryWaitsForSettlement", [](TestContext& context) {
			AdmissionFixture fixture(4U, 1U);
			// A canceled compiler deliberately retains the GUID-stable staging file.
			// Spare executor slots must not let its replacement use that file early.
			fixture.gate->retainCanceled = true;
			const auto staged = fixture.root / "Build" / "Assets" / "shared.tmp";
			const auto cooked = fixture.root / "Build" / "Assets" / "source0.tepcoll";
			auto& database = AssetDatabase::Instance();
			database.SetAsyncImportCompileHookForTests([gate = fixture.gate, staged, cooked](const AssetMetadata& metadata, const std::atomic_bool& canceled) {
				std::ofstream(staged, std::ios::trunc) << metadata.settings.value("revision", 0U);
				const bool succeeded = gate->Compile(metadata, canceled);
				std::error_code ignored; std::filesystem::remove(staged, ignored);
				if (succeeded) std::ofstream(cooked, std::ios::trunc) << "fresh-generation";
				return succeeded;
			});
			fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "Old generation must own its staging file before cancellation.");
			context.Expect(database.CancelImport(fixture.Source(0U), false), "Held generation cancellation must be accepted.");
			fixture.WriteMetadata(0U, 7U); fixture.Import(0U); database.Update();
			context.Expect(database.IsAssetCompiling(fixture.Source(0U)) && fixture.gate->Entries().size() == 1U,
				"Canceled physical ownership must serialize replacement despite three spare slots.");
			std::ifstream previous(cooked); std::string bytes; previous >> bytes; previous.close();
			context.Expect(bytes == "last-good", "Canceled held work must preserve the published artifact before retry.");
			fixture.gate->Release(); context.Expect(fixture.Settle(), "Physical settlement must admit exactly one fresh retry.");
			const auto entries = fixture.gate->Entries();
			context.Expect(entries.size() == 2U && entries.back().settings.value("revision", 0U) == 7U && fixture.gate->Peak() == 1U,
				"Old cleanup must retain the refreshed retry and same-source execution must never overlap.");
			std::ifstream published(cooked); published >> bytes;
			context.Expect(bytes == "fresh-generation" && !std::filesystem::exists(staged), "Fresh output must survive old cleanup without a stale staging artifact.");
		});
		registry.Add("Editor.AssetImportAdmission.CanceledAudioPreservesLastGood", [](TestContext& context) {
			AdmissionFixture fixture(4U, 0U); auto& database = AssetDatabase::Instance();
			database.SetAsyncImportCompileHookForTests({});
			const auto source = fixture.root / "Assets" / "clip.wav";
			const auto cooked = fixture.root / "Build" / "Assets" / "clip.teaudio";
			const auto id = TerraEngine::GenerateUUID();
			// A genuine bounded mono PCM WAV drives the internal audio decoder.
			const auto writeWave = [&](unsigned sample) {
				std::ofstream output(source, std::ios::binary | std::ios::trunc);
				const auto word = [&](unsigned value, unsigned bytes) { for (unsigned index = 0U; index < bytes; ++index) output.put(static_cast<char>(value >> (index * 8U))); };
				output.write("RIFF", 4); word(38U, 4); output.write("WAVEfmt ", 8); word(16U, 4);
				word(1U, 2); word(1U, 2); word(48000U, 4); word(96000U, 4); word(2U, 2); word(16U, 2);
				output.write("data", 4); word(2U, 4); word(sample, 2);
			};
			const auto readBytes = [](const std::filesystem::path& path) { std::ifstream input(path, std::ios::binary); return std::string(std::istreambuf_iterator<char>(input), {}); };
			writeWave(1U);
			context.Expect(TerraEngine::AssetPipeline::CompileAudioClip({source, cooked, id}).Succeeded(), "Initial genuine audio cook must establish valid last-good bytes.");
			const auto lastGood = readBytes(cooked);
			nlohmann::json metadata{{"guid", TerraEngine::ToHexString(id)}, {"importerName", "AudioClipImporter"},
				{"lastModified", std::filesystem::last_write_time(source).time_since_epoch().count()},
				{"sourcePath", "clip.wav"}, {"compiledPath", "clip.teaudio"}, {"settings", nlohmann::json::object()}};
			std::ofstream(source.string() + ".meta") << metadata.dump();
			writeWave(3000U);
			database.SetImportAdmissionHookForTests([gate = fixture.gate](const auto&, auto boundary) {
				if (boundary == AssetDatabase::ImportAdmissionBoundaryForTests::BeforePublication) {
					const std::atomic_bool neverCanceled{false}; (void)gate->Compile(AssetMetadata{}, neverCanceled);
				}
			});
			database.ImportAssetWithProgress(source, true);
			context.Expect(fixture.gate->Wait(1U), "Real audio candidate must reach the unlocked prepublication checkpoint.");
			context.Expect(database.CancelImport(source, false), "Cancellation must win before guarded audio publication.");
			context.Expect(readBytes(cooked) == lastGood, "Prepared audio candidate must not replace authoritative bytes before publication.");
			fixture.gate->Release(); context.Expect(fixture.Settle(), "Canceled real audio work must physically settle.");
			context.Expect(readBytes(cooked) == lastGood && !std::filesystem::exists(cooked.parent_path() / "clip.staged.teaudio"), "Canceled audio must retain last-good and remove its private candidate.");
			database.SetImportAdmissionHookForTests({}); database.ImportAssetWithProgress(source, true);
			context.Expect(fixture.Settle() && readBytes(cooked) != lastGood, "A fresh generation must publish its validated changed audio bytes.");
			const auto expected = fixture.root / "expected.teaudio";
			context.Expect(TerraEngine::AssetPipeline::CompileAudioClip({source, expected, id}).Succeeded() && readBytes(expected) == readBytes(cooked), "Fresh artifact must exactly match the valid real compiler output.");
		});
		registry.Add("Editor.AssetImportAdmission.CanceledModelPreservesLastGood", [](TestContext& context) {
			AdmissionFixture fixture(4U, 0U); auto& database = AssetDatabase::Instance();
			database.SetAsyncImportCompileHookForTests({});
			const auto source = fixture.root / "Assets" / "model.gltf";
			// Installed acceptance uses the sealed fixture beside the test executable.
			// Build-tree development runs may use the authored repository copy.
			const auto installedFixture = TerraEngine::PlatformUtils::GetExecutableDirectory()
				/ "Fixtures/AssetPipeline/Gltf/one-material.gltf";
			const auto modelFixture = std::filesystem::is_regular_file(installedFixture)
				? installedFixture : std::filesystem::path(TERRAENGINE_SOURCE_DIR)
					/ "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			std::filesystem::copy_file(modelFixture, source);
			// A genuine initial import establishes the complete durable package. No
			// compile substitute is installed, so Windows runs the actual child CLI.
			database.ImportAsset(source, true);
			TerraEngine::AssetPipeline::ModelPackageImportResult original;
			const bool initialReady = TerraEngine::AssetPipeline::LoadModelPackageImportResult(source, original);
			context.Expect(initialReady && original.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed
				&& !original.generatedMeshes.empty() && !original.generatedMaterials.empty(), "Real model subprocess must establish a valid last-good package.");
			if (!initialReady || original.generatedMeshes.empty() || original.generatedMaterials.empty()) return;
			const auto privatePart = [](const std::filesystem::path& path) {
				for (const auto& part : path) { const auto name = part.string(); if (name.starts_with(".ta-") || name.starts_with(".tb-")) return true; }
				return false;
			};
			const auto snapshot = [&] {
				std::map<std::filesystem::path, std::string> files;
				// Capture all authoritative sidecars, generated sources, payloads and
				// generation manifests; only the deliberately edited source is excluded.
				for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture.root)) {
					if (!entry.is_regular_file() || entry.path() == source || privatePart(entry.path())) continue;
					std::ifstream input(entry.path(), std::ios::binary);
					files.emplace(std::filesystem::relative(entry.path(), fixture.root), std::string(std::istreambuf_iterator<char>(input), {}));
				}
				return files;
			};
			const auto privateRootsClean = [&] {
				for (const auto& root : {database.GetAssetRoot(), database.GetBuildRoot()})
					for (const auto& entry : std::filesystem::directory_iterator(root)) if (privatePart(entry.path())) return false;
				return true;
			};
			const auto lastGood = snapshot();
			nlohmann::json changed; { std::ifstream input(source); input >> changed; }
			changed["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0.1, 0.8, 0.3, 1.0};
			{ std::ofstream output(source, std::ios::trunc); output << changed.dump(); }
			database.SetImportAdmissionHookForTests([gate = fixture.gate](const auto&, auto boundary) {
				if (boundary == AssetDatabase::ImportAdmissionBoundaryForTests::BeforePublication) {
					const std::atomic_bool neverCanceled{false}; (void)gate->Compile(AssetMetadata{}, neverCanceled);
				}
			});
			database.ImportAssetWithProgress(source, true);
			context.Expect(fixture.gate->Wait(1U), "Actual model child must settle and reach the unlocked guarded-publication checkpoint.");
			context.Expect(snapshot() == lastGood, "Complete private model candidate must leave every authoritative file unchanged before commit.");
			context.Expect(database.CancelImport(source, false), "Cancellation must reject the prepared model generation before parent publication.");
			fixture.gate->Release(); context.Expect(fixture.Settle(), "Canceled model request must release its physical slot.");
			context.Expect(snapshot() == lastGood && privateRootsClean() && database.GetImportAdmissionDiagnostics().active == 0U,
				"Canceled package must retain exact prior sidecar/payload/manifest bytes, clean private roots and settle capacity.");
			database.SetImportAdmissionHookForTests({}); database.ImportAssetWithProgress(source, true);
			context.Expect(fixture.Settle(), "A fresh real model subprocess retry must settle successfully.");
			TerraEngine::AssetPipeline::ModelPackageImportResult fresh;
			const bool loaded = TerraEngine::AssetPipeline::LoadModelPackageImportResult(source, fresh);
			context.Expect(loaded && fresh.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed
				&& fresh.animationManifest.publishedGeneration > original.animationManifest.publishedGeneration
				&& fresh.generatedMeshes.size() == original.generatedMeshes.size() && fresh.generatedMaterials.size() == original.generatedMaterials.size(),
				"Fresh valid candidate must advance the package generation without losing children.");
			if (loaded && fresh.generatedMeshes.size() == original.generatedMeshes.size() && fresh.generatedMaterials.size() == original.generatedMaterials.size()) {
				for (std::size_t index = 0U; index < fresh.generatedMeshes.size(); ++index)
					context.Expect(fresh.generatedMeshes[index].guid == original.generatedMeshes[index].guid, "Fresh model generation must retain stable mesh identity.");
				for (std::size_t index = 0U; index < fresh.generatedMaterials.size(); ++index)
					context.Expect(fresh.generatedMaterials[index].guid == original.generatedMaterials[index].guid, "Fresh model generation must retain stable material identity.");
			}
			context.Expect(snapshot() != lastGood && privateRootsClean(), "Fresh changed package must publish new bytes and retire its private candidate roots.");
		});
		registry.Add("Editor.AssetImportAdmission.QueuedCancelNeverStarts", [](TestContext& context) {
			AdmissionFixture fixture(1U, 2U); fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "An active callback must occupy the slot."); fixture.Import(1U);
			context.Expect(AssetDatabase::Instance().CancelImport(fixture.Source(1U), false), "Queued cancellation must be accepted."); fixture.gate->Release(); context.Expect(fixture.Settle(), "Queued cancellation must release ownership.");
			for (const auto& entry : fixture.gate->Entries()) context.Expect(entry.guid != fixture.ids[1], "Canceled queued source must never enter compiler execution.");
		});
		registry.Add("Editor.AssetImportAdmission.ActiveCancelSettlesProcess", [](TestContext& context) {
			AdmissionFixture fixture(1U, 1U); auto result = std::make_shared<TerraEngine::Utilities::OwnedProcessResult>();
			const auto child = TerraEngine::PlatformUtils::GetExecutableDirectory() /
#ifdef _WIN32
				"CorrectnessProcessFixture.exe";
#else
				"CorrectnessProcessFixture";
#endif
			context.Expect(std::filesystem::is_regular_file(child), "Install the real process fixture beside native test binaries.");
			const auto receipt = fixture.root / "child-pid";
			TerraEngine::Utilities::OwnedProcessRequest request; request.commandLine = Quote(child) + " --gate " + Quote(fixture.root / "never-release") + " --pid-file " + Quote(receipt) + " --timeout-ms 300000";
			request.timeout = std::chrono::seconds(15);
			AssetDatabase::Instance().SetAsyncImportCompileHookForTests([request, result](const AssetMetadata&, const std::atomic_bool& canceled) { *result = TerraEngine::Utilities::OwnedProcess::Run(request, canceled); return result->exitCode == 0; });
			fixture.Import(0U); context.Expect(WaitForFile(receipt.string() + ".0"), "A real held compiler child must report its process identity.");
			unsigned long pid{}; std::ifstream(receipt.string() + ".0") >> pid;
#ifdef _WIN32
			const HANDLE retained = OpenProcess(SYNCHRONIZE, FALSE, pid); context.Expect(retained != nullptr, "Retain the compiler process before cancellation.");
#endif
			AssetDatabase::Instance().CancelAllImportsAndWait(false);
#ifdef _WIN32
			const auto wait = WaitForSingleObject(retained, 0U); CloseHandle(retained); context.Expect(wait == WAIT_OBJECT_0, "Import cancellation must physically settle the retained compiler process before returning.");
#else
			context.Expect(kill(static_cast<pid_t>(pid), 0) != 0, "Canceled compiler must no longer be live at drain return.");
#endif
			context.Expect(result->started && result->canceled && AssetDatabase::Instance().GetCompileJobsCount() == 0U, "Cancellation must settle the owned import future and process together.");
		});
		registry.Add("Editor.AssetImportAdmission.FailureReleasesSlot", [](TestContext& context) {
			{
			AdmissionFixture fixture(1U, 6U); auto calls = std::make_shared<std::atomic_uint>(0U);
			AssetDatabase::Instance().SetAsyncImportCompileHookForTests([calls](const AssetMetadata&, const std::atomic_bool&) { const auto index = calls->fetch_add(1U); if (index == 1U) throw std::runtime_error("Controlled compiler failure"); return index != 0U; });
			for (unsigned index = 0U; index < 6U; ++index) fixture.Import(index);
			context.Expect(fixture.Settle() && calls->load() == 6U && AssetDatabase::Instance().GetCompileJobsCount() == 0U, "False and exceptional compiler results must release capacity for every later source.");
			}
			// These native faults run on the actual dispatcher/finalizer boundaries,
			// including the tracked-but-unsubmitted future rollback path.
			using Boundary = AssetDatabase::ImportAdmissionBoundaryForTests;
			for (const auto boundary : {Boundary::ExecutorCreation, Boundary::Enqueue, Boundary::Finalization}) {
				AdmissionFixture fixture(1U, 4U); auto hit = std::make_shared<std::atomic_bool>(false);
				AssetDatabase::Instance().SetImportAdmissionHookForTests([boundary, hit](const std::filesystem::path& path, Boundary current) {
					if (current == boundary && path.filename() == "source0.tecollider" && !hit->exchange(true)) throw std::runtime_error("Controlled admission boundary failure");
				});
				for (unsigned index = 0U; index < 4U; ++index) fixture.Import(index); fixture.gate->Release();
				context.Expect(fixture.Settle() && hit->load() && AssetDatabase::Instance().GetImportAdmissionDiagnostics().active == 0U,
					"Creation, enqueue and protected finalization failures must release every reserved slot without waiting on unsubmitted work.");
				context.Expect(AssetDatabase::Instance().FindAssetByGuid(fixture.ids[0]) != nullptr,
					"A failed replacement boundary must preserve last-good catalog registration.");
			}
		});
		registry.Add("Editor.AssetImportAdmission.ProjectCloseReopen", [](TestContext& context) {
			AdmissionFixture fixture(1U, 2U); fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "Old project must own active compiler work."); fixture.Import(1U);
			AssetDatabase::Instance().CancelAllImportsAndWait(false); AssetDatabase::Instance().Shutdown();
			context.Expect(AssetDatabase::Instance().GetCompileJobsCount() == 0U && fixture.gate->Entries().size() == 1U, "Project close must settle active work while canceled queued requests remain unlaunched.");
			AssetDatabase::Instance().Initialize(fixture.root); AssetDatabase::Instance().PauseWatcherForTests(); fixture.gate->Release();
			AssetDatabase::Instance().SetAsyncImportCompileHookForTests([owned = fixture.gate](const AssetMetadata& metadata, const std::atomic_bool& canceled) { return owned->Compile(metadata, canceled); });
			fixture.Import(0U); context.Expect(fixture.Settle() && AssetDatabase::Instance().FindAssetByGuid(fixture.ids[0]), "Reopened owner must admit and register a fresh import.");
		});
		registry.Add("Editor.AssetImportAdmission.SingleFrameWorkerProgress", [](TestContext& context) {
			AdmissionFixture fixture(1U, 4U); auto& frame = ThreadPool::instance(); frame.shutdown(); frame.initialize(1U);
			fixture.Import(0U); context.Expect(fixture.gate->Wait(1U), "Import work must remain genuinely held.");
			auto sentinel = frame.createTask([] { return 73U; }); auto completed = sentinel->getFuture(); frame.enqueue(sentinel);
			const bool ready = completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
			fixture.gate->Release(); context.Expect(fixture.Settle(), "Held import must settle after release."); frame.shutdown();
			context.Expect(ready && completed.get() == 73U, "An isolated import executor must leave the one-worker frame pool free to execute its sentinel.");
		});
	}
}
