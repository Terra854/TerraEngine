#include "TestHarness.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class TelemetryPoolSession final {
		public:
			explicit TelemetryPoolSession(std::size_t historyCapacity = ThreadPool::kMaximumTelemetryHistoryCapacityPerWorker,
				std::uint32_t samplingInterval = 1)
		{
				ThreadPool& pool = ThreadPool::instance();
				pool.shutdown();
				pool.ConfigureTelemetrySampling(samplingInterval, historyCapacity);
				// One worker makes queue-depth and history assertions deterministic;
				// production initialization still defaults to all detected hardware.
				pool.initialize(1);
			}

			~TelemetryPoolSession()
			{
				ThreadPool& pool = ThreadPool::instance();
				pool.shutdown();
				pool.ConfigureTelemetrySampling(1, ThreadPool::kMaximumTelemetryHistoryCapacityPerWorker);
			}

			TelemetryPoolSession(const TelemetryPoolSession&) = delete;
			TelemetryPoolSession& operator=(const TelemetryPoolSession&) = delete;
		};

		void WaitForFuture(TestContext& context, std::future<void>& future, std::string_view message)
		{
			const std::future_status status = future.wait_for(std::chrono::seconds(3));
			context.Expect(status == std::future_status::ready, message);
			future.get();
		}

		std::shared_ptr<const ThreadPoolTelemetrySnapshot> WaitForDispatchCount(
			TestContext& context,
			ThreadPool& pool,
			std::uint64_t expectedDispatchCount)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			do {
				auto snapshot = pool.GetTelemetrySnapshot();
				if (!snapshot->workers.empty() && snapshot->workers.front().dispatchCount >= expectedDispatchCount
					&& snapshot->workers.front().activity != ThreadPoolWorkerActivity::Running) {
					return snapshot;
				}
				std::this_thread::yield();
			} while (std::chrono::steady_clock::now() < deadline);

			context.Expect(false, "Worker telemetry should publish every completed dispatch before the timeout.");
			return pool.GetTelemetrySnapshot();
		}
	}

	void RegisterWorkerPoolTelemetryTests(TestRegistry& registry)
	{
		registry.Add("Engine.ThreadPool.TelemetryCapturesQueueWorkAndTimingPerWorker", [](TestContext& context) {
			TelemetryPoolSession session;
			ThreadPool& pool = ThreadPool::instance();
			// Give the worker a measurable idle period before the first dispatch.
			std::this_thread::sleep_for(std::chrono::milliseconds(2));

			std::promise<void> blockerStarted;
			std::future<void> blockerStartedFuture = blockerStarted.get_future();
			std::promise<void> releaseBlocker;
			std::shared_future<void> releaseFuture = releaseBlocker.get_future().share();
			auto blocker = pool.createTask([&blockerStarted, releaseFuture]() {
				blockerStarted.set_value();
				releaseFuture.wait();
			});
			std::future<void> blockerFuture = blocker->getFuture();
			pool.enqueue(blocker, 2);
			context.Expect(blockerStartedFuture.wait_for(std::chrono::seconds(3)) == std::future_status::ready,
				"The blocking task should start before backlog work is queued.");

			std::vector<std::future<void>> futures;
			for (std::uint64_t usefulItems : {3ULL, 5ULL, 0ULL}) {
				auto task = pool.createTask([]() {});
				futures.push_back(task->getFuture());
				pool.enqueue(task, usefulItems);
			}
			releaseBlocker.set_value();
			WaitForFuture(context, blockerFuture, "The blocking telemetry task should finish after release.");
			for (std::future<void>& future : futures) {
				WaitForFuture(context, future, "Every queued telemetry task should complete.");
			}

			auto snapshot = WaitForDispatchCount(context, pool, 4);
			context.ExpectEqual(1ULL, static_cast<unsigned long long>(snapshot->workers.size()),
				"A one-worker pool should publish one worker telemetry record.");
			const ThreadPoolWorkerTelemetry& worker = snapshot->workers.front();
			context.ExpectEqual(4ULL, static_cast<unsigned long long>(worker.dispatchCount),
				"Every claimed task should increment the worker dispatch count exactly once.");
			context.ExpectEqual(10ULL, static_cast<unsigned long long>(worker.usefulItemCount),
				"Useful-item totals should preserve caller-provided work, including a visible zero-item dispatch.");
			context.Expect(worker.waitNanoseconds > 0 && worker.runNanoseconds > 0 && worker.idleNanoseconds > 0,
				"Worker telemetry should retain queue wait, execution, and idle timing separately.");
			context.ExpectEqual(4ULL, static_cast<unsigned long long>(worker.history.size()),
				"The default one-dispatch sampling interval should retain each completed dispatch within the bound.");

			std::size_t maximumEnqueueDepth = 0;
			std::size_t maximumStartDepth = 0;
			for (const ThreadPoolWorkerTelemetrySample& sample : worker.history) {
				maximumEnqueueDepth = std::max(maximumEnqueueDepth, sample.queueDepthAtEnqueue);
				maximumStartDepth = std::max(maximumStartDepth, sample.queueDepthAtStart);
			}
			context.Expect(maximumEnqueueDepth >= 3 && maximumStartDepth >= 3,
				"Backlogged dispatches should report queue depth at both enqueue and worker start.");
		});

		registry.Add("Engine.ThreadPool.TelemetryHistoryIsBoundedAndAccountsForDroppedSamples", [](TestContext& context) {
			constexpr std::size_t historyCapacity = 8;
			constexpr std::size_t dispatchCount = 20;
			TelemetryPoolSession session(historyCapacity);
			ThreadPool& pool = ThreadPool::instance();
			std::vector<std::future<void>> futures;
			futures.reserve(dispatchCount);
			for (std::size_t index = 0; index < dispatchCount; ++index) {
				auto task = pool.createTask([]() {});
				futures.push_back(task->getFuture());
				pool.enqueue(task);
			}
			for (std::future<void>& future : futures) {
				WaitForFuture(context, future, "Bounded-history work should complete.");
			}

			auto snapshot = WaitForDispatchCount(context, pool, dispatchCount);
			const ThreadPoolWorkerTelemetry& worker = snapshot->workers.front();
			context.ExpectEqual(static_cast<unsigned long long>(historyCapacity),
				static_cast<unsigned long long>(snapshot->historyCapacityPerWorker),
				"The immutable snapshot should report the configured per-worker history bound.");
			context.ExpectEqual(static_cast<unsigned long long>(historyCapacity),
				static_cast<unsigned long long>(worker.history.size()),
				"Completed history must never grow beyond its configured per-worker bound.");
			context.ExpectEqual(static_cast<unsigned long long>(dispatchCount - historyCapacity),
				static_cast<unsigned long long>(worker.droppedHistorySampleCount),
				"Overwritten worker history should be counted rather than silently discarded.");
			context.ExpectEqual(worker.droppedHistorySampleCount, snapshot->droppedHistorySampleCount,
				"The pool-wide dropped count should equal the only worker's dropped history in a one-worker run.");
		});

		registry.Add("Engine.ThreadPool.TelemetrySnapshotsAreImmutableAndReportSampling", [](TestContext& context) {
			TelemetryPoolSession session(8, 2);
			ThreadPool& pool = ThreadPool::instance();
			auto before = pool.GetTelemetrySnapshot();
			static_assert(std::is_same_v<decltype(before), std::shared_ptr<const ThreadPoolTelemetrySnapshot>>,
				"ThreadPool snapshots must expose const ownership to readers.");

			std::vector<std::future<void>> futures;
			for (std::size_t index = 0; index < 5; ++index) {
				auto task = pool.createTask([]() {});
				futures.push_back(task->getFuture());
				pool.enqueue(task);
			}
			for (std::future<void>& future : futures) {
				WaitForFuture(context, future, "Sampled telemetry work should complete.");
			}

			auto after = WaitForDispatchCount(context, pool, 5);
			context.ExpectEqual(0ULL, static_cast<unsigned long long>(before->workers.front().dispatchCount),
				"A previously returned snapshot must not change when workers publish later counters.");
			context.ExpectEqual(2U, after->samplingIntervalDispatches,
				"Snapshots should expose the interval used to select retained dispatch detail.");
			context.ExpectEqual(2ULL, static_cast<unsigned long long>(after->workers.front().history.size()),
				"A two-dispatch sampling interval should retain completed dispatches two and four.");
			context.ExpectEqual(5ULL, static_cast<unsigned long long>(after->workers.front().dispatchCount),
				"Sampling should reduce only detailed history, never cumulative dispatch evidence.");
		});
	}
}
