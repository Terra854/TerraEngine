// Scope: service operation classification through real async IO and Update.
// Fixture ingress configures only a fresh service's project/root lifecycle. No
// selected capture, operation, completion or fake IO failure is injected.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Profiling/EditorProfilerService.h"
#include "Profiling/ProfilerJournalV1.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <thread>

namespace TerraEngine::EditorTests {
	struct ProfilerOperationFailureTestAccess {
		static void InitializeOwnedRoot(EditorProfilerService& service, const std::filesystem::path& root) {
			std::lock_guard<std::mutex> lock(service.mutex);
			// Same root/cancel/generation primitives used by OnProjectOpened. This
			// fresh fixture has no prior selections or requests to reset. It never
			// constructs ProjectManager or touches a user's recent-project settings.
			++service.projectGeneration;
			service.cancellationToken->store(true, std::memory_order_release);
			service.cancellationToken = std::make_shared<std::atomic_bool>(false);
			service.captureStore.SetProjectRoot(root);
			service.sessionAccepting = true;
			service.PublishPresentationLocked();
		}
		static void CancelOwnedRoot(EditorProfilerService& service) {
			std::lock_guard<std::mutex> lock(service.mutex);
			service.cancellationToken->store(true, std::memory_order_release);
			// Do not call project Shutdown: that clears the unrelated singleton
			// Profiler. Fixture workers own copied paths and weak service handles.
		}
	};
	namespace {
		using namespace TerraEngine::Profiling;
		std::size_t OperationIndex(EditorProfilerOperationKind kind) { return static_cast<std::size_t>(kind) - 1; }
		// Service-only tests do not start Engine. Borrow an existing executor, or
		// own a small real worker session until all fixture service owners retire.
		// This follows the editor capture-service fixture convention and never
		// shuts down another fixture's already-running shared executor.
		struct OperationWorkerSession final {
			bool ownsExecutor = ThreadPool::instance().getThreadCount() == 0U;
			OperationWorkerSession() { if(ownsExecutor) ThreadPool::instance().initialize(2U); }
			OperationWorkerSession(const OperationWorkerSession&) = delete;
			OperationWorkerSession& operator=(const OperationWorkerSession&) = delete;
			~OperationWorkerSession() { if(ownsExecutor) ThreadPool::instance().shutdown(); }
		};
		struct OperationFixture {
			// Declared first, released last: cancellation/service destruction below
			// precede retirement of the executor we exclusively started.
			OperationWorkerSession workers;
			std::filesystem::path root, journal;
			std::shared_ptr<EditorProfilerService> service = std::make_shared<EditorProfilerService>();
			bool safelySettled = true;
			OperationFixture(TestContext& context) {
				context.Expect(Profiler::Instance().GetState() != ProfilerSessionState::Recording
					&& Profiler::Instance().GetState() != ProfilerSessionState::Paused, "Operation fixture requires no active profiler recording; it never clears global state.");
				static std::atomic_uint64_t next{0}; const auto seed = std::chrono::steady_clock::now().time_since_epoch().count(); bool owned = false;
				for(unsigned attempt = 0; attempt < 100 && !owned; ++attempt) {
					root = std::filesystem::temp_directory_path() / ("TerraProfilerOperation-" + std::to_string(seed) + "-" + std::to_string(next++));
					owned = std::filesystem::create_directory(root);
				}
				context.Expect(owned, "Must exclusively reserve fixture-owned project root.");
				std::filesystem::create_directory(root / "Profiler"); journal = root / "Profiler/status.teprof";
				WriteJournal(context); ProfilerOperationFailureTestAccess::InitializeOwnedRoot(*service, root);
				service->LoadCapture(journal); Pump(context, EditorProfilerOperationKind::Opening);
				const auto snapshot = service->GetPresentationSnapshot();
				context.Expect(snapshot && snapshot->selectedCapture && snapshot->indexPage && snapshot->selectedSummary,
					"Actual opening worker must discover/read selected journal before fault requests.");
			}
			~OperationFixture() {
				ProfilerOperationFailureTestAccess::CancelOwnedRoot(*service); service.reset();
				// After a timeout retain only our owned evidence directory: never race
				// cleanup with an unsettled read. Only our own worker lease retires later.
				if(safelySettled) { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
			}
			void WriteJournal(TestContext& context) {
				ProfilerJournalV1Writer writer; std::string error;
				context.Expect(writer.Open(journal, {}, error) == ProfilerJournalV1ResultCode::Ok, error);
				std::vector<ProfilerSealedFrame> frames(3);
				for(unsigned i = 0; i < frames.size(); ++i) { frames[i].frame.frameIndex = 17 + i; frames[i].frame.durationMs = 1; frames[i].ownedByteCount = EstimateProfilerFrameOwnedBytes(frames[i].frame); }
				context.Expect(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok, error);
				context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok, error);
			}
			void Pump(TestContext& context, EditorProfilerOperationKind kind) {
				safelySettled = false; const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
				while(std::chrono::steady_clock::now() < end) {
					service->Update(0.0F); const auto snapshot = service->GetPresentationSnapshot();
					if(snapshot && snapshot->operations[OperationIndex(kind)].operationId != 0 && !snapshot->operations[OperationIndex(kind)].active) { safelySettled = true; return; }
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				context.Expect(false, "Actual profiler worker/Update publication must settle within5seconds; owned root retained: " + root.string());
			}
		};
		std::string Lower(std::string value) { std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); return value; }
		void ExpectFailure(TestContext& context, OperationFixture& fixture, EditorProfilerOperationKind kind) {
			const auto snapshot = fixture.service->GetPresentationSnapshot(); context.Expect(snapshot != nullptr, "Actual immutable presentation must be published.");
			const auto& operation = snapshot->operations[OperationIndex(kind)];
			context.Expect(operation.kind == kind && operation.state == EditorProfilerOperationState::Failed && !operation.active && operation.operationId != 0,
				"Real IO failure must settle the matching typed operation.");
			context.Expect(!snapshot->lastError.empty() && snapshot->lastError == fixture.service->GetLastError(), "Actual IO error must be retained in copied and service diagnostics.");
			const auto text = Lower(snapshot->statusText);
			const bool classified = kind == EditorProfilerOperationKind::LoadingIndex ? text.find("index") != std::string::npos
				: text.find("frame") != std::string::npos || text.find("detail") != std::string::npos;
			context.Expect(classified && text.find("refresh capture list") == std::string::npos && snapshot->statusText == fixture.service->GetStatusText(),
				"Displayed failure must identify the actual read operation; status=" + snapshot->statusText + "; reason=" + snapshot->lastError);
		}
	}
	void RegisterProfilerOperationFailureStatusTests(TestRegistry& registry) {
		for(const auto kind : {EditorProfilerOperationKind::LoadingIndex, EditorProfilerOperationKind::LoadingDetail}) {
			const std::string name = kind == EditorProfilerOperationKind::LoadingIndex ? "Editor.ProfilerOperationFailure.IndexReadKeepsActualClassification" : "Editor.ProfilerOperationFailure.DetailReadKeepsActualClassification";
			registry.Add(name, [kind](TestContext& context) {
				OperationFixture fixture(context); const auto before = fixture.service->GetPresentationSnapshot();
				context.Expect(std::filesystem::remove(fixture.journal), "Remove only our fixture-owned journal after actual selection.");
				if(kind == EditorProfilerOperationKind::LoadingIndex) fixture.service->RequestIndexPage(1, 1); else fixture.service->RequestSelectedFrame(18);
				fixture.Pump(context, kind); const auto after = fixture.service->GetPresentationSnapshot();
				context.Expect(after->selectedCapture && after->selectedCapture->artifactId == before->selectedCapture->artifactId,
					"Failed read must preserve the previously selected capture identity.");
				ExpectFailure(context, fixture, kind);
			});
		}
		registry.Add("Editor.ProfilerOperationFailure.RealIndexAndDetailSuccessControl", [](TestContext& context) {
			OperationFixture fixture(context); fixture.service->RequestIndexPage(1, 1); fixture.Pump(context, EditorProfilerOperationKind::LoadingIndex);
			auto snapshot = fixture.service->GetPresentationSnapshot();
			context.Expect(snapshot->operations[OperationIndex(EditorProfilerOperationKind::LoadingIndex)].state == EditorProfilerOperationState::Completed
				&& snapshot->indexPage && snapshot->indexPage->offset == 1 && snapshot->lastError.empty(), "Actual index control must load requested bounded page.");
			fixture.service->RequestSelectedFrame(18); fixture.Pump(context, EditorProfilerOperationKind::LoadingDetail); snapshot = fixture.service->GetPresentationSnapshot();
			context.Expect(snapshot->operations[OperationIndex(EditorProfilerOperationKind::LoadingDetail)].state == EditorProfilerOperationState::Completed
				&& snapshot->selectedFrame && snapshot->selectedFrame->frameIndex == 18 && snapshot->lastError.empty(), "Actual detail control must publish requested frame.");
		});
	}
}
