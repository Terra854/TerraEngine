#include "TestHarness.h"

#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/JobGraphTestAccess.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "ThreadPool/ThreadPool.h"
#include "Runtime/CorrectnessTestSupport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class ThreadPoolSession {
		public:
			explicit ThreadPoolSession(std::size_t workers = 0U)
			{
				// The job graph uses the engine ThreadPool singleton. Restart it
				// per test so worker availability and queued work are deterministic.
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize(workers);
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

	// Release precedes coordinator join on every test exit, including assertions
	// or fixture setup failures. Production workers use only the real ThreadPool.
	namespace {
		using Graph = TerraEngine::Scheduling::JobGraph;
		using Checkpoint = TerraEngine::Scheduling::JobGraphCheckpoint;
		using Access = TerraEngine::Scheduling::JobGraphTestAccess;
		using Policy = TerraEngine::Scheduling::JobGraphExecutionPolicy;
		struct ReleaseAndJoin {
			Correctness::ControlledGate& gate; std::future<std::exception_ptr>& coordinator;
			~ReleaseAndJoin() { gate.Release(); if (coordinator.valid()) coordinator.wait(); }
		};
		struct CoordinatorCause final : std::runtime_error {
			std::shared_ptr<const std::uint64_t> identity;
			std::uint64_t detail = 0x9ac415U;
			explicit CoordinatorCause(const char* message) : std::runtime_error(message), identity(std::make_shared<const std::uint64_t>(0x7bf901U)) {}
		};
		std::exception_ptr MakeCoordinatorCause(const char* message) { return std::make_exception_ptr(CoordinatorCause(message)); }
		bool IsOriginalCause(const std::exception_ptr& actual, const std::exception_ptr& expected) {
			// MSVC current_exception/rethrow_exception may copy the exception, making
			// exception_ptr operator== false for the same causal exception. A unique
			// immutable shared token, exact final subtype, state and message survive
			// legitimate copies and reject replacement by any secondary failure.
			if (!actual || !expected) return false;
			try { std::rethrow_exception(actual); }
			catch (const CoordinatorCause& cause) {
				try { std::rethrow_exception(expected); }
				catch (const CoordinatorCause& original) {
					return cause.identity == original.identity && cause.detail == original.detail
						&& std::string_view(cause.what()) == std::string_view(original.what());
				} catch (...) {}
			} catch (...) {}
			return false;
		}
		std::exception_ptr ExecuteFailure(Graph& graph, const TerraEngine::Scheduling::JobGraphExecuteOptions& options = {}) {
			try { (void)graph.Execute(options); } catch (...) { return std::current_exception(); } return {};
		}
		void CheckHeldFatal(TestContext& context, Checkpoint injected, bool boundary) {
			Correctness::ControlledGate gate; auto waiter = gate.MakeWaiter(std::chrono::seconds(30));
			std::atomic<unsigned> settled = 0U, laterCalls = 0U;
			const auto primary = MakeCoordinatorCause("original coordinator cause");
			std::promise<void> observed; auto observation = observed.get_future();
			Graph graph("fatal-settlement", "Fatal settlement");
			auto held = MakeJob("Held secondary failure", Policy::WorkerEligible);
			held.work = [waiter, &settled](auto&) { (void)waiter(); ++settled; throw std::runtime_error("secondary worker cause"); };
			graph.AddJob(std::move(held));
			TerraEngine::Scheduling::JobGraphExecuteOptions options;
			const auto throwPrimary = [&] { if (!gate.WaitForArrivals(1U)) throw std::runtime_error("Held worker did not enter."); observed.set_value(); std::rethrow_exception(primary); };
			if (boundary) {
				TerraEngine::Scheduling::JobGraphBarrierDeclaration barrier; barrier.name = "Failing owner boundary"; barrier.diagnosePendingMutations = true;
				graph.AddBarrier(std::move(barrier)); options.mutationBarrierCallback = [&](const auto&) -> TerraEngine::Scheduling::MutationFlushRuntimeState { throwPrimary(); return {}; };
			} else {
				graph.AddJob(MakeJob("Later admission", Policy::WorkerEligible));
				Access::SetCheckpoint(graph, [&](auto point, auto index) { if (point == injected && index == 1U) throwPrimary(); });
			}
			auto later = MakeJob("Never started after fatal", Policy::WorkerEligible); later.work = [&](auto&) { ++laterCalls; }; graph.AddJob(std::move(later));
			auto coordinator = std::async(std::launch::async, [&] { return ExecuteFailure(graph, options); });
			ReleaseAndJoin cleanup{gate, coordinator};
			const bool fatalObserved = observation.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			const bool retained = coordinator.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready;
			gate.Release(); const auto caught = coordinator.get();
			context.Expect(fatalObserved && retained, "Fatal Execute must retain all admitted workers behind the explicit release gate.");
			context.Expect(IsOriginalCause(caught, primary) && settled == 1U && laterCalls == 0U, "Original exception identity must survive secondary failure and stop later dispatch.");
			context.Expect(!graph.LatestSummary().completed && !graph.LatestSummary().valid && graph.LatestSummary().fatalExecution
				&& graph.LatestSummary().secondaryWorkerFailureCount == 1U && graph.LatestSummary().settledWorkerCount == 1U
				&& std::all_of(graph.LatestSummary().terminalStates.begin(), graph.LatestSummary().terminalStates.end(), [](auto state) {
					return state == TerraEngine::Scheduling::JobGraphNodeState::Completed || state == TerraEngine::Scheduling::JobGraphNodeState::Failed || state == TerraEngine::Scheduling::JobGraphNodeState::Blocked;
				}), "Fatal summary must retain truthful bounded worker counts and terminal outcomes instead of prior success.");
		}
	}

	void RegisterJobGraphTests(TestRegistry& registry)
	{
		registry.Add("Engine.JobGraph.FailureSettlement.HeldBoundaryPrimaryAndSecondary", [](TestContext& context) {
			for (const auto workers : {1U, 2U}) { ThreadPoolSession session(workers);
				for (unsigned iteration = 0U; iteration < 100U; ++iteration) CheckHeldFatal(context, Checkpoint::BeforePreparation, true);
			}
		});
		registry.Add("Engine.JobGraph.FailureSettlement.BeforeAndFailedAdmission", [](TestContext& context) {
			ThreadPoolSession session(1U);
			for (const auto checkpoint : {Checkpoint::BeforePreparation, Checkpoint::BeforeSubmission}) {
				Graph graph; graph.AddJob(MakeJob("First worker", Policy::WorkerEligible)); (void)graph.Execute();
				const auto primary = MakeCoordinatorCause("unadmitted submission cause");
				Access::SetCheckpoint(graph, [=](auto point, auto) { if (point == checkpoint) std::rethrow_exception(primary); });
				context.Expect(IsOriginalCause(ExecuteFailure(graph), primary), "Submission failure must propagate without waiting an unadmitted promise.");
				context.Expect(!graph.LatestSummary().completed && !graph.LatestSummary().valid, "A first-admission fault must invalidate prior success.");
			}
		});
		registry.Add("Engine.JobGraph.FailureSettlement.LaterAdmissionRetainsEarlierWorker", [](TestContext& context) {
			ThreadPoolSession session(2U);
			for (const auto checkpoint : {Checkpoint::BeforePreparation, Checkpoint::BeforeSubmission}) CheckHeldFatal(context, checkpoint, false);
		});
		registry.Add("Engine.JobGraph.FailureSettlement.CompletionProcessingRetainsWorker", [](TestContext& context) {
			ThreadPoolSession session(2U); Correctness::ControlledGate gate; auto waiter = gate.MakeWaiter(std::chrono::seconds(30));
			Graph graph; std::atomic<unsigned> settled = 0U; std::promise<void> observed; auto observation = observed.get_future();
			auto held = MakeJob("Held independent", Policy::WorkerEligible); held.work = [waiter, &settled](auto&) { (void)waiter(); ++settled; }; graph.AddJob(std::move(held));
			graph.AddJob(MakeJob("Fast completion", Policy::WorkerEligible));
			const auto primary = MakeCoordinatorCause("completion processing cause");
			Access::SetCheckpoint(graph, [&](auto point, auto index) { if (point == Checkpoint::BeforeCompletion && index == 1U) {
				if (!gate.WaitForArrivals(1U)) throw std::runtime_error("Held worker did not start."); observed.set_value(); std::rethrow_exception(primary);
			} });
			auto coordinator = std::async(std::launch::async, [&] { return ExecuteFailure(graph); }); ReleaseAndJoin cleanup{gate, coordinator};
			const bool observedFatal = observation.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			const bool retained = coordinator.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready;
			gate.Release(); context.Expect(IsOriginalCause(coordinator.get(), primary) && observedFatal && retained && settled == 1U,
				"Completion bookkeeping faults must drain admitted work without relying on further mailbox reads.");
		});
		registry.Add("Engine.JobGraph.FailureSettlement.RearmAndDestructionAreQuiescent", [](TestContext& context) {
			ThreadPoolSession session(2U); std::atomic<unsigned> calls = 0U;
			// A burst crosses the initial cache reservation. Released unadmitted
			// handles must be reusable without allocating during their deleters.
			std::vector<std::shared_ptr<Task<void>>> burst; burst.reserve(80U);
			for (unsigned index = 0U; index < 80U; ++index) burst.push_back(ThreadPool::instance().createTask([] {}));
			const auto last = burst.back().get(); for (auto& task : burst) task.reset(); burst.clear();
			const auto reused = ThreadPool::instance().createTask([] {});
			context.Expect(reused.get() == last, "Global void-task cache must retain normal burst capacity for reuse.");
			{ Graph graph; auto job = MakeJob("Reusable", Policy::WorkerEligible); job.work = [&](auto&) { ++calls; }; graph.AddJob(std::move(job));
				(void)graph.Execute(); graph.Rearm("faulted invocation"); const auto primary = MakeCoordinatorCause("reuse cause");
				Access::SetCheckpoint(graph, [=](auto point, auto) { if (point == Checkpoint::BeforeSubmission) std::rethrow_exception(primary); });
				context.Expect(IsOriginalCause(ExecuteFailure(graph), primary) && !graph.LatestSummary().completed, "A settled fatal invocation must replace old success.");
				Access::SetCheckpoint(graph, {}); graph.Rearm("healthy invocation"); const auto summary = graph.Execute();
				context.Expect(summary.valid && summary.completed && calls == 2U, "Legal Rearm must execute fresh work after settlement exactly once.");
			} context.Expect(calls == 2U, "Graph destruction after settlement must retain no old worker writes.");
		});
		registry.Add("Engine.JobGraph.FailureSettlement.EntryAndBodyNotifyExactlyOnce", [](TestContext& context) {
			ThreadPoolSession session(2U);
			{ Graph overlap; Correctness::ControlledGate gate; auto waiter = gate.MakeWaiter(std::chrono::seconds(30));
				std::promise<void> dependentEntered; auto dependentObservation = dependentEntered.get_future();
				auto held = MakeJob("Held healthy sibling", Policy::WorkerEligible); held.work = [waiter](auto&) { (void)waiter(); }; overlap.AddJob(std::move(held));
				const auto fast = overlap.AddJob(MakeJob("Fast healthy sibling", Policy::WorkerEligible));
				auto dependent = MakeJob("Completion driven dependent", Policy::Protected); dependent.dependencies = {fast}; dependent.work = [&](auto&) { dependentEntered.set_value(); }; overlap.AddJob(std::move(dependent));
				auto coordinator = std::async(std::launch::async, [&] { return ExecuteFailure(overlap); }); ReleaseAndJoin cleanup{gate, coordinator};
				const bool heldStarted = gate.WaitForArrivals(1U);
				const bool overlapped = dependentObservation.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
				const bool retained = coordinator.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
				gate.Release(); context.Expect(!coordinator.get() && heldStarted && overlapped && retained,
					"A fast healthy completion must release its dependent while the unrelated sibling remains held.");
			}
			Graph graph; std::atomic<unsigned> calls = 0U;
			const auto entry = graph.AddJob(MakeJob("Entry failure", Policy::WorkerEligible));
			auto body = MakeJob("Body failure", Policy::WorkerEligible); body.work = [](auto&) { throw std::runtime_error("body cause"); }; graph.AddJob(std::move(body));
			auto healthy = MakeJob("Healthy independent", Policy::WorkerEligible); healthy.work = [&](auto&) { ++calls; }; graph.AddJob(std::move(healthy));
			auto blocked = MakeJob("Blocked entry descendant", Policy::Protected); blocked.dependencies = {entry}; blocked.work = [&](auto&) { ++calls; }; graph.AddJob(std::move(blocked));
			Access::SetCheckpoint(graph, [](auto point, auto index) { if (point == Checkpoint::WorkerEntry && index == 0U) throw std::runtime_error("entry cause"); });
			const auto summary = graph.Execute();
			context.Expect(!summary.valid && summary.workerCompletionWakeCount == 3U && calls == 1U
				&& HasJobState(summary, "Entry failure", TerraEngine::Scheduling::JobGraphNodeState::Failed)
				&& HasJobState(summary, "Body failure", TerraEngine::Scheduling::JobGraphNodeState::Failed),
				"Entry/setup and body failures must each publish exactly one completion and retain independent progress.");
		});

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

		registry.Add("Engine.JobGraph.RequiredFailureSettlesIndependentWorkAndSkipsDescendants", [](TestContext& context) {
			for (const auto policy : {TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible,
				TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected}) {
				ThreadPoolSession session(2U);
				Correctness::ControlledGate independentGate;
				auto waiter = independentGate.MakeWaiter();
				std::atomic<unsigned> independentCompleted = 0U;
				std::atomic<unsigned> dependentCalls = 0U;
				std::promise<void> failureEntered;
				auto failureObserved = failureEntered.get_future();
				TerraEngine::Scheduling::JobGraph graph("required-failure-settlement", "Required failure settlement");
				auto independent = MakeJob("Independent", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
				independent.work = [waiter, &independentCompleted](TerraEngine::Scheduling::JobGraphContext&) {
					(void)waiter(); ++independentCompleted;
				};
				graph.AddJob(std::move(independent));
				auto failing = MakeJob("Failing", policy);
				failing.work = [&failureEntered, &independentGate](TerraEngine::Scheduling::JobGraphContext&) {
					(void)independentGate.WaitForArrivals(1U);
					failureEntered.set_value();
					throw std::runtime_error("Unexpected required prerequisite failure");
				};
				const auto failed = graph.AddJob(std::move(failing));
				auto dependent = MakeJob("Dependent", TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected);
				dependent.dependencies = {failed};
				dependent.work = [&dependentCalls](TerraEngine::Scheduling::JobGraphContext&) { ++dependentCalls; };
				const auto blocked = graph.AddJob(std::move(dependent));
				auto descendant = MakeJob("Descendant", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
				descendant.dependencies = {blocked};
				descendant.work = [&dependentCalls](TerraEngine::Scheduling::JobGraphContext&) { ++dependentCalls; };
				graph.AddJob(std::move(descendant));
				// The test coordinator waits separately; all production work still
				// runs on the existing graph/pool. Release before any assertion can
				// unwind a future whose destructor joins the coordinator.
				auto executing = std::async(std::launch::async, [&graph] { return graph.Execute(); });
				const bool observed = failureObserved.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
				const bool retainedIndependent = executing.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
				independentGate.Release();
				const auto summary = executing.get();
				context.Expect(observed && retainedIndependent, "A required fault must retain already-running independent work until settlement.");
				context.Expect(!summary.valid, "Required failure must remain visible in graph diagnostics.");
				context.Expect(summary.firstFailure && summary.firstFailure->nodeLabel == "Failing"
					&& summary.firstFailure->message.find("Unexpected required prerequisite failure") != std::string::npos,
					"First failure evidence must retain the original job and exception before display sorting.");
				context.ExpectEqual(1U, independentCompleted.load(), "Independent worker work must settle once.");
				context.ExpectEqual(0U, dependentCalls.load(), "Required dependents and descendants must never execute.");
				context.Expect(HasJobState(summary, "Dependent", TerraEngine::Scheduling::JobGraphNodeState::Blocked)
					&& HasJobState(summary, "Descendant", TerraEngine::Scheduling::JobGraphNodeState::Blocked),
					"Skipped required work must have explicit terminal blocked states.");
			}
		});

		registry.Add("Engine.JobGraph.InvalidRequiredPlanRunsNoWork", [](TestContext& context) {
			ThreadPoolSession session(2U);
			std::atomic<unsigned> calls = 0U;
			TerraEngine::Scheduling::JobGraph graph("invalid-required-plan", "Invalid required plan");
			auto job = MakeJob("Invalid", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			job.dependencies = {TerraEngine::Scheduling::DependencyHandle{999999U}};
			job.work = [&calls](TerraEngine::Scheduling::JobGraphContext&) { ++calls; };
			graph.AddJob(std::move(job));
			const auto summary = graph.Execute();
			context.Expect(!summary.valid, "An unresolved prerequisite must reject the required plan.");
			context.ExpectEqual(0U, calls.load(), "Invalid required plans must execute no workload.");
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

		registry.Add("Engine.JobGraph.CompletionDrivenDispatchDoesNotWaitForSlowSibling", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Scheduling::JobGraph graph("completion-driven", "Completion Driven");
			std::atomic_bool slowStarted = false;
			std::atomic_bool slowFinished = false;
			std::atomic_bool dependentRanBeforeSlowFinished = false;

			auto fast = MakeJob("Fast", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			fast.work = [&slowStarted](TerraEngine::Scheduling::JobGraphContext&) {
				while (!slowStarted.load(std::memory_order_acquire))
				{
					std::this_thread::yield();
				}
			};
			const auto fastHandle = graph.AddJob(std::move(fast));

			auto slow = MakeJob("Slow", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			slow.work = [&slowStarted, &slowFinished](TerraEngine::Scheduling::JobGraphContext&) {
				slowStarted.store(true, std::memory_order_release);
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				slowFinished.store(true, std::memory_order_release);
			};
			graph.AddJob(std::move(slow));

			auto dependent = MakeJob("FastDependent", TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible);
			dependent.dependencies = { fastHandle };
			dependent.work = [&slowFinished, &dependentRanBeforeSlowFinished](TerraEngine::Scheduling::JobGraphContext&) {
				dependentRanBeforeSlowFinished.store(
					!slowFinished.load(std::memory_order_acquire), std::memory_order_release);
			};
			graph.AddJob(std::move(dependent));

			const TerraEngine::Scheduling::JobGraphSummary& summary = graph.Execute();
			context.Expect(summary.valid && summary.completed,
				"Completion-driven fixture should execute successfully.");
			context.Expect(dependentRanBeforeSlowFinished.load(std::memory_order_acquire),
				"A fast dependency chain should continue while an unrelated slow sibling remains active.");
			context.Expect(summary.completionDrivenDispatchCount >= 1u,
				"Structured diagnostics should count worker work dispatched from an early completion.");
			context.Expect(summary.dependencyWaveCount >= 2u
				&& summary.peakConcurrentWorkerCount >= 2u
				&& summary.criticalPathDurationNs != 0u,
				"Scheduler diagnostics should expose wave depth, concurrency, and measured critical path.");
			context.Expect(std::all_of(summary.jobs.begin(), summary.jobs.end(),
				[](const TerraEngine::Scheduling::JobGraphResultEntry& entry) {
					return entry.kind != TerraEngine::Scheduling::JobGraphNodeKind::Job
						|| entry.executionPolicy
							!= TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible
						|| entry.completionSequence != 0u;
				}),
				"Every completed worker result must retain its observed completion sequence.");
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
