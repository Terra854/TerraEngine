#include "TestHarness.h"

#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class ThreadPoolSession {
		public:
			ThreadPoolSession()
			{
				// The job graph uses the engine ThreadPool singleton. Restart it
				// per test so worker availability and queued work are deterministic.
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession()
			{
				ThreadPool::instance().shutdown();
			}

			ThreadPoolSession(const ThreadPoolSession&) = delete;
			ThreadPoolSession& operator=(const ThreadPoolSession&) = delete;
		};

		class FakeSystem final : public SystemsBase {
		public:
			explicit FakeSystem(std::string systemName)
				: name(std::move(systemName))
			{
			}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}

			std::string SystemName() override
			{
				return name;
			}

		private:
			std::string name;
		};

		TerraEngine::Scheduling::JobGraphJobDeclaration MakeJob(std::string name, TerraEngine::Scheduling::JobGraphExecutionPolicy policy, std::vector<std::string>* log = nullptr)
		{
			TerraEngine::Scheduling::JobGraphJobDeclaration job;
			job.name = std::move(name);
			job.executionPolicy = policy;
			job.profilingLabel = job.name;
			job.work = [name = job.name, log](TerraEngine::Scheduling::JobGraphContext&) {
				if (log != nullptr)
				{
					log->push_back(name);
				}
			};
			return job;
		}

		TerraEngine::Scheduling::SystemDeclaration MakeSystemDeclaration(std::string name, std::string phase)
		{
			TerraEngine::Scheduling::SystemDeclaration declaration;
			declaration.name = std::move(name);
			declaration.phase = std::move(phase);
			declaration.mainThread = true;
			declaration.profilingLabel = declaration.name;
			declaration.usesDefaultMetadata = false;
			return declaration;
		}

		bool HasJobState(const TerraEngine::Scheduling::JobGraphSummary& summary, const std::string& name, TerraEngine::Scheduling::JobGraphNodeState state)
		{
			return std::any_of(summary.jobs.begin(), summary.jobs.end(), [&](const TerraEngine::Scheduling::JobGraphResultEntry& entry) {
				return entry.name == name && entry.state == state;
			});
		}

		bool HasBarrier(const TerraEngine::Scheduling::JobGraphSummary& summary, const std::string& name)
		{
			return std::any_of(summary.barriers.begin(), summary.barriers.end(), [&](const TerraEngine::Scheduling::JobGraphResultEntry& entry) {
				return entry.name == name;
			});
		}

		bool HasGroup(const TerraEngine::Scheduling::JobGraphSummary& summary, const std::string& name)
		{
			return std::any_of(summary.taskGroups.begin(), summary.taskGroups.end(), [&](const TerraEngine::Scheduling::JobGraphResultEntry& entry) {
				return entry.name == name;
			});
		}

		bool HasDiagnostic(const TerraEngine::Scheduling::JobGraphSummary& summary, TerraEngine::Scheduling::JobGraphDiagnosticCategory category)
		{
			return std::any_of(summary.diagnostics.begin(), summary.diagnostics.end(), [&](const TerraEngine::Scheduling::JobGraphDiagnostic& diagnostic) {
				return diagnostic.category == category;
			});
		}
	}

	void RegisterJobGraphTests(TestRegistry& registry)
	{
		registry.Add("Engine.JobGraph.ValidDependenciesCompleteDeterministically", [](TestContext& context) {
			ThreadPoolSession session;
			std::vector<std::string> firstLog;
			TerraEngine::Scheduling::JobGraph graph("valid", "Valid Graph");
			auto a = graph.AddJob(MakeJob("A", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, &firstLog));
			auto b = graph.AddJob(MakeJob("B", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, &firstLog));
			auto cJob = MakeJob("C", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, &firstLog);
			cJob.dependencies = { a, b };
			graph.AddJob(std::move(cJob));

			const TerraEngine::Scheduling::JobGraphSummary first = graph.Execute();
			const TerraEngine::Scheduling::JobGraphSummary second = graph.Execute();

			context.Expect(first.valid && first.completed, "Valid dependency graph should execute to completion.");
			context.Expect(a.IsValid() && b.IsValid(), "Returned job handles should be valid graph-local dependency handles.");
			context.Expect(HasJobState(first, "C", TerraEngine::Scheduling::JobGraphNodeState::Completed), "Dependent job should complete after prerequisites.");
			context.ExpectEqual(first.formatted, second.formatted, "Repeated graph execution should keep summary text deterministic.");
		});

		registry.Add("Engine.JobGraph.RearmReusesValidatedTopologyWithFreshCallbacks", [](TestContext& context) {
			std::vector<std::string> firstLog;
			std::vector<std::string> secondLog;
			TerraEngine::Scheduling::JobGraph graph("rearm-first", "Reusable Graph");
			auto first = graph.AddJob(MakeJob(
				"First", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected,
				&firstLog));
			auto second = graph.AddJob(MakeJob(
				"Second", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected,
				&firstLog));
			graph.AddDependency(first, second);
			const TerraEngine::Scheduling::JobGraphSummary initial = graph.Execute();

			graph.Rearm("rearm-second");
			context.Expect(graph.UpdateJobWork(first,
				[&secondLog](TerraEngine::Scheduling::JobGraphContext&) {
					secondLog.push_back("First");
				}), "A retained job handle should accept the next frame callback.");
			context.Expect(graph.UpdateJobWork(second,
				[&secondLog](TerraEngine::Scheduling::JobGraphContext&) {
					secondLog.push_back("Second");
				}), "Every retained job should refresh its frame-local callback.");
			const TerraEngine::Scheduling::JobGraphSummary reused = graph.Execute();

			context.Expect(initial.valid && initial.completed && reused.valid && reused.completed,
				"Both the initial and retained topology executions should complete.");
			context.Expect(firstLog == std::vector<std::string>({ "First", "Second" })
				&& secondLog == std::vector<std::string>({ "First", "Second" }),
				"Rearming must preserve dependency order while replacing every callback.");
		});

		registry.Add("Engine.JobGraph.MissingHandleAndCycleAreRejected", [](TestContext& context) {
			TerraEngine::Scheduling::JobGraph missing("missing", "Missing Handle");
			auto job = MakeJob("UsesMissing", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
			TerraEngine::Scheduling::DependencyHandle stale;
			stale.id = 99;
			stale.generation = 1;
			stale.kind = TerraEngine::Scheduling::JobGraphNodeKind::Job;
			stale.label = "Missing";
			job.dependencies.push_back(stale);
			missing.AddJob(std::move(job));
			const TerraEngine::Scheduling::JobGraphSummary& missingSummary = missing.Validate();

			context.Expect(!missingSummary.valid, "Unknown dependency handles should reject validation.");
			context.Expect(HasDiagnostic(missingSummary, TerraEngine::Scheduling::JobGraphDiagnosticCategory::Dependency), "Missing handle should produce a dependency diagnostic.");

			TerraEngine::Scheduling::JobGraph cycle("cycle", "Cycle");
			auto a = cycle.AddJob(MakeJob("A", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected));
			auto b = cycle.AddJob(MakeJob("B", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected));
			cycle.AddDependency(a, b);
			cycle.AddDependency(b, a);

			const TerraEngine::Scheduling::JobGraphSummary& cycleSummary = cycle.Validate();
			context.Expect(!cycleSummary.valid, "Dependency cycles should reject validation.");
			context.Expect(HasDiagnostic(cycleSummary, TerraEngine::Scheduling::JobGraphDiagnosticCategory::Dependency), "Cycle should produce a dependency diagnostic.");
		});

		registry.Add("Engine.JobGraph.WorkerJobsOverlapBeforeDependentFinalJob", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("overlap", "Overlap Stress");
			std::atomic<int> active = 0;
			std::atomic<int> maxActive = 0;
			std::atomic<int> completed = 0;
			std::vector<TerraEngine::Scheduling::DependencyHandle> workers;

			for (int index = 0; index < 32; ++index)
			{
				TerraEngine::Scheduling::JobGraphJobDeclaration job;
				job.name = "Worker" + std::to_string(index);
				job.executionPolicy = TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible;
				job.profilingLabel = job.name;
				job.work = [&active, &maxActive, &completed](TerraEngine::Scheduling::JobGraphContext&) {
					const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
					int observed = maxActive.load(std::memory_order_acquire);
					while (now > observed && !maxActive.compare_exchange_weak(observed, now, std::memory_order_acq_rel))
					{
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					active.fetch_sub(1, std::memory_order_acq_rel);
					completed.fetch_add(1, std::memory_order_acq_rel);
				};
				workers.push_back(graph.AddJob(std::move(job)));
			}

			TerraEngine::Scheduling::JobGraphJobDeclaration finalJob = MakeJob("Final", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
			finalJob.dependencies = workers;
			finalJob.work = [&completed](TerraEngine::Scheduling::JobGraphContext&) {
				if (completed.load(std::memory_order_acquire) != 32)
				{
					throw std::runtime_error("Final job ran before all workers completed.");
				}
			};
			graph.AddJob(std::move(finalJob));

			for (int run = 0; run < 5; ++run)
			{
				active.store(0, std::memory_order_release);
				maxActive.store(0, std::memory_order_release);
				completed.store(0, std::memory_order_release);
				const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute();
				context.Expect(summary.valid && summary.completed, "Worker stress graph should complete.");
				context.Expect(maxActive.load(std::memory_order_acquire) > 1, "Independent worker jobs should overlap on ThreadPool workers.");
				context.Expect(HasJobState(summary, "Final", TerraEngine::Scheduling::JobGraphNodeState::Completed), "Final dependent job should complete.");
			}
		});

		registry.Add("Engine.JobGraph.BarriersAndGroupsEnforceOrdering", [](TestContext& context) {
			ThreadPoolSession session;
			std::vector<std::string> log;
			TerraEngine::Scheduling::JobGraph graph("barrier", "Barrier");
			auto before = graph.AddJob(MakeJob("Before", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, &log));
			auto afterJob = MakeJob("After", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, &log);
			auto group = graph.AddTaskGroup({ "Group", { before }, TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected, "Group" });
			auto after = graph.AddJob(std::move(afterJob));
			graph.AddBarrier({ "Boundary", TerraEngine::Scheduling::JobGraphBarrierPurpose::Phase, { group }, { after }, "Boundary", false });

			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Barrier graph should execute.");
			context.Expect(HasGroup(summary, "Group"), "Summary should include task group result.");
			context.Expect(HasBarrier(summary, "Boundary"), "Summary should include barrier result.");
			context.ExpectEqual(std::string("Before"), log[0], "Before-barrier job should run first.");
			context.ExpectEqual(std::string("After"), log[1], "After-barrier job should run after the boundary.");
		});

		registry.Add("Engine.JobGraph.MixedWorkerMainThreadGroupIsProtectedOrRejected", [](TestContext& context) {
			TerraEngine::Scheduling::JobGraph invalid("mixed", "Mixed Group");
			auto worker = invalid.AddJob(MakeJob("Worker", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible));
			auto mainThread = invalid.AddJob(MakeJob("Main", TerraEngine::Scheduling::JobGraphExecutionPolicy::MainThread));
			invalid.AddTaskGroup({ "UnsafeMixed", { worker, mainThread }, TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible, "UnsafeMixed" });

			const TerraEngine::Scheduling::JobGraphSummary& invalidSummary = invalid.Validate();
			context.Expect(!invalidSummary.valid, "Worker group that mixes main-thread work should be rejected.");
			context.Expect(HasDiagnostic(invalidSummary, TerraEngine::Scheduling::JobGraphDiagnosticCategory::TaskGroup), "Mixed group should produce task group diagnostics.");

			TerraEngine::Scheduling::JobGraph valid("mixed-protected", "Mixed Protected Group");
			worker = valid.AddJob(MakeJob("Worker", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible));
			mainThread = valid.AddJob(MakeJob("Main", TerraEngine::Scheduling::JobGraphExecutionPolicy::MainThread));
			valid.AddTaskGroup({ "SafeMixed", { worker, mainThread }, TerraEngine::Scheduling::JobGraphExecutionPolicy::MixedProtected, "SafeMixed" });
			context.Expect(valid.Validate().valid, "Mixed-protected group should be accepted.");
		});

		registry.Add("Engine.JobGraph.MutationBarrierReportsPendingMutations", [](TestContext& context) {
			TerraEngine::Scheduling::JobGraph graph("mutation", "Mutation Barrier");
			auto job = graph.AddJob(MakeJob("Scene", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected));
			graph.AddBarrier({ "Frame End", TerraEngine::Scheduling::JobGraphBarrierPurpose::FrameEnd, { job }, {}, "Frame End", true });

			TerraEngine::Scheduling::JobGraphExecuteOptions options;
			options.mutationBarrierCallback = [](const std::string&) {
				TerraEngine::Scheduling::MutationFlushRuntimeState state;
				state.pendingAfterFlush = 1;
				return state;
			};
			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute(options);

			context.Expect(!summary.valid, "Pending mutations at a diagnostic barrier should invalidate the frame graph summary.");
			context.Expect(HasDiagnostic(summary, TerraEngine::Scheduling::JobGraphDiagnosticCategory::Mutation), "Pending mutations should produce a mutation diagnostic.");
		});

		registry.Add("Engine.JobGraph.CancellationBeforeExecutionSkipsPendingWork", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("cancel-before", "Cancel Before");
			auto worker = MakeJob("Worker", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			worker.cancellable = true;
			auto workerHandle = graph.AddJob(std::move(worker));
			auto dependent = MakeJob("Dependent", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
			dependent.dependencies = { workerHandle };
			graph.AddJob(std::move(dependent));
			graph.RequestCancellation("test cancellation");

			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute();

			context.Expect(summary.valid && summary.completed, "Cancelled graph should still reach terminal states.");
			context.Expect(HasJobState(summary, "Worker", TerraEngine::Scheduling::JobGraphNodeState::Cancelled), "Pending cancellable job should be cancelled.");
			context.Expect(HasJobState(summary, "Dependent", TerraEngine::Scheduling::JobGraphNodeState::Blocked), "Dependent should block when required dependency is cancelled.");
		});

		registry.Add("Engine.JobGraph.CancellationDuringExecutionStopsPendingStarts", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("cancel-during", "Cancel During");
			std::atomic<bool> running = false;
			auto longJob = MakeJob("Long", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			longJob.cancellable = false;
			longJob.work = [&running](TerraEngine::Scheduling::JobGraphContext& jobContext) {
				running.store(true, std::memory_order_release);
				for (int i = 0; i < 20 && !jobContext.IsCancellationRequested(); ++i)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			};
			auto longHandle = graph.AddJob(std::move(longJob));
			auto pending = MakeJob("Pending", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
			pending.cancellable = true;
			pending.dependencies = { longHandle };
			graph.AddJob(std::move(pending));

			std::future<TerraEngine::Scheduling::JobGraphSummary> future = std::async(std::launch::async, [&graph]() {
				return graph.Execute();
			});
			while (!running.load(std::memory_order_acquire))
			{
				std::this_thread::yield();
			}
			graph.RequestCancellation("during execution");
			const TerraEngine::Scheduling::JobGraphSummary summary = future.get();

			context.Expect(HasJobState(summary, "Long", TerraEngine::Scheduling::JobGraphNodeState::Completed), "Already-running jobs should finish cooperatively.");
			context.Expect(HasJobState(summary, "Pending", TerraEngine::Scheduling::JobGraphNodeState::Cancelled), "Pending cancellable dependent should not start after cancellation.");
		});

		registry.Add("Engine.JobGraph.FailurePropagatesToRequiredDependents", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("failure", "Failure");
			TerraEngine::Scheduling::JobGraphJobDeclaration failing = MakeJob("Failing", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			failing.work = [](TerraEngine::Scheduling::JobGraphContext&) {
				throw std::runtime_error("expected failure");
			};
			auto failingHandle = graph.AddJob(std::move(failing));
			auto dependent = MakeJob("Dependent", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
			dependent.dependencies = { failingHandle };
			graph.AddJob(std::move(dependent));

			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute();

			context.Expect(!summary.valid, "Failed job should make summary invalid.");
			context.Expect(HasJobState(summary, "Failing", TerraEngine::Scheduling::JobGraphNodeState::Failed), "Throwing job should report failed state.");
			context.Expect(HasJobState(summary, "Dependent", TerraEngine::Scheduling::JobGraphNodeState::Blocked), "Required dependent should be blocked.");
		});

		registry.Add("Engine.JobGraph.SummaryOrderingIsDeterministicAcrossWorkerCompletionOrder", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("deterministic", "Deterministic");
			for (int index = 0; index < 8; ++index)
			{
				TerraEngine::Scheduling::JobGraphJobDeclaration job;
				job.name = "Worker" + std::to_string(index);
				job.executionPolicy = TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible;
				job.profilingLabel = job.name;
				job.work = [index](TerraEngine::Scheduling::JobGraphContext&) {
					std::this_thread::sleep_for(std::chrono::milliseconds(8 - index));
				};
				graph.AddJob(std::move(job));
			}

			const std::string first = graph.Execute().formatted;
			for (int run = 0; run < 10; ++run)
			{
				context.ExpectEqual(first, graph.Execute().formatted, "Worker completion order should not reorder summaries.");
			}
		});

		registry.Add("Engine.JobGraph.SchedulerLoweringPreservesPhasesSystemsAndFlushes", [](TestContext& context) {
			FakeSystem scene("SceneSystem");
			FakeSystem render("RenderSystem");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			auto sceneDeclaration = MakeSystemDeclaration("SceneSystem", "Scene");
			sceneDeclaration.writes.push_back("SceneGraph");
			auto renderDeclaration = MakeSystemDeclaration("RenderSystem", "Render");
			renderDeclaration.reads.push_back("SceneGraph");
			renderDeclaration.after.push_back("SceneSystem");
			scheduler.RegisterSystem(scene, sceneDeclaration);
			scheduler.RegisterSystem(render, renderDeclaration);

			const TerraEngine::Scheduling::ScheduleSummary& schedule = scheduler.BuildSchedule();
			context.Expect(schedule.valid, "Scheduler fixture should be valid before lowering.");

			TerraEngine::Scheduling::JobGraphSchedulerLoweringInput input;
			input.graphId = "lowered";
			input.label = "Lowered";
			input.phases = scheduler.PhaseDefinitions();
			input.orderedSystems = scheduler.OrderedDeclarations();
			input.mutationFlushPoints = scheduler.MutationFlushPoints();
			TerraEngine::Scheduling::JobGraph graph = TerraEngine::Scheduling::JobGraph::FromSchedulerLowering(input);
			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Validate();

			context.Expect(summary.valid, "Valid scheduler schedule should lower into a valid job graph.");
			context.Expect(HasGroup(summary, "Phase: Scene"), "Lowering should create phase task groups.");
			context.Expect(HasJobState(summary, "SceneSystem", TerraEngine::Scheduling::JobGraphNodeState::Pending), "Lowering should create system jobs.");
			context.Expect(HasBarrier(summary, "Mutation: After Scene"), "Lowering should create configured mutation barriers.");
			context.Expect(summary.formatted.find("SceneSystem -> RenderSystem") != std::string::npos, "Lowering should preserve visible system order.");
		});
	}
}
