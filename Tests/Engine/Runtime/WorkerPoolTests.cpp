#include "TestHarness.h"

#include "Utilities/Tepak/WorkerPool.h"

#include <atomic>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	void RegisterWorkerPoolTests(TestRegistry& registry) {
		registry.Add("Engine.WorkerPool.ZeroWorkerCountCreatesOneWorker", [](TestContext& context) {
			const tepak::util::WorkerPool pool(0U);

			context.ExpectEqual(1U, pool.GetWorkerCount(), "A zero-sized worker pool request should clamp to one worker.");
		});

		registry.Add("Engine.WorkerPool.SubmittedTasksReturnValuesAndDrain", [](TestContext& context) {
			tepak::util::WorkerPool pool(2U);
			std::future<int> number = pool.Submit([]() {
				return 21 * 2;
			});
			std::future<std::string> text = pool.Submit([]() {
				return std::string("complete");
			});

			context.ExpectEqual(42, number.get(), "Submitted integer tasks should return their computed value through the future.");
			context.ExpectEqual(std::string("complete"), text.get(), "Submitted string tasks should return their computed value through the future.");
		});

		registry.Add("Engine.WorkerPool.MultipleTasksCanUpdateSharedAtomicState", [](TestContext& context) {
			tepak::util::WorkerPool pool(3U);
			std::atomic<int> sum = 0;
			std::vector<std::future<void>> futures;

			for(int value = 1; value <= 8; ++value) {
				futures.push_back(pool.Submit([&sum, value]() {
					sum.fetch_add(value);
				}));
			}

			for(std::future<void>& future : futures) {
				future.get();
			}

			context.ExpectEqual(36, sum.load(), "All queued worker tasks should run before their futures report completion.");
		});

		registry.Add("Engine.WorkerPool.TaskExceptionsPropagateThroughFuture", [](TestContext& context) {
			tepak::util::WorkerPool pool(1U);
			std::future<int> failure = pool.Submit([]() -> int {
				throw std::runtime_error("worker failure");
			});

			bool propagated = false;
			try {
				(void)failure.get();
			} catch(const std::runtime_error& error) {
				propagated = std::string(error.what()).find("worker failure") != std::string::npos;
			}

			context.Expect(propagated, "Exceptions thrown by worker tasks should propagate through the returned future.");
		});
	}
}
