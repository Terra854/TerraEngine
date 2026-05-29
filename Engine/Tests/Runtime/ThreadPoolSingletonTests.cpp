#include "TestHarness.h"

#include "ThreadPool/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace TerraEngine::Tests {
	namespace {
		template<typename T>
		T WaitForValue(TestContext& context, std::future<T>& future, std::string_view message) {
			const auto status = future.wait_for(std::chrono::seconds(3));
			context.Expect(status == std::future_status::ready, message);
			return future.get();
		}

		void WaitForVoid(TestContext& context, std::future<void>& future, std::string_view message) {
			const auto status = future.wait_for(std::chrono::seconds(3));
			context.Expect(status == std::future_status::ready, message);
			future.get();
		}

		class ThreadPoolSession {
		public:
			ThreadPoolSession() {
				// Start every test from a known lifecycle state. shutdown is
				// intentionally called first because the singleton outlives each
				// test body.
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				active_ = true;
			}

			~ThreadPoolSession() {
				if (active_) {
					ThreadPool::instance().shutdown();
				}
			}

			ThreadPoolSession(const ThreadPoolSession&) = delete;
			ThreadPoolSession& operator=(const ThreadPoolSession&) = delete;

			void Restart() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				active_ = true;
			}

			void Shutdown() {
				ThreadPool::instance().shutdown();
				active_ = false;
			}

		private:
			bool active_ = false;
		};
	}

	void RegisterThreadPoolSingletonTests(TestRegistry& registry) {
		registry.Add("Engine.ThreadPool.InitializeShutdownAndReinitializeExecutesTasks", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& pool = ThreadPool::instance();

			context.Expect(pool.getThreadCount() > 0U, "Initializing the engine thread pool should start at least one worker.");
			auto firstTask = pool.createTask([]() {
				return 21 * 2;
			});
			std::future<int> firstFuture = firstTask->getFuture();
			pool.enqueue(firstTask);
			context.ExpectEqual(42, WaitForValue(context, firstFuture, "The first initialized thread pool should execute queued tasks."), "The first queued task should return its computed value.");

			session.Shutdown();
			context.ExpectEqual(0ULL, static_cast<unsigned long long>(pool.getThreadCount()), "Shutting down the engine thread pool should clear worker ownership.");

			session.Restart();
			context.Expect(pool.getThreadCount() > 0U, "Reinitializing after shutdown should create a fresh worker group.");
			auto secondTask = pool.createTask([]() {
				return std::string("restarted");
			});
			std::future<std::string> secondFuture = secondTask->getFuture();
			pool.enqueue(secondTask);
			context.ExpectEqual(std::string("restarted"), WaitForValue(context, secondFuture, "A restarted thread pool should execute queued tasks."), "The restarted pool should return task results.");
		});

		registry.Add("Engine.ThreadPool.ValueAndVoidTasksCompleteThroughFutures", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& pool = ThreadPool::instance();
			std::atomic<int> voidTaskValue = 0;

			auto valueTask = pool.createTask([](int left, int right) {
				return left + right;
			}, 19, 23);
			std::future<int> valueFuture = valueTask->getFuture();
			pool.enqueue(valueTask);

			auto voidTask = pool.createTask([&voidTaskValue]() {
				voidTaskValue.store(7, std::memory_order_release);
			});
			std::future<void> voidFuture = voidTask->getFuture();
			pool.enqueue(voidTask);

			context.ExpectEqual(42, WaitForValue(context, valueFuture, "Value-returning engine tasks should complete their futures."), "Value-returning tasks should preserve their result.");
			WaitForVoid(context, voidFuture, "Void engine tasks should complete their futures.");
			context.ExpectEqual(7, voidTaskValue.load(std::memory_order_acquire), "Void tasks should run their side effects before the future becomes ready.");
		});

		registry.Add("Engine.ThreadPool.TaskExceptionsPropagateThroughFuture", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& pool = ThreadPool::instance();
			auto failingTask = pool.createTask([]() -> int {
				throw std::runtime_error("engine task failure");
			});
			std::future<int> future = failingTask->getFuture();
			pool.enqueue(failingTask);
			context.Expect(future.wait_for(std::chrono::seconds(3)) == std::future_status::ready, "Failing tasks should still complete their future.");

			bool propagated = false;
			try {
				(void)future.get();
			} catch (const std::runtime_error& error) {
				propagated = std::string(error.what()).find("engine task failure") != std::string::npos;
			}

			context.Expect(propagated, "Exceptions thrown by engine thread-pool tasks should propagate through the future.");
		});

		registry.Add("Engine.ThreadPool.DependencyTasksRunBeforeParentTask", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& pool = ThreadPool::instance();
			std::atomic<bool> dependencyFinished = false;

			auto dependency = pool.createTask([&dependencyFinished]() {
				// The small delay makes premature parent execution visible even
				// on fast machines with many worker threads.
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
				dependencyFinished.store(true, std::memory_order_release);
			});
			auto parent = pool.createTask([&dependencyFinished]() {
				return dependencyFinished.load(std::memory_order_acquire);
			});
			parent->addDependency(dependency);

			std::future<bool> parentFuture = parent->getFuture();
			pool.enqueue(parent);

			context.Expect(WaitForValue(context, parentFuture, "Parent tasks should complete after their dependencies finish."), "A parent task should observe completed dependency side effects.");
		});

		registry.Add("Engine.ThreadPool.VoidTaskPoolReusesReleasedTaskObjects", [](TestContext& context) {
			ThreadPoolSession session;
			ThreadPool& pool = ThreadPool::instance();
			Task<void>* firstAddress = nullptr;
			std::atomic<int> runCount = 0;

			{
				auto firstTask = pool.createTask([&runCount]() {
					runCount.fetch_add(1, std::memory_order_acq_rel);
				});
				firstAddress = firstTask.get();
				std::future<void> firstFuture = firstTask->getFuture();
				pool.enqueue(firstTask);
				WaitForVoid(context, firstFuture, "The first pooled void task should complete.");
				firstTask.reset();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));

			auto secondTask = pool.createTask([&runCount]() {
				runCount.fetch_add(1, std::memory_order_acq_rel);
			});
			Task<void>* secondAddress = secondTask.get();
			std::future<void> secondFuture = secondTask->getFuture();
			pool.enqueue(secondTask);
			WaitForVoid(context, secondFuture, "The second pooled void task should complete.");

			context.Expect(firstAddress == secondAddress, "Released void tasks should be returned to the pool and reused.");
			context.ExpectEqual(2, runCount.load(std::memory_order_acquire), "Reused void task objects should reset their callable and promise state.");
		});
	}
}
