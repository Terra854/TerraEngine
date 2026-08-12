#include "TestHarness.h"

#include "Physics/Backend/PhysicsAllocatorAdapter.h"
#include "Physics/Backend/PhysicsJobAdapter.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class ThreadPoolSession final {
		public:
			explicit ThreadPoolSession(std::size_t workerCount = 0U) {
				// The shared engine pool is a singleton. Restart it for every test so
				// queued jobs from one case cannot hide a lifecycle regression in another.
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize(workerCount);
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}

			ThreadPoolSession(const ThreadPoolSession&) = delete;
			ThreadPoolSession& operator=(const ThreadPoolSession&) = delete;
		};

		void ExpectAdapterUsesSharedPool(TestContext& context, ThreadPool& sharedPool, std::uint32_t workerLimit) {
			const std::size_t sharedWorkerCount = sharedPool.getThreadCount();
			context.Expect(sharedWorkerCount > 0U, "The shared TerraEngine ThreadPool must be initialized before a physics adapter submits work.");

			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = workerLimit,
				.maxBarriers = 2U,
				.maxJobs = 32U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(sharedPool, configuration);
			std::atomic<int> executionCount = 0;
			auto* barrier = adapter.CreateBarrier();
			// A dependency holds the job until it belongs to the barrier. This mirrors
			// Jolt's documented ownership protocol and prevents a fast worker from
			// completing the job before the test has attached the barrier.
			auto job = adapter.CreateJob("Physics adapter shared-pool job", JPH::Color::sWhite, [&]() {
				executionCount.fetch_add(1, std::memory_order_acq_rel);
			}, 1U);
			barrier->AddJob(job);
			job.RemoveDependency();
			adapter.WaitForJobs(barrier);
			adapter.WaitForIdle();
			adapter.DestroyBarrier(barrier);

			context.ExpectEqual(1, executionCount.load(std::memory_order_acquire), "A completed physics job must execute exactly once.");
			context.ExpectEqual(static_cast<int>(std::min<std::size_t>(workerLimit, sharedWorkerCount) + 1U), adapter.GetMaxConcurrency(),
				"Jolt concurrency must include the bounded shared-pool share plus the barrier-waiting caller lane.");
			context.ExpectEqual(static_cast<std::uint32_t>(std::min<std::size_t>(workerLimit, sharedWorkerCount)), adapter.Diagnostics().workerLimit,
				"Diagnostics must expose the effective shared-pool worker limit without claiming provider-private workers.");
			context.ExpectEqual(0U, adapter.Diagnostics().activePumps,
				"All shared-pool pumps must finish before an idle adapter reports completion.");
		}
	}

	void RegisterPhysicsJobAdapterTests(TestRegistry& registry) {
		registry.Add("Engine.Physics.JobAdapter.OneWorkerNestedPumpRunsInline", [](TestContext& context) {
			ThreadPoolSession session(1U);
			ThreadPool& sharedPool = ThreadPool::instance();
			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = 1U,
				.maxBarriers = 1U,
				.maxJobs = 8U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(sharedPool, configuration);
			std::atomic<bool> jobRanInline = false;
			std::atomic<int> executionCount = 0;
			auto* barrier = adapter.CreateBarrier();

			// Run the provider submission from the only engine worker. Before the
			// serial fallback, this job was merely queued behind its own caller.
			auto outerTask = sharedPool.createTask([&]() {
				auto job = adapter.CreateJob("one-worker nested pump", JPH::Color::sWhite, [&]() {
					executionCount.fetch_add(1, std::memory_order_acq_rel);
				}, 1U);
				barrier->AddJob(job);
				job.RemoveDependency();
				jobRanInline.store(executionCount.load(std::memory_order_acquire) == 1,
					std::memory_order_release);
			});
			auto outerFuture = outerTask->getFuture();
			sharedPool.enqueue(std::move(outerTask));
			context.Expect(outerFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
				"One-worker nested provider submission must complete without waiting behind its own pool task.");
			outerFuture.get();

			context.Expect(jobRanInline.load(std::memory_order_acquire),
				"A one-worker provider submission must make progress before its enclosing worker task returns.");
			context.ExpectEqual(1, adapter.GetMaxConcurrency(),
				"A one-worker shared pool must expose serial Jolt concurrency rather than a nonexistent helper lane.");
			adapter.WaitForJobs(barrier);
			adapter.WaitForIdle();
			adapter.DestroyBarrier(barrier);
			context.ExpectEqual(1, executionCount.load(std::memory_order_acquire),
				"The inline nested pump must still execute the Jolt job exactly once.");
			context.ExpectEqual(0U, adapter.Diagnostics().activePumps,
				"The one-worker inline pump must drain before the enclosing physics task completes.");
		});

		registry.Add("Engine.Physics.JobAdapter.UsesSharedThreadPoolAcrossWorkerConfigurations", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& sharedPool = ThreadPool::instance();
			const std::uint32_t sharedPoolWorkerCount = static_cast<std::uint32_t>(sharedPool.getThreadCount());
			for(const std::uint32_t workerLimit : {1U, 2U, 4U, sharedPoolWorkerCount}) {
				// Each configuration must preserve completion behavior even when Jolt
				// requests a different degree of parallelism than the host machine offers.
				ExpectAdapterUsesSharedPool(context, sharedPool, workerLimit);
			}
		});

		registry.Add("Engine.Physics.JobAdapter.BarrierWaitIsSafeAndDeterministic", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = 4U,
				.maxBarriers = 2U,
				.maxJobs = 32U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(ThreadPool::instance(), configuration);
			std::atomic<int> completedJobs = 0;
			auto* barrier = adapter.CreateBarrier();

			for(int jobIndex = 0; jobIndex < 16; ++jobIndex) {
				auto job = adapter.CreateJob("Physics adapter barrier job", JPH::Color::sWhite, [&completedJobs]() {
					completedJobs.fetch_add(1, std::memory_order_acq_rel);
				}, 1U);
				barrier->AddJob(job);
				job.RemoveDependency();
			}

			adapter.WaitForJobs(barrier);
			adapter.WaitForIdle();
			adapter.DestroyBarrier(barrier);
			context.ExpectEqual(16, completedJobs.load(std::memory_order_acquire), "Barrier completion must observe every attached job exactly once.");
			context.ExpectEqual(16U, adapter.Diagnostics().completed,
				"Diagnostics must record each completed job once after the barrier and shared-pool pumps are drained.");
		});

		registry.Add("Engine.Physics.JobAdapter.BatchWaveAmortizesQueueLocksAndPumpSubmissions", [](TestContext& context) {
			ThreadPoolSession session(4U);
			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = 4U,
				.maxBarriers = 2U,
				.maxJobs = 256U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(ThreadPool::instance(), configuration);
			constexpr JPH::uint jobCount = 128U;
			std::vector<JPH::JobSystem::JobHandle> jobs;
			jobs.reserve(jobCount);
			std::vector<std::atomic<std::uint32_t>> results(jobCount);
			for(std::atomic<std::uint32_t>& result : results) result.store(0U, std::memory_order_relaxed);
			auto* barrier = adapter.CreateBarrier();
			for(JPH::uint index = 0U; index < jobCount; ++index) {
				jobs.push_back(adapter.CreateJob("Physics adapter batch-wave job", JPH::Color::sWhite, [&, index]() {
					// Every job owns one deterministic output slot. Execution order may vary,
					// but the published result remains stable across worker counts.
					results[index].store(index + 1U, std::memory_order_release);
				}, 1U));
			}
			barrier->AddJobs(jobs.data(), jobCount);
			JPH::JobSystem::JobHandle::sRemoveDependencies(jobs.data(), jobCount);
			adapter.WaitForJobs(barrier);
			adapter.DestroyBarrier(barrier);

			for(JPH::uint index = 0U; index < jobCount; ++index) {
				context.ExpectEqual(index + 1U, results[index].load(std::memory_order_acquire),
					"A provider batch wave must complete every stable output slot exactly once.");
			}
			const auto diagnostics = adapter.Diagnostics();
			context.ExpectEqual(static_cast<std::uint64_t>(jobCount), diagnostics.submitted,
				"Batch diagnostics must reconcile every submitted provider job.");
			context.ExpectEqual(diagnostics.submitted, diagnostics.completed,
				"A completed batch barrier must reconcile submitted and completed totals.");
			context.ExpectEqual(1U, diagnostics.batchCount,
				"One Jolt dependency wave must be observed as one adapter batch.");
			context.Expect(diagnostics.lockAcquisitionCount < diagnostics.submitted,
				"Bounded pump chunks must amortize queue locks below the provider job count.");
			context.Expect(diagnostics.pumpSubmissionCount <= configuration.maxPhysicsWorkers,
				"A batch wave must submit no more useful pumps than its physics worker cap.");
			context.Expect(diagnostics.maxActivePumps <= configuration.maxPhysicsWorkers,
				"Active pumps must remain within the configured provider share.");
			context.ExpectEqual(0U, diagnostics.queued,
				"No provider jobs may remain queued after the batch barrier drains.");
			context.ExpectEqual(0U, diagnostics.activePumps,
				"No shared-pool pump may outlive the completed batch wave.");
		});

		registry.Add("Engine.Physics.JobAdapter.BoundedConfigurationAndDiagnosticsRemainWithinCapacity", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = 2U,
				.maxBarriers = 3U,
				.maxJobs = 24U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(ThreadPool::instance(), configuration);

			context.ExpectEqual(configuration.maxJobs, adapter.Config().maxJobs,
				"The adapter must retain an explicit fixed job capacity instead of allocating an unbounded provider queue.");
			context.ExpectEqual(configuration.maxBarriers, adapter.Config().maxBarriers,
				"The adapter must retain an explicit barrier capacity for bounded barrier bookkeeping.");
			context.Expect(adapter.Diagnostics().queued <= configuration.maxJobs,
				"Diagnostics must never report more queued jobs than the adapter's fixed configured capacity.");
			context.ExpectEqual(0U, adapter.Diagnostics().rejected,
				"An idle adapter must begin with no rejected bounded-work requests.");
		});

		registry.Add("Engine.Physics.JobAdapter.CancellationDrainsQueuedWorkAndPreventsLateCallbacks", [](TestContext& context) {
			ThreadPoolSession session;
			TerraEngine::Physics::Backend::PhysicsJobAdapterConfig configuration{
				.maxPhysicsWorkers = 1U,
				.maxBarriers = 1U,
				.maxJobs = 8U
			};
			TerraEngine::Physics::Backend::PhysicsAllocatorAdapter allocator;
			TerraEngine::Physics::Backend::PhysicsJobAdapter adapter(ThreadPool::instance(), configuration);
			std::atomic<bool> stopObserved = false;
			auto* barrier = adapter.CreateBarrier();
			auto job = adapter.CreateJob("Physics adapter cancellation job", JPH::Color::sWhite, [&]() {
				// Jobs already accepted into Jolt may run while stopping, but they must
				// observe the stop latch and leave no callback after the barrier drains.
				stopObserved.store(adapter.IsStopRequested(), std::memory_order_release);
			}, 1U);
			barrier->AddJob(job);

			adapter.RequestStop();
			context.Expect(adapter.IsStopRequested(), "RequestStop must latch cancellation before queued Jolt work is released.");
			context.Expect(!adapter.IsAcceptingJobs(), "A stopped physics adapter must reject new Jolt job submissions.");
			job.RemoveDependency();
			adapter.WaitForJobs(barrier);
			adapter.WaitForIdle();
			adapter.DestroyBarrier(barrier);

			context.Expect(stopObserved.load(std::memory_order_acquire), "Accepted jobs must observe cancellation while their barrier still drains safely.");
			context.ExpectEqual(0U, adapter.Diagnostics().queued, "A stopped and drained adapter must expose no queued physics jobs.");
		});
}
}
