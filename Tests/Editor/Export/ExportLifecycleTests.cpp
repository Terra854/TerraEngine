#include "TestHarness.h"
#include "BuildProfile.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/ThreadPool/ThreadPool.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"
#include "Editor/Export/EditorExportOperationController.h"
#include "Editor/Export/ExportOutputTransaction.h"
#include "Editor/TerraEngine-Editor/Windows/BuildProfilesWindow.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <future>
#include <fstream>
#include <memory>
#include <thread>

namespace TerraEngine::EditorTests {
	namespace {
		using namespace TerraEngine::Tests::Correctness;
		class PoolSession {
		public:
			PoolSession() { ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2); }
			~PoolSession() { ThreadPool::instance().shutdown(); }
		};
		bool AwaitFinal(EditorExportOperationController& controller, std::uint64_t id) {
			const auto deadline = std::chrono::steady_clock::now() + kDefaultTimeout;
			while (std::chrono::steady_clock::now() < deadline) {
				controller.Poll(); const auto observation = controller.Observe(id);
				if (observation && observation->result) return true;
				std::this_thread::yield();
			}
			return false;
		}
		EditorExportOperationRequest Request(EditorExportOperationController& controller) {
			EditorExportOperationRequest request;
			request.projectGeneration = controller.Generation();
			request.request.runId = "controlled-export";
			request.launchAfterSuccess = true;
			request.launcherPath = "captured-platform-launcher";
			return request;
		}
		ExportResult ControlledSuccess(const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
			ExportResult result; result.message = "fixed controlled outcome";
			result.success = cancellation->Commit([] { return true; }); return result;
		}
	}
	void RegisterExportLifecycleTests(TestRegistry& registry) {
		registry.Add("Editor.ExportRetention.ThousandExportsBounded", [](TestContext& context) {
			PoolSession pool;
			EditorExportOperationController controller([](const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
				// Fixed-size native outcomes isolate retained ownership from asset size.
				ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
			});
			for (std::uint64_t expected = 1; expected <= 1000; ++expected) {
				const auto admission = controller.Admit(Request(controller));
				context.Expect(admission.accepted && admission.operationId == expected && AwaitFinal(controller, admission.operationId), "Every controlled export should receive a contiguous identity and settle.");
			}
			std::size_t retained = 0;
			for (std::uint64_t id = 1; id <= 1000; ++id) if (controller.Observe(id)) ++retained;
			context.Expect(retained == 64, "One thousand exports must retain only the default 64 terminal observations.");
			const auto before = controller.RetentionDiagnostics();
			for (int visit = 0; visit < 1000; ++visit) controller.Poll();
			const auto after = controller.RetentionDiagnostics();
			context.Expect(after.retainedTerminal == 64 && after.active == 0 && after.expiredCount == 936 && after.lastIssuedId == 1000 && after.pollVisits == before.pollVisits,
				"Idle polling must visit no historical exports and expose bounded ownership diagnostics.");
		});
		registry.Add("Editor.ExportRetention.ProjectDrainExpiresHistory", [](TestContext& context) {
			PoolSession pool;
			EditorExportOperationController controller([](const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
				ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
			});
			const auto admission = controller.Admit(Request(controller));
			context.Expect(admission.accepted && AwaitFinal(controller, admission.operationId), "The project should own a completed export before retirement.");
			const auto copied = controller.Observe(admission.operationId);
			controller.StopAndDrain();
			context.Expect(!controller.Observe(admission.operationId) && !controller.TakeLaunch(admission.operationId), "Drained project ownership and launch intent must expire.");
			context.Expect(copied && copied->result && copied->result->success, "An observer's already copied receipt must survive controller expiry.");
			context.Expect(controller.Lookup(admission.operationId).disposition == EditorExportLookupDisposition::Expired
				&& controller.RetentionDiagnostics().retainedTerminal == 0, "Project drain must expose explicit expiry and remove history ownership.");
			controller.BeginProject();
			const auto next = controller.Admit(Request(controller));
			context.Expect(next.accepted && next.operationId == admission.operationId + 1 && AwaitFinal(controller, next.operationId), "A replacement project must continue identities without aliasing old receipts.");
		});
		registry.Add("Editor.ExportRetention.PositiveLimitAndLowering", [](TestContext& context) {
			PoolSession pool; EditorExportOperationController controller(ControlledSuccess);
			bool rejected = false;
			try { controller.SetTerminalHistoryLimit(0); } catch (const std::invalid_argument&) { rejected = true; }
			context.Expect(rejected && controller.RetentionDiagnostics().terminalLimit == 64, "Zero capacity must reject without altering the positive default.");
			controller.SetTerminalHistoryLimit(4);
			for (int index = 0; index < 6; ++index) { const auto admitted = controller.Admit(Request(controller)); context.Expect(admitted.accepted && AwaitFinal(controller, admitted.operationId), "Configured history exports should settle."); }
			controller.SetTerminalHistoryLimit(2);
			context.Expect(controller.RetentionDiagnostics().retainedTerminal == 2 && controller.RetentionDiagnostics().terminalLimit == 2,
				"Lowering capacity must immediately retain only the newest positive limit.");
			for (std::uint64_t id = 1; id <= 4; ++id) context.Expect(controller.Lookup(id).disposition == EditorExportLookupDisposition::Expired, "FIFO lowering must expire oldest receipts.");
			for (std::uint64_t id : {5u, 6u}) context.Expect(controller.Lookup(id).disposition == EditorExportLookupDisposition::Terminal, "FIFO lowering must preserve newest receipts.");
			context.Expect(controller.Lookup(0).disposition == EditorExportLookupDisposition::Unknown && controller.Lookup(7).disposition == EditorExportLookupDisposition::Unknown, "Zero and never-issued identities must not masquerade as expired history.");
		});
		registry.Add("Editor.ExportRetention.AbsentObserverCannotPin", [](TestContext& context) {
			PoolSession pool; EditorExportOperationController controller(ControlledSuccess); controller.SetTerminalHistoryLimit(2);
			for (int index = 0; index < 5; ++index) { const auto admitted = controller.Admit(Request(controller)); context.Expect(admitted.accepted && AwaitFinal(controller, admitted.operationId), "Exports with absent observers must progress."); }
			context.Expect(controller.Consume(1, 77).disposition == EditorExportLookupDisposition::Expired && !controller.TakeLaunch(1), "Unconsumed observers and launches must not pin expired history.");
			context.Expect(controller.RetentionDiagnostics().retainedTerminal == 2, "Absent observers cannot grow terminal ownership.");
		});
		registry.Add("Editor.ExportRetention.ActiveAndAdoptionNeverEvicted", [](TestContext& context) {
			PoolSession pool; ControlledGate gate; bool failAdoption = false;
			auto executions = std::make_shared<std::atomic_uint>(0);
			EditorExportOperationController::OwnerHooks hooks;
			hooks.beforeAdoption = [&] { if (failAdoption) throw std::runtime_error("controlled adoption failure"); };
			EditorExportOperationController controller([executions, wait = gate.MakeWaiter()](const ExportRequest& request, ExportProgressCallback progress, std::shared_ptr<ExportCancellationState> cancellation) {
				if (executions->fetch_add(1) != 0) (void)wait();
				return ControlledSuccess(request, progress, std::move(cancellation));
			}, hooks);
			const auto historical = controller.Admit(Request(controller)); context.Expect(AwaitFinal(controller, historical.operationId), "Initial receipt should settle.");
			const auto active = controller.Admit(Request(controller)); context.Expect(active.accepted && gate.WaitForArrivals(1), "The next export must be physically active at its gate.");
			controller.SetTerminalHistoryLimit(1);
			context.Expect(controller.Lookup(active.operationId).disposition == EditorExportLookupDisposition::Active && controller.RetentionDiagnostics().active == 1, "Capacity changes must not evict executing work.");
			failAdoption = true; gate.Release();
			bool failed = false; const auto deadline = std::chrono::steady_clock::now() + kDefaultTimeout;
			while (!failed && std::chrono::steady_clock::now() < deadline) { try { controller.Poll(); } catch (const std::runtime_error&) { failed = true; } std::this_thread::yield(); }
			context.Expect(failed && controller.RetentionDiagnostics().adoptionPending == 1 && controller.Lookup(active.operationId).disposition == EditorExportLookupDisposition::Active,
				"A consumed physical future must retain exact unadopted truth for owner retry.");
			controller.SetTerminalHistoryLimit(2); controller.SetTerminalHistoryLimit(1);
			failAdoption = false;
			context.Expect(AwaitFinal(controller, active.operationId) && executions->load() == 2 && controller.Observe(active.operationId)->result->message == "fixed controlled outcome",
				"Retry must adopt the original result without executing another worker.");
			context.Expect(controller.Lookup(historical.operationId).disposition == EditorExportLookupDisposition::Expired && controller.RetentionDiagnostics().adoptionPending == 0, "Successful adoption must atomically enter FIFO history.");
		});
		registry.Add("Editor.ExportRetention.ConsumeAndLaunchOnce", [](TestContext& context) {
			PoolSession pool; EditorExportOperationController controller(ControlledSuccess);
			const auto admitted = controller.Admit(Request(controller)); context.Expect(AwaitFinal(controller, admitted.operationId), "The multi-observer export should settle.");
			const auto first = controller.Consume(admitted.operationId, 1); const auto second = controller.Consume(admitted.operationId, 2);
			context.Expect(first.result && second.result && first.result->message == second.result->message, "Independent observers must receive the exact same copied outcome.");
			context.Expect(!controller.Consume(admitted.operationId, 1).result && !controller.Consume(admitted.operationId, 2).result, "Each observer can consume only once while retained.");
			context.Expect(controller.TakeLaunch(admitted.operationId) == std::optional<std::filesystem::path>("captured-platform-launcher") && !controller.TakeLaunch(admitted.operationId), "Multiple observers must not multiply launch intent.");
			controller.BeginProject(); context.Expect(first.result->success && second.result->success && !controller.TakeLaunch(admitted.operationId), "Copied outcomes survive retirement while launch authority does not.");
		});
		registry.Add("Editor.ExportRetention.ExpiryRevokesLaunch", [](TestContext& context) {
			PoolSession pool; DisposableFixturePath files; Engine engine; EditorContext editor;
			editor.engine = &engine; editor.projectManager = std::make_shared<ProjectManager>();
			context.Expect(editor.projectManager->CreateProject(files.Child("Project"), "RetentionPanel", ProjectTemplate::Blank), "The real panel workspace should open.");
			BuildProfile profile; profile.name = "Retention"; profile.id = "retention"; profile.enableManagedCompilation = false; profile.startupScene = "Scenes/Scene.tescene";
			context.Expect(SaveBuildProfile(profile, editor.projectManager->GetProjectRoot() / "BuildProfiles/Retention.json"), "The real panel profile should save.");
			editor.exportController = std::make_shared<EditorExportOperationController>(ControlledSuccess); editor.exportController->SetTerminalHistoryLimit(1);
			BuildProfilesWindow window(editor); window.StartExportAndRun(); const auto oldId = window.GetExportOperationId();
			context.Expect(oldId && AwaitFinal(*editor.exportController, oldId), "The real Build And Run action should complete before its absent panel observes it.");
			const auto later = editor.exportController->Admit(Request(*editor.exportController)); context.Expect(later.accepted && AwaitFinal(*editor.exportController, later.operationId), "A later export should evict the pending panel receipt.");
			window.PollExportAtProtectedFrame();
			context.Expect(window.GetExportOperationId() == 0 && !editor.exportController->TakeLaunch(oldId), "The real panel must stop polling expiry and revoke unconsumed launch intent.");
			window.PollExportAtProtectedFrame();
		});
		registry.Add("Editor.ExportRetention.ContiguousIssuanceAndConcurrentCopies", [](TestContext& context) {
			PoolSession pool; bool denySubmission = true;
			EditorExportOperationController::OwnerHooks hooks;
			hooks.beforeSubmission = [&] { if (denySubmission) throw std::runtime_error("controlled submission failure"); };
			EditorExportOperationController controller(ControlledSuccess, hooks); controller.SetTerminalHistoryLimit(2);
			const auto failed = controller.Admit(Request(controller));
			context.Expect(!failed.accepted && failed.operationId == 0 && controller.Lookup(1).disposition == EditorExportLookupDisposition::Unknown && controller.RetentionDiagnostics().lastIssuedId == 0,
				"Failed submission must leave no externally issued hole or active ownership.");
			denySubmission = false;
			std::atomic_bool stop = false; std::atomic_bool coherent = true;
			// Readers perform only copied lookup; adoption remains protected-owner work.
			std::jthread reader([&] { while (!stop.load()) { for (std::uint64_t id = 1; id <= 100; ++id) {
				const auto copied = controller.Lookup(id);
				if (copied.observation && (copied.observation->operationId != id || (copied.observation->result && !copied.observation->progress.finished))) coherent.store(false);
			} } });
			struct ReaderCleanup { std::atomic_bool& stop; ~ReaderCleanup() { stop.store(true); } } readerCleanup{stop};
			for (std::uint64_t id = 1; id <= 100; ++id) {
				const auto admitted = controller.Admit(Request(controller)); context.Expect(admitted.accepted && admitted.operationId == id && AwaitFinal(controller, id), "All successful public IDs must remain contiguous after failed submission.");
			}
			stop.store(true); reader.join(); context.Expect(coherent.load(), "Concurrent copied observations must remain coherent through FIFO expiry.");
			controller.BeginProject(); context.Expect(controller.Lookup(100).disposition == EditorExportLookupDisposition::Expired && controller.Lookup(101).disposition == EditorExportLookupDisposition::Unknown, "Retirement must distinguish issued expiry from unknown IDs without aliasing.");
		});
		registry.Add("Editor.ExportLifecycle.EngineBackedDuplicateAdmission", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			using namespace TerraEngine::Tests::Correctness;
			DisposableFixturePath files;
			Engine engine;
			EditorContext editor;
			editor.engine = &engine;
			editor.projectManager = std::make_shared<ProjectManager>();
			context.Expect(editor.projectManager->CreateProject(files.Child("Project"), "ExportAdmission", ProjectTemplate::Blank), "The real editor project fixture should open.");
			BuildProfile profile;
			profile.name = "Controlled";
			profile.id = "controlled";
			profile.enableManagedCompilation = false;
			profile.outputDirectory = "Builds/Controlled";
			profile.startupScene = "Scenes/Scene.tescene";
			context.Expect(SaveBuildProfile(profile, editor.projectManager->GetProjectRoot() / "BuildProfiles" / "Controlled.json"), "The captured profile should save.");
			EditorAutomationService service;
			service.SetEditorContext(&editor);
			auto& pool = ThreadPool::instance();
			pool.shutdown();
			pool.initialize(1);
			auto started = std::make_shared<std::promise<void>>();
			auto startedFuture = started->get_future();
			auto canceledInWorker = std::make_shared<std::atomic_bool>(false);
			// The actual admitted worker occupies the sole pool slot until the
			// production cancellation token changes. No manual release can conceal
			// owner-side schema validation waiting behind that occupied worker.
			editor.exportController = std::make_shared<EditorExportOperationController>([started, canceledInWorker](const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
				started->set_value();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				while (!cancellation->requested.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				canceledInWorker->store(cancellation->requested.load());
				ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
			});
			bool arrived = false;
			nlohmann::json first;
			nlohmann::json duplicate;
			nlohmann::json cancellationResponse;
			try {
				first = service.HandleTool("export.start", {{"profile", "Controlled"}}, WriteMode::WritesEnabled, "controlled-first");
				arrived = startedFuture.wait_for(kDefaultTimeout) == std::future_status::ready;
				duplicate = service.HandleTool("export.start", {{"profile", "Controlled"}}, WriteMode::WritesEnabled, "controlled-duplicate");
				if (first.value("ok", false)) cancellationResponse = service.HandleTool("export.cancel", {{"jobId", first["data"].value("jobId", std::string{})}}, WriteMode::WritesEnabled, "controlled-cancel");
			} catch (...) {
				editor.exportController->StopAndDrain();
				pool.shutdown();
				throw;
			}
			// Drain while the service, project and context still exist. Assertions
			// happen after cleanup, so the red witness cannot cause owner UAF.
			editor.exportController->StopAndDrain();
			pool.shutdown();
			context.Expect(arrived, "The actual admitted export worker should reach its controlled gate.");
			context.Expect(first.value("ok", false), "The engine-backed production branch should accept its first export.");
			context.Expect(!duplicate.value("ok", true), "A second frontend export must be denied until the first has a final result.");
			context.Expect(cancellationResponse.value("ok", false) && cancellationResponse["data"].value("cancellationDisposition", std::string{}) == "accepted" && canceledInWorker->load(), "Real cancellation must return and settle the sole occupied worker without a manual fixture release.");
		});
		registry.Add("Editor.ExportLifecycle.ControlledInterleavings", [](TestContext& context) {
			PoolSession pool;
			for (int iteration = 0; iteration < 100; ++iteration) {
				ControlledGate gate;
				const auto wait = gate.MakeWaiter();
				const bool commitEarly = iteration % 5 == 4;
				const bool throwWorker = iteration % 5 == 2;
				const auto attempts = std::make_shared<std::atomic_uint>(0);
				EditorExportOperationController controller([wait, commitEarly, throwWorker, attempts](const ExportRequest&, ExportProgressCallback progress, std::shared_ptr<ExportCancellationState> cancellation) {
					ExportProgress informational; informational.finished = true; informational.phase = ExportPhase::Complete; informational.overallProgress = 1;
					progress(informational);
					if (commitEarly) cancellation->Commit([] { return true; });
					(void)wait();
					if (throwWorker && attempts->fetch_add(1) == 0) throw std::runtime_error("controlled worker exception");
					ExportResult result; result.success = commitEarly || cancellation->Commit([] { return true; }); return result;
				});
				const auto admission = controller.Admit(Request(controller));
				context.Expect(admission.accepted && gate.WaitForArrivals(1), "The worker should reach its production controller gate.");
				const auto progress = controller.Observe(admission.operationId);
				context.Expect(progress && progress->progress.finished && !progress->result, "Informational completion cannot publish a terminal result.");
				context.Expect(!controller.Admit(Request(controller)).accepted, "Progress-before-result cannot reopen admission.");
				// Concurrent readers copy one locked revision/progress/result snapshot.
				std::atomic_bool stop = false; std::atomic_bool coherent = true;
				std::thread reader([&] { std::uint64_t previous = 0; while (!stop.load()) { const auto sample = controller.Observe(admission.operationId); if (!sample || sample->revision < previous || (sample->result && !sample->progress.finished)) coherent.store(false); if (sample) previous = sample->revision; } });
				ExportCancellationDisposition disposition = ExportCancellationDisposition::AlreadyTerminal;
				if (iteration % 5 == 1 || commitEarly) disposition = controller.RequestCancellation(admission.operationId);
				gate.Release();
				const bool ready = AwaitFinal(controller, admission.operationId);
				stop.store(true); reader.join();
				context.Expect(ready && coherent.load(), "All controlled exports should settle with coherent concurrent observations.");
				const auto result = controller.ConsumeFinal(admission.operationId, 99);
				context.Expect(result.has_value() && !controller.ConsumeFinal(admission.operationId, 99), "Each owner observer should consume the terminal result exactly once.");
				if (iteration % 5 == 1) context.Expect(result->canceled && disposition == ExportCancellationDisposition::Accepted, "Pre-commit cancellation should win.");
				else if (throwWorker) context.Expect(result->failureCategory == ExportFailureCategory::UnexpectedFailure, "Escaped worker exceptions must become one failure result.");
				else context.Expect(result->success, "Committed exports should remain successful.");
				if (commitEarly) context.Expect(disposition == ExportCancellationDisposition::TooLate, "Post-commit cancellation should report too late.");
				if (result->success) {
					context.Expect(controller.TakeLaunch(admission.operationId) == std::optional<std::filesystem::path>("captured-platform-launcher"), "Launch must use captured immutable intent.");
					context.Expect(!controller.TakeLaunch(admission.operationId), "Successful launch intent should be consumed once.");
				} else context.Expect(!controller.TakeLaunch(admission.operationId), "Canceled/failed output cannot launch.");
				controller.BeginProject();
				context.Expect(!controller.TakeLaunch(admission.operationId), "Project replacement must invalidate old successful launch intent.");
				const auto retry = controller.Admit(Request(controller));
				context.Expect(retry.accepted && AwaitFinal(controller, retry.operationId), "A surviving editor should admit retry after failure or cancellation.");
				context.Expect(controller.Observe(retry.operationId)->result->success, "The retry should complete successfully after the first controlled failure.");
				controller.StopAndDrain(); controller.StopAndDrain();
			}
		});
		registry.Add("Editor.ExportLifecycle.RealFrontendsShareProjectOwnership", [](TestContext& context) {
			using namespace TerraEngine::Editor::Automation;
			PoolSession pool;
			DisposableFixturePath files;
			Engine engine;
			EditorContext editor; editor.engine = &engine; editor.projectManager = std::make_shared<ProjectManager>();
			context.Expect(editor.projectManager->CreateProject(files.Child("Project"), "FrontendOwnership", ProjectTemplate::Blank), "The engine-backed frontend project should open.");
			BuildProfile profile; profile.name = "Frontend"; profile.id = "frontend"; profile.startupScene = "Scenes/Scene.tescene"; profile.enableManagedCompilation = false;
			context.Expect(SaveBuildProfile(profile, editor.projectManager->GetProjectRoot() / "BuildProfiles" / "Frontend.json"), "The real UI profile should save.");
			ControlledGate gate;
			const auto wait = gate.MakeWaiter();
			editor.exportController = std::make_shared<EditorExportOperationController>([wait](const ExportRequest&, ExportProgressCallback progress, std::shared_ptr<ExportCancellationState> cancellation) {
				progress({}); (void)wait(); ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
			});
			EditorAutomationService service; service.SetEditorContext(&editor);
			const auto loaded = ScriptingSystem::Instance().HasLoadedGameplayAssembly();
			const auto assembly = ScriptingSystem::Instance().GetLoadedGameplayAssemblyPath();
			{
				BuildProfilesWindow window(editor);
				window.StartExportAndRun();
				context.Expect(gate.WaitForArrivals(1), "The actual UI action should enter shared controller execution.");
				const auto captured = editor.exportController->Observe(window.GetExportOperationId());
				context.Expect(captured && captured->intent.launchAfterSuccess && captured->intent.request.profile.platform == profile.platform
					&& captured->intent.launcherPath.filename() == GetExportLauncherFileName(editor.projectManager->GetProjectRoot().filename().string(), profile.platform), "Build And Run should capture its original project/platform launcher before asynchronous execution.");
				window.SetVisible(false);
				const auto denied = service.HandleTool("export.start", {{"profile", "Frontend"}}, WriteMode::WritesEnabled, "frontend-duplicate");
				context.Expect(!denied.value("ok", true), "Real automation must share admission with a hidden UI panel.");
			}
			// Destroying the panel leaves only project-owned request/shared state.
			gate.Release(); editor.exportController->StopAndDrain(); service.PumpExportJobsAtProtectedFrame();
			context.Expect(ScriptingSystem::Instance().HasLoadedGameplayAssembly() == loaded && ScriptingSystem::Instance().GetLoadedGameplayAssemblyPath() == assembly, "Export must leave current gameplay assembly state intact.");
			editor.exportController->BeginProject();
			const auto accepted = service.HandleTool("export.start", {{"profile", "Frontend"}}, WriteMode::WritesEnabled, "frontend-retry");
			context.Expect(accepted.value("ok", false), "Engine-backed automation should retry after UI ownership settles.");
			editor.exportController->StopAndDrain(); service.PumpExportJobsAtProtectedFrame();
			const auto job = service.HandleTool("jobs.get", {{"jobId", accepted["data"].value("jobId", std::string{})}}, WriteMode::ReadOnly, "frontend-final");
			context.Expect(job.value("ok", false), "The settled real automation job should remain queryable.");
		});
		registry.Add("Editor.ExportLifecycle.CloseDrainAndLateCallbacks", [](TestContext& context) {
			PoolSession pool;
			ControlledGate gate;
			ExportProgressCallback lateProgress;
			std::shared_ptr<ExportCancellationState> cancellation;
			auto attempts = std::make_shared<std::atomic_uint>(0U);
			auto ownedController = std::make_unique<EditorExportOperationController>([wait = gate.MakeWaiter(), attempts, &lateProgress, &cancellation](const ExportRequest&, ExportProgressCallback progress, std::shared_ptr<ExportCancellationState> token) {
				if (attempts->fetch_add(1U) == 0U) throw std::runtime_error("retained historical worker failure");
				lateProgress = progress; cancellation = token;
				(void)wait(); ExportResult result; result.success = token->Commit([] { return true; }); return result;
			});
			auto& controller = *ownedController;
			// Teardown settles the active operation before expiring owned history.
			// Already copied receipts remain valid independently of controller state.
			const auto historical = controller.Admit(Request(controller));
			context.Expect(historical.accepted && AwaitFinal(controller, historical.operationId), "The historical worker failure must become a final receipt.");
			controller.Poll();
			const auto prior = controller.Observe(historical.operationId);
			context.Expect(prior && prior->result && prior->result->failureCategory == ExportFailureCategory::UnexpectedFailure
				&& prior->result->message == "retained historical worker failure", "Repeated adoption must preserve consumed worker exception truth.");
			const auto admission = controller.Admit(Request(controller));
			context.Expect(admission.accepted && gate.WaitForArrivals(1), "The active export should reach its controlled closing boundary.");
			// A worker releases only after observing the actual cancellation request;
			// the protected owner can now prove drain/cancel ordering without sleeps.
			std::thread release([&] { while (!cancellation->requested.load()) std::this_thread::yield(); gate.Release(); });
			controller.StopAndDrain(); release.join();
			context.Expect(prior->result->message == "retained historical worker failure" && controller.Lookup(historical.operationId).disposition == EditorExportLookupDisposition::Expired, "Draining must expire ownership without rewriting copied historical failure.");
			context.Expect(cancellation->requested.load() && !cancellation->WasCommitted() && controller.Lookup(admission.operationId).disposition == EditorExportLookupDisposition::Expired, "Owner close must cancel and settle before expiring active ownership.");
			lateProgress({});
			context.Expect(controller.Lookup(admission.operationId).disposition == EditorExportLookupDisposition::Expired, "Late worker callbacks cannot republish expired observations.");
			context.Expect(!controller.TakeLaunch(admission.operationId) && !controller.Admit(Request(controller)).accepted, "Stopped owners must reject launch and new admission.");
			controller.StopAndDrain(); controller.BeginProject();
			context.Expect(controller.Generation() != prior->intent.projectGeneration, "Replacement must advance the generation before admission reopens.");
			ownedController.reset();
			lateProgress({}); // The callback owns shared mailbox data after owner destruction.
		});
		registry.Add("Editor.ExportLifecycle.StopInvalidatesUnconsumedSuccessfulLaunch", [](TestContext& context) {
			PoolSession pool;
			EditorExportOperationController controller([](const ExportRequest&, ExportProgressCallback, std::shared_ptr<ExportCancellationState> cancellation) {
				ExportResult result; result.success = cancellation->Commit([] { return true; }); return result;
			});
			const auto admission = controller.Admit(Request(controller));
			context.Expect(admission.accepted && AwaitFinal(controller, admission.operationId), "The captured launch request should commit successfully.");
			const auto copied = controller.Observe(admission.operationId);
			controller.StopAndDrain();
			context.Expect(copied->result->success && controller.Lookup(admission.operationId).disposition == EditorExportLookupDisposition::Expired && !controller.TakeLaunch(admission.operationId), "Shutdown preserves copied committed success while expiring history and invalidating an unconsumed launch.");
		});
		registry.Add("Editor.ExportLifecycle.OutputCommitCancellationBoundary", [](TestContext& context) {
			DisposableFixturePath files;
			for (const bool cancelFirst : {true, false}) {
				ExportOutputTransaction transaction(files.Child(cancelFirst ? "canceled" : "committed"), cancelFirst ? "cancel-first" : "commit-first");
				std::string error; context.Expect(transaction.Begin(error), "The production output transaction should own fresh staging.");
				std::ofstream(transaction.StagingDirectory() / "output.txt") << "complete-output";
				context.Expect(transaction.MarkStagedAndValidated(error), "Fresh staged output should reach validation.");
				auto cancellation = std::make_shared<ExportCancellationState>();
				if (cancelFirst) cancellation->RequestCancellation();
				const bool committed = transaction.Commit(error, cancellation);
				context.Expect(committed != cancelFirst, "The first protected cancellation/commit decision should win.");
				if (cancelFirst) { context.Expect(transaction.Rollback(error), "Canceled staging should roll back."); context.Expect(!std::filesystem::exists(transaction.FinalDirectory() / "output.txt"), "Canceled output must stay invisible."); }
				else context.Expect(cancellation->RequestCancellation() == ExportCancellationDisposition::TooLate && std::filesystem::is_regular_file(transaction.FinalDirectory() / "output.txt"), "Published output must remain successful after late cancellation.");
			}
		});
	}
}
