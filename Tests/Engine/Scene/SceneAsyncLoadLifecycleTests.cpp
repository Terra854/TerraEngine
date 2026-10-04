// Exercise the real scene/prefab request ownership with deterministic native gates.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "Scene/SceneManager.h"

#include <chrono>
#include <fstream>
#include <thread>

namespace TerraEngine::Tests {
	namespace {
		using Correctness::ControlledGate;
		using Correctness::CompletionCounter;
		struct LoadFixture {
			Correctness::DisposableFixturePath files;
			std::filesystem::path oldRoot = SceneManager::Instance().GetSceneRoot();
			std::filesystem::path oldStartup = SceneManager::Instance().GetStartupScene();
			bool ownsWorkers = ThreadPool::instance().getThreadCount() == 0;
			bool reopenOnExit = false;
			std::vector<std::shared_ptr<ControlledGate>> gates;
			std::vector<std::shared_future<TerraEngine::UUID>> completions;
			LoadFixture() {
				// Let the runner's TERRAENGINE_THREADPOOL_WORKERS select capacity
				// so the same deterministic gates exercise both worker matrices.
				if(ownsWorkers) ThreadPool::instance().initialize();
				auto& scenes = SceneManager::Instance();
				scenes.BeginLoadSession(); scenes.SetSceneRoot(files.Root()); scenes.SetStartupScene({});
			}
			~LoadFixture() {
				// Release held producers before their paths, callback captures or pool
				// lifetime end, including assertion unwinding in deliberately red cases.
				for(const auto& gate : gates) gate->Cancel();
				for(const auto& future : completions) if(future.valid()) future.wait();
				auto& scenes = SceneManager::Instance();
				if(reopenOnExit) scenes.BeginLoadSession();
				scenes.UnloadAll(); scenes.SetSceneRoot(oldRoot); scenes.SetStartupScene(oldStartup);
				if(ownsWorkers) ThreadPool::instance().shutdown();
			}
			std::filesystem::path Write(bool prefab, std::string name, std::filesystem::path root = {}) {
				if(root.empty()) root = files.Root();
				std::filesystem::create_directories(root);
				const auto path = root / (prefab ? "source.teprefab" : "source.tescene");
				nlohmann::json data{{"format", prefab ? "TerraPrefabUnboundedV1" : "TerraSceneUnboundedV1"},
					{"name", std::move(name)}, {"objects", nlohmann::json::array()}};
				if(prefab) data["sourceId"] = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
				std::ofstream output(path); output << data.dump();
				if(!output.good()) throw TestFailure("The owned scene lifecycle source could not be written.");
				return path;
			}
			std::shared_future<TerraEngine::UUID> Async(bool prefab, const std::filesystem::path& path, SceneLoadOptions options = {}) {
				auto& scenes = SceneManager::Instance();
				auto future = prefab ? scenes.LoadPrefabAsync(path, std::move(options)) : scenes.LoadSceneAsync(path, std::move(options));
				completions.push_back(future);
				return future;
			}
			TerraEngine::UUID Sync(bool prefab, const std::filesystem::path& path) {
				return prefab ? SceneManager::Instance().LoadPrefab(path) : SceneManager::Instance().LoadScene(path);
			}
			const SceneDocument* Document(bool prefab, const TerraEngine::UUID& id) {
				return prefab ? SceneManager::Instance().GetPrefab(id) : SceneManager::Instance().GetScene(id);
			}
			std::shared_ptr<ControlledGate> Gate() {
				auto gate = std::make_shared<ControlledGate>(); gates.push_back(gate); return gate;
			}
		};
		SceneLoadOptions Hold(const std::shared_ptr<ControlledGate>& gate, SceneLoadCheckpoint stage,
			CompletionCounter visits = {}, bool failAfterRelease = false, std::shared_ptr<SceneLoadSourceSnapshot> captured = {}) {
			SceneLoadOptions options;
			options.checkpoint = [wait = gate->MakeWaiter(), stage, visits, failAfterRelease, captured](SceneLoadCheckpoint observed, const SceneLoadSourceSnapshot& source) {
				if(observed != stage) return;
				if(captured) *captured = source;
				visits.Increment();
				if(wait() == Correctness::GateResult::TimedOut) throw TestFailure("The owned load gate timed out.");
				if(failAfterRelease) throw std::runtime_error("Controlled unexpected scene loader failure.");
			};
			return options;
		}
		TerraEngine::UUID Result(TestContext& context, const std::shared_future<TerraEngine::UUID>& future) {
			context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "The real load must reach a terminal completion.");
			return future.get();
		}
		void ExpectUnexpected(TestContext& context, const std::shared_future<TerraEngine::UUID>& future) {
			context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "Exceptional load must settle its future.");
			bool observed = false;
			try { (void)future.get(); } catch(const std::runtime_error& error) { observed = std::string(error.what()).find("Controlled unexpected") != std::string::npos; }
			context.Expect(observed, "Unexpected loader errors must remain the original future exception.");
		}
		void Retirement(TestContext& context, bool prefab) {
			LoadFixture fixture;
			for(unsigned iteration = 0; iteration < 100; ++iteration) {
				SceneManager::Instance().UnloadAll();
				const auto path = fixture.Write(prefab, "retired source");
				const auto gate = fixture.Gate();
				const auto old = fixture.Async(prefab, path, Hold(gate, SceneLoadCheckpoint::AfterInterpretation));
				context.Expect(gate->WaitForArrivals(1), "Old interpretation must reach the controlled publication gate.");
				SceneManager::Instance().UnloadAll();
				const auto retired = SceneManager::Instance().GetLoadDiagnostics();
				context.Expect(retired.pendingScenes == 0 && retired.pendingPrefabs == 0 && retired.ownedLoads >= 1,
					"Visible cache retirement must clear reservations while retaining the held physical reader.");
				fixture.Write(prefab, "current source");
				const auto current = fixture.Sync(prefab, path);
				context.Expect(current != TerraEngine::EMPTY_UUID, "A fresh cache lifetime must admit replacement work.");
				gate->Release();
				context.Expect(Result(context, old) == TerraEngine::EMPTY_UUID && fixture.Document(prefab, current)->name == "current source",
					"No retired result may publish across any of the hundred controlled interleavings.");
			}
		}
		void NewReservation(TestContext& context, bool oldFailure) {
			for(bool prefab : {false, true}) {
				LoadFixture fixture;
				const auto path = fixture.Write(prefab, "old source");
				const auto oldGate = fixture.Gate();
				const auto old = fixture.Async(prefab, path, Hold(oldGate, SceneLoadCheckpoint::BeforeRead, {}, oldFailure));
				context.Expect(oldGate->WaitForArrivals(1), "The old reader must be physically held before retirement.");
				SceneManager::Instance().UnloadAll(); fixture.Write(prefab, "new source");
				const auto newGate = fixture.Gate(); CompletionCounter visits;
				const auto current = fixture.Async(prefab, path, Hold(newGate, SceneLoadCheckpoint::AfterInterpretation, visits));
				const auto reserved = SceneManager::Instance().GetLoadDiagnostics();
				oldGate->Release();
				if(oldFailure) ExpectUnexpected(context, old); else context.Expect(Result(context, old) == TerraEngine::EMPTY_UUID, "Retired reader must not return a usable ID.");
				context.Expect(newGate->WaitForArrivals(1), "The replacement must still own its pending reservation after old cleanup.");
				const auto afterOld = SceneManager::Instance().GetLoadDiagnostics();
				context.Expect(afterOld.latestRequestId == reserved.latestRequestId && afterOld.latestDisposition == SceneLoadDisposition::Pending
					&& (prefab ? afterOld.pendingPrefabs : afterOld.pendingScenes) == 1,
					"Old terminal diagnostics and cleanup must preserve the newer pending request identity.");
				const auto joined = fixture.Async(prefab, path, Hold(newGate, SceneLoadCheckpoint::AfterInterpretation, visits));
				newGate->Release();
				const auto id = Result(context, current);
				context.Expect(id != TerraEngine::EMPTY_UUID && Result(context, joined) == id && visits.Value() == 1,
					"Old cleanup must neither remove the new reservation nor force duplicate source work.");
			}
		}
		void InjectedRepair(TestContext& context, SceneLoadCheckpoint stage) {
			for(bool prefab : {false, true}) {
				LoadFixture fixture; const auto path = fixture.Write(prefab, "repaired source");
				const auto before = SceneManager::Instance().GetLoadDiagnostics();
				SceneLoadOptions options;
				options.checkpoint = [stage](SceneLoadCheckpoint observed, const SceneLoadSourceSnapshot&) {
					if(observed == stage) throw std::runtime_error("Controlled unexpected scene loader failure.");
				};
				if(stage == SceneLoadCheckpoint::Admission || stage == SceneLoadCheckpoint::Submission) {
					bool propagated = false;
					try { (void)fixture.Async(prefab, path, options); } catch(const std::runtime_error& error) { propagated = std::string(error.what()).find("Controlled unexpected") != std::string::npos; }
					context.Expect(propagated, "Failed admission/submission must clean reservation before propagating its initiating error.");
				} else ExpectUnexpected(context, fixture.Async(prefab, path, options));
				const auto failed = SceneManager::Instance().GetLoadDiagnostics();
				const auto expected = stage == SceneLoadCheckpoint::Admission ? SceneLoadDisposition::AdmissionFailure
					: stage == SceneLoadCheckpoint::Submission ? SceneLoadDisposition::SubmissionFailure
					: stage == SceneLoadCheckpoint::Publication ? SceneLoadDisposition::PublicationFailure : SceneLoadDisposition::LoaderException;
				context.Expect(failed.latestRequestId > before.latestRequestId && failed.latestDisposition == expected
					&& failed.pendingScenes == 0 && failed.pendingPrefabs == 0,
					"Unexpected completion must expose its exact disposition and release only its own reservation.");
				const auto id = Result(context, fixture.Async(prefab, path));
				context.Expect(id != TerraEngine::EMPTY_UUID && fixture.Document(prefab, id)->name == "repaired source", "Same-source retry must succeed without restarting or clearing unrelated caches.");
			}
		}
	}
	void RegisterSceneAsyncLoadLifecycleTests(TestRegistry& registry) {
		registry.Add("Engine.SceneLoadRecovery.SceneRetirement", [](TestContext& context) { Retirement(context, false); });
		registry.Add("Engine.SceneLoadRecovery.PrefabRetirement", [](TestContext& context) { Retirement(context, true); });
		registry.Add("Engine.SceneLoadRecovery.RootSnapshot", [](TestContext& context) {
			for(bool prefab : {false, true}) {
				LoadFixture fixture; fixture.Write(prefab, "old root"); const auto gate = fixture.Gate();
				auto source = std::make_shared<SceneLoadSourceSnapshot>();
				const auto old = fixture.Async(prefab, prefab ? "source.teprefab" : "source.tescene", Hold(gate, SceneLoadCheckpoint::BeforeRead, {}, false, source));
				context.Expect(gate->WaitForArrivals(1), "The reader must capture its source before the root changes.");
				const auto nextRoot = fixture.files.Child("next-root"); fixture.Write(prefab, "new root", nextRoot);
				SceneManager::Instance().SetSceneRoot(nextRoot);
				const auto current = fixture.Sync(prefab, prefab ? "source.teprefab" : "source.tescene"); gate->Release();
				context.Expect(source->root == fixture.files.Root() && source->absolutePath.parent_path() == fixture.files.Root()
					&& source->mountedLogicalPath == (prefab ? "source.teprefab" : "source.tescene")
					&& Result(context, old) == TerraEngine::EMPTY_UUID && current != TerraEngine::EMPTY_UUID
					&& fixture.Document(prefab, current)->name == "new root",
					"Root replacement must retain immutable old source spelling while revoking old publication authority.");
			}
		});
		registry.Add("Engine.SceneLoadRecovery.OldCompletionAfterNewPublication", [](TestContext& context) {
			LoadFixture fixture; const auto path = fixture.Write(false, "old authored version"); const auto gate = fixture.Gate();
			const auto old = fixture.Async(false, path, Hold(gate, SceneLoadCheckpoint::AfterInterpretation));
			context.Expect(gate->WaitForArrivals(1), "Old decode must be held while explicit reread publishes a fresh token.");
			fixture.Write(false, "new authored version"); const auto current = SceneManager::Instance().ReloadScene(path); gate->Release();
			context.Expect(current != TerraEngine::EMPTY_UUID && Result(context, old) == TerraEngine::EMPTY_UUID
				&& fixture.Document(false, current)->name == "new authored version", "Explicit reload must supersede rather than wait for or be overwritten by old work.");
		});
		registry.Add("Engine.SceneLoadRecovery.OldCleanupKeepsNewReservation", [](TestContext& context) { NewReservation(context, false); });
		registry.Add("Engine.SceneLoadRecovery.SameSourceSingleFlight", [](TestContext& context) {
			for(bool prefab : {false, true}) {
				LoadFixture fixture; const auto path = fixture.Write(prefab, "single authoritative document"); const auto gate = fixture.Gate(); CompletionCounter visits;
				const auto first = fixture.Async(prefab, path, Hold(gate, SceneLoadCheckpoint::BeforeRead, visits));
				context.Expect(gate->WaitForArrivals(1), "A single reader must own the source reservation.");
				std::vector<std::shared_future<TerraEngine::UUID>> observers;
				for(unsigned observer = 0; observer < 16; ++observer) observers.push_back(fixture.Async(prefab, path, Hold(gate, SceneLoadCheckpoint::BeforeRead, visits)));
				gate->Release(); const auto id = Result(context, first);
				for(const auto& observer : observers) context.Expect(Result(context, observer) == id, "Every same-source observer must share the authoritative completion.");
				context.Expect(id != TerraEngine::EMPTY_UUID && visits.Value() == 1, "Single flight must execute exactly one reader.");
			}
		});
		registry.Add("Engine.SceneLoadRecovery.ShutdownClosesAndDrains", [](TestContext& context) {
			LoadFixture fixture; const auto gate = fixture.Gate();
			const auto scene = fixture.Async(false, fixture.Write(false, "owned scene"), Hold(gate, SceneLoadCheckpoint::BeforeRead));
			const auto prefab = fixture.Async(true, fixture.Write(true, "owned prefab"), Hold(gate, SceneLoadCheckpoint::BeforeRead));
			context.Expect(gate->WaitForArrivals(1), "An admitted scene/prefab reader must still be physically held.");
			SceneManager::Instance().UnloadAll(); fixture.reopenOnExit = true;
			// A separate existing executor represents the lifecycle caller, allowing
			// this proof to run even with one global scene-reader worker.
			auto caller = ThreadPool::CreateOwned(1); CompletionCounter started;
			auto task = caller->createTask([started] { started.Increment(); SceneManager::Instance().PrepareShutdown(); return true; });
			auto closing = task->getFuture(); caller->enqueue(task);
			struct ClosingOwner { std::shared_ptr<ControlledGate> gate; std::future<bool>& completion; ~ClosingOwner() { gate->Cancel(); if(completion.valid()) completion.wait(); } } owner{gate, closing};
			context.Expect(started.WaitForAtLeast(1), "The actual shutdown caller must begin closing admission.");
			const auto latePath = fixture.Write(false, "valid late request", fixture.files.Child("late"));
			bool closed = false; const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			while(std::chrono::steady_clock::now() < deadline) {
				if(!SceneManager::Instance().GetLoadDiagnostics().admissionOpen) { closed = true; break; }
				std::this_thread::yield();
			}
			context.Expect(closed && closing.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready, "Closed admission must be observable while shutdown still owns held physical loads.");
			const auto held = SceneManager::Instance().GetLoadDiagnostics();
			context.Expect(held.ownedLoads == 2 && held.pendingScenes == 0 && held.pendingPrefabs == 0,
				"Shutdown must own both physically held requests even after their visible caches were cleared.");
			context.Expect(Result(context, fixture.Async(false, latePath)) == TerraEngine::EMPTY_UUID,
				"Closed admission must return an ordinary unusable result without scheduling new work.");
			gate->Release(); context.Expect(closing.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready && closing.get(), "Shutdown must settle all admitted generations after controlled release.");
			const auto drained = SceneManager::Instance().GetLoadDiagnostics();
			context.Expect(!drained.admissionOpen && drained.ownedLoads == 0 && drained.physicallySettled >= held.physicallySettled + 2,
				"Shutdown completion must prove physical settlement before dependencies can be released.");
			context.Expect(Result(context, scene) == TerraEngine::EMPTY_UUID && Result(context, prefab) == TerraEngine::EMPTY_UUID, "Shutdown-retired results cannot publish.");
			SceneManager::Instance().Shutdown(); SceneManager::Instance().Shutdown(); SceneManager::Instance().Initialize();
		});
		registry.Add("Engine.SceneLoadRecovery.InitializeReopens", [](TestContext& context) {
			LoadFixture fixture; const auto path = fixture.Write(false, "fresh initialize lifetime");
			fixture.reopenOnExit = true;
			SceneManager::Instance().Shutdown(); SceneManager::Instance().Shutdown();
			context.Expect(Result(context, fixture.Async(false, path)) == TerraEngine::EMPTY_UUID, "Repeated shutdown must retain explicitly closed admission.");
			SceneManager::Instance().Initialize();
			context.Expect(Result(context, fixture.Async(false, path)) != TerraEngine::EMPTY_UUID, "Initialize must explicitly reopen a fresh cache lifetime.");
		});
		registry.Add("Engine.SceneLoadRecovery.InvalidFieldRepair", [](TestContext& context) {
			for(bool prefab : {false, true}) {
				LoadFixture fixture; const auto path = fixture.Write(prefab, "bad field");
				nlohmann::json data; { std::ifstream input(path); input >> data; } data["name"] = 42;
				{ std::ofstream output(path); output << data.dump(); }
				context.Expect(Result(context, fixture.Async(prefab, path)) == TerraEngine::EMPTY_UUID, "Malformed authored field must settle normally without a usable identity.");
				fixture.Write(prefab, "repaired field"); context.Expect(Result(context, fixture.Async(prefab, path)) != TerraEngine::EMPTY_UUID, "A repaired field must admit fresh same-session work.");
			}
		});
		registry.Add("Engine.SceneLoadRecovery.LoaderExceptionRepair", [](TestContext& context) { InjectedRepair(context, SceneLoadCheckpoint::BeforeRead); });
		registry.Add("Engine.SceneLoadRecovery.AdmissionFailureRepair", [](TestContext& context) { InjectedRepair(context, SceneLoadCheckpoint::Admission); });
		registry.Add("Engine.SceneLoadRecovery.SubmissionFailureRepair", [](TestContext& context) { InjectedRepair(context, SceneLoadCheckpoint::Submission); });
		registry.Add("Engine.SceneLoadRecovery.PublicationFailureRepair", [](TestContext& context) { InjectedRepair(context, SceneLoadCheckpoint::Publication); });
		registry.Add("Engine.SceneLoadRecovery.OldFailureKeepsNewRequest", [](TestContext& context) { NewReservation(context, true); });
	}
}
