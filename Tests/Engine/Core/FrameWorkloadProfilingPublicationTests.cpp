// CPU metadata transport and actual public profiler/scheduler lifecycle tests.
// Unit keys below are explicit transport inputs, never manufactured runtime IDs.
#include "TestHarness.h"
#include "Core/Scheduling/FrameWorkloadProfiling.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Profiling/Profiler.h"

#include <string>
#include <vector>

namespace TerraEngine::Tests {
	// BEGIN exact primitive test cohort (standalone source-extraction boundary).
	namespace {
		using namespace TerraEngine::Scheduling;
		using namespace TerraEngine::Profiling;
		struct TransportLease {
			TransportLease() { InvalidateFrameWorkloadProfilingCapture(); }
			~TransportLease() { InvalidateFrameWorkloadProfilingCapture(); }
		};
		std::vector<WorkloadExecutionRecord> TransportRows(std::string label = "unit-input") {
			WorkloadExecutionRecord row;
			row.frameId = label; row.descriptorId = "transport/row"; row.descriptorGeneration = 1u;
			row.ownerSystem = "TransportUnitInput"; row.phase = "Scene"; row.workloadName = label;
			return {std::move(row)};
		}
	}
	void RegisterFrameWorkloadProfilingPrimitiveTests(TestRegistry& registry) {
		registry.Add("Engine.WorkloadPublication.AbsentOrRegisteredIsUnavailable", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 11u};
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Never published is unavailable.");
			BeginFrameWorkloadProfilingCapture(key);
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Registration alone is not collected-empty evidence.");
		});
		registry.Add("Engine.WorkloadPublication.CollectedEmptyAvailableExactlyOnce", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 12u};
			BeginFrameWorkloadProfilingCapture(key); const auto prepared = PrepareFrameWorkloadProfilingPublication();
			PublishFrameWorkloadProfilingSnapshot(prepared, {}); const auto snapshot = TakeLatestFrameWorkloadProfilingSnapshot(key);
			context.Expect(snapshot.available && snapshot.records.empty() && snapshot.identity == key, "Actual accepted empty publication differs from absence.");
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "A consumed publication is not observed twice.");
		});
		registry.Add("Engine.WorkloadPublication.NonemptyCopiesIndependentOwnedRows", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 13u}; auto rows = TransportRows();
			BeginFrameWorkloadProfilingCapture(key); PublishFrameWorkloadProfilingSnapshot(PrepareFrameWorkloadProfilingPublication(), rows);
			rows.front().workloadName = "mutated caller input"; const auto snapshot = TakeLatestFrameWorkloadProfilingSnapshot(key);
			context.Expect(snapshot.available && snapshot.identity == key && snapshot.records.size() == 1u
				&& snapshot.records.front().workloadName == "unit-input", "Typed snapshot owns copied values, not producer references.");
		});
		registry.Add("Engine.WorkloadPublication.WrongIdentityCannotConsumeCurrent", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 14u}; const auto rows = TransportRows();
			BeginFrameWorkloadProfilingCapture(key); PublishFrameWorkloadProfilingSnapshot(PrepareFrameWorkloadProfilingPublication(), rows);
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot({8u, 14u}).available && !TakeLatestFrameWorkloadProfilingSnapshot({7u, 15u}).available,
				"Wrong session or frame is unavailable.");
			context.Expect(TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Mismatched reader preserves the actual current snapshot.");
		});
		registry.Add("Engine.WorkloadPublication.LateOldReadersAndPublishersPreserveNewFrame", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity old{7u, 16u}, current{8u, 17u};
			BeginFrameWorkloadProfilingCapture(old); const auto delayed = PrepareFrameWorkloadProfilingPublication();
			BeginFrameWorkloadProfilingCapture(current); PublishFrameWorkloadProfilingSnapshot(PrepareFrameWorkloadProfilingPublication(), TransportRows("current"));
			PublishFrameWorkloadProfilingSnapshot(delayed, TransportRows("stale"));
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(old).available, "Stale consumer stays unavailable.");
			const auto snapshot = TakeLatestFrameWorkloadProfilingSnapshot(current);
			context.Expect(snapshot.available && snapshot.records.size() == 1u && snapshot.records.front().frameId == "current", "Late old work cannot clear/replace a newer accepted snapshot.");
		});
		registry.Add("Engine.WorkloadPublication.InvalidationAndTakeCloseAgainstLatePublication", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 18u};
			BeginFrameWorkloadProfilingCapture(key); const auto delayed = PrepareFrameWorkloadProfilingPublication();
			InvalidateFrameWorkloadProfilingCapture(); PublishFrameWorkloadProfilingSnapshot(delayed, TransportRows());
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Stopped scope cannot be resurrected.");
			BeginFrameWorkloadProfilingCapture(key); const auto fresh = PrepareFrameWorkloadProfilingPublication();
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Unpublished take closes this scope.");
			PublishFrameWorkloadProfilingSnapshot(fresh, TransportRows());
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "Late publisher cannot resurrect an already-consumed key.");
		});
		registry.Add("Engine.WorkloadPublication.OversizeUnavailableWithoutTruncatedSuccess", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 19u};
			std::vector<WorkloadExecutionRecord> rows(kMaximumWorkloadExecutionRecordsPerFrame);
			BeginFrameWorkloadProfilingCapture(key); PublishFrameWorkloadProfilingSnapshot(PrepareFrameWorkloadProfilingPublication(), rows);
			const auto exact = TakeLatestFrameWorkloadProfilingSnapshot(key);
			context.Expect(exact.available && exact.records.size() == kMaximumWorkloadExecutionRecordsPerFrame, "Exact existing record bound is supported.");
			BeginFrameWorkloadProfilingCapture(key); const auto prepared = PrepareFrameWorkloadProfilingPublication();
			PublishFrameWorkloadProfilingSnapshot(prepared, TransportRows()); rows.emplace_back(); PublishFrameWorkloadProfilingSnapshot(prepared, rows);
			const auto oversized = TakeLatestFrameWorkloadProfilingSnapshot(key);
			context.Expect(!oversized.available && oversized.records.empty(), "Oversize replacement cannot retain prior success or partially truncate to a complete result.");
		});
		registry.Add("Engine.WorkloadPublication.PrepareRemovesPriorPassBeforeValidation", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{7u, 20u};
			BeginFrameWorkloadProfilingCapture(key); PublishFrameWorkloadProfilingSnapshot(PrepareFrameWorkloadProfilingPublication(), TransportRows());
			context.Expect(PrepareFrameWorkloadProfilingPublication() == key, "Repeat pass keeps the actual capture identity.");
			context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(key).available, "A prepared pass without subsequent actual publication cannot reuse its predecessor.");
		});
	}
	// END exact primitive test cohort.

	namespace {
		struct ProfilerSessionLease {
			Profiler& profiler = Profiler::Instance();
			explicit ProfilerSessionLease(TestContext& context) {
				context.Expect(profiler.GetState() != ProfilerSessionState::Recording && profiler.GetState() != ProfilerSessionState::Paused,
					"Public lifecycle fixture requires no unrelated active recording.");
				profiler.Clear(); ProfilerCaptureSettings settings; settings.maxFrames = 4u;
				profiler.StartRecording({}, settings);
			}
			~ProfilerSessionLease() { profiler.AbandonFrameAfterFailure(); (void)profiler.FinishRecording(); profiler.Clear(); }
			ProfilerFrameRecord FinishFrame(TestContext& context) {
				profiler.EndFrame(); const auto frames = profiler.GetFramesSnapshot();
				context.Expect(!frames.empty(), "Real EndFrame must retain its owned in-memory frame."); return frames.back();
			}
		};
		class PublicationSystem final : public SystemsBase {
		public:
			unsigned calls = 0u;
			bool overflowRecords = false;
			void Initialize() override {}
			void Shutdown() override {}
			void Update(float) override { ++calls; }
			std::string SystemName() override { return "PublicationSystem"; }
			std::vector<FrameWorkloadDeclaration> DeclareFrameWorkloads(float) override {
				// Fixture-only protected callback writes one owned scalar; it has no
				// live engine resources, worker mutation, new phase or flush point.
				auto work = MakeProtectedCommitPhase("PublicationSystem", "PublicationSystem", {}, {}, "Owned test counter only.");
				if (overflowRecords) {
					// Protected work deliberately stays single-range in the real executor.
					// Distinct owned declarations exceed the existing diagnostic bound
					// through real callbacks, without starting workers or altering limits.
					std::vector<FrameWorkloadDeclaration> workloads;
					workloads.reserve(kMaximumWorkloadExecutionRecordsPerFrame + 1u);
					for (std::size_t index = 0; index <= kMaximumWorkloadExecutionRecordsPerFrame; ++index) {
						auto item = MakeProtectedCommitPhase("PublicationSystem-" + std::to_string(index),
							"PublicationSystem", {}, {}, "Owned test counter only.");
						item.work = [this](JobGraphContext&) { ++calls; };
						workloads.push_back(std::move(item));
					}
					return workloads;
				}
				work.work = [this](JobGraphContext&) { ++calls; };
				return {std::move(work)};
			}
		};
		SystemDeclaration PublicationDeclaration() {
			SystemDeclaration declaration; declaration.name = "PublicationSystem"; declaration.phase = "Scene";
			declaration.mainThread = true; declaration.usesDefaultMetadata = false; return declaration;
		}
	}
	void RegisterFrameWorkloadProfilingPublicationTests(TestRegistry& registry) {
		RegisterFrameWorkloadProfilingPrimitiveTests(registry);
		registry.Add("Engine.WorkloadPublication.LegacyTransferAndDisabledCollectionStayIndependent", [](TestContext& context) {
			TransportLease lease; const FrameWorkloadProfilingIdentity key{9u, 1u};
			FrameWorkloadDeclaration work = MakeProtectedCommitPhase("Legacy", "Legacy", {}, {}, "Owned CPU callback only.");
			work.work = [](JobGraphContext&) {};
			const auto plan = ExecuteFrameWorkloadsCompatibility({work}, "legacy-source");
			const auto first = TakeLatestFrameWorkloadExecutionRecordsForProfiling(); const auto second = TakeLatestFrameWorkloadExecutionRecordsForProfiling();
			context.Expect(plan.valid && plan.completed && !first.empty() && first.size() == plan.executionRecords.size() && second.empty(), "Historical vector wrapper keeps exact transfer-once semantics.");
			BeginFrameWorkloadProfilingCapture(key); PrepareFrameWorkloadProfilingPublication(); FrameWorkloadExecutionOptions options; options.collectExecutionRecords = false;
			const auto disabled = ExecuteFrameWorkloadsCompatibility({work}, "disabled-source", options);
			context.Expect(disabled.valid && disabled.completed && disabled.executionRecords.empty() && !TakeLatestFrameWorkloadProfilingSnapshot(key).available,
				"Real collection-disabled generic executor cannot invent observed-empty profiler publication.");
		});
		registry.Add("Engine.WorkloadPublication.RealProfilerWithoutSchedulerUnavailable", [](TestContext& context) {
			ProfilerSessionLease session(context); session.profiler.BeginFrame(); const auto frame = session.FinishFrame(context);
			context.Expect(!frame.workloadExecutionsAvailable && frame.workloadExecutions.empty(), "Actual frame without a scheduler pass stays unavailable.");
		});
		registry.Add("Engine.WorkloadPublication.RealProfilerCollectedEmptySchedulerAvailable", [](TestContext& context) {
			ProfilerSessionLease session(context); SystemScheduler scheduler; scheduler.SetMutationFlushPoints({}); session.profiler.BeginFrame();
			const auto& summary = scheduler.ExecuteFrame(0.0F, {}); const auto frame = session.FinishFrame(context);
			context.Expect(summary.valid && scheduler.LatestFrameExecutionPlan().completed && scheduler.LatestFrameExecutionPlan().executionRecords.empty(), "Empty real scheduler fixture must execute a valid collected-empty plan.");
			context.Expect(frame.workloadExecutionsAvailable && frame.workloadExecutions.empty(), "Actual observed empty scheduler pass differs from absent publication.");
		});
		registry.Add("Engine.WorkloadPublication.RealProfilerNonemptySchedulerMatchesActualRows", [](TestContext& context) {
			ProfilerSessionLease session(context); SystemScheduler scheduler; scheduler.SetMutationFlushPoints({}); PublicationSystem system; scheduler.RegisterSystem(system, PublicationDeclaration());
			session.profiler.BeginFrame(); const auto& summary = scheduler.ExecuteFrame(0.0F, {}); const auto frame = session.FinishFrame(context);
			context.Expect(summary.valid && system.calls == 1u && !scheduler.LatestFrameExecutionPlan().executionRecords.empty(), "Actual protected fixture callback and real execution records must exist.");
			context.Expect(frame.workloadExecutionsAvailable && frame.workloadExecutions.size() == scheduler.LatestFrameExecutionPlan().executionRecords.size()
				&& frame.workloadExecutions.front().frameId == scheduler.LatestFrameExecutionPlan().frameId, "Actual profiler consumes the matching scheduler publication, not a legacy vector guess.");
		});
		registry.Add("Engine.WorkloadPublication.RealInvalidScheduleCannotReusePriorSameFramePass", [](TestContext& context) {
			ProfilerSessionLease session(context); SystemScheduler scheduler; scheduler.SetMutationFlushPoints({}); session.profiler.BeginFrame();
			context.Expect(scheduler.ExecuteFrame(0.0F, {}).valid, "Initial real scheduler pass must succeed.");
			PublicationSystem system; auto declaration = PublicationDeclaration(); declaration.after.push_back("MissingOwnedDependency"); scheduler.RegisterSystem(system, declaration);
			context.Expect(!scheduler.ExecuteFrame(0.0F, {}).valid, "Actual invalid dependency must reject before execution."); const auto frame = session.FinishFrame(context);
			context.Expect(!frame.workloadExecutionsAvailable && frame.workloadExecutions.empty() && system.calls == 0u, "Rejected repeat pass invalidates prior same-frame publication without executing callbacks.");
		});
		registry.Add("Engine.WorkloadPublication.RealDroppedSchedulerRowsRemainUnavailable", [](TestContext& context) {
			ProfilerSessionLease session(context); SystemScheduler scheduler; scheduler.SetMutationFlushPoints({});
			PublicationSystem system; system.overflowRecords = true; scheduler.RegisterSystem(system, PublicationDeclaration());
			session.profiler.BeginFrame(); const auto& summary = scheduler.ExecuteFrame(0.0F, {}); const auto frame = session.FinishFrame(context);
			const auto& plan = scheduler.LatestFrameExecutionPlan();
			context.Expect(summary.valid && plan.completed && plan.droppedExecutionRecordCount == 1u
				&& plan.executionRecords.size() == kMaximumWorkloadExecutionRecordsPerFrame
				&& system.calls == kMaximumWorkloadExecutionRecordsPerFrame + 1u, "Actual bounded protected pass must complete while dropping exactly its final diagnostic row; summaryValid="
				+ std::to_string(summary.valid) + "; planValid=" + std::to_string(plan.valid)
				+ "; completed=" + std::to_string(plan.completed) + "; rows=" + std::to_string(plan.executionRecords.size())
				+ "; dropped=" + std::to_string(plan.droppedExecutionRecordCount) + "; callbacks=" + std::to_string(system.calls)
				+ "; descriptors=" + std::to_string(plan.results.size()));
			context.Expect(!frame.workloadExecutionsAvailable && frame.workloadExecutions.empty(), "A real capped/truncated plan cannot become complete profiler evidence.");
		});
		registry.Add("Engine.WorkloadPublication.RealPauseAbandonFinishAndClearRetireKeys", [](TestContext& context) {
			for(unsigned action = 0; action < 4u; ++action) {
				ProfilerSessionLease session(context); session.profiler.BeginFrame(); const auto actualKey = PrepareFrameWorkloadProfilingPublication();
				context.Expect(actualKey.sessionGeneration != 0u && actualKey.frameToken != 0u, "Key must come from actual BeginFrame, never a test-assigned runtime identity.");
				if(action == 0u) session.profiler.PauseRecording();
				if(action == 1u) session.profiler.AbandonFrameAfterFailure();
				if(action == 2u) (void)session.profiler.FinishRecording();
				if(action == 3u) session.profiler.Clear();
				PublishFrameWorkloadProfilingSnapshot(actualKey, TransportRows("late-unit-payload"));
				context.Expect(!TakeLatestFrameWorkloadProfilingSnapshot(actualKey).available, "Actual inactive lifecycle must close the prepared key against late publication.");
			}
		});
	}
}
