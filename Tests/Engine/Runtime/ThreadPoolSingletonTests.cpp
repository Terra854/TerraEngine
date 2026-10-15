#include "TestHarness.h"

#include "ThreadPool/ThreadPool.h"
#include "CorrectnessTestSupport.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Native-only access stages the same admission state as a newly constructed,
// unstarted executor. Production admission and shutdown still do all queue work.
struct ThreadPoolTestAccess {
	struct CacheSnapshot {
		std::uint64_t generation;
		std::size_t outstanding;
		std::size_t cached;
		bool recycling;
	};
	static CacheSnapshot Snapshot(ThreadPool& pool) {
		std::lock_guard lock(pool.poolMutex);
		return {pool.cacheGeneration, pool.outstandingVoidTasks, pool.voidTaskPool.size(),
			pool.recyclingState == ThreadPool::RecyclingState::Recycling};
	}
	static void StageUnstartedTask(ThreadPool& pool, const std::shared_ptr<ITask>& task) {
		{
			std::lock_guard lock(pool.queue_mutex);
			if(!pool.workers.empty() || !pool.tasks.empty()) {
				throw std::logic_error("Unstarted queue staging requires quiescent empty ownership");
			}
			pool.stop = false;
		}
		pool.enqueue(task);
	}
};

namespace TerraEngine::Tests {
	namespace {
		void EmitCacheObservation(std::string_view caseName, ThreadPool& pool,
			std::size_t actualWorkers, std::size_t destroyed, std::size_t expected, bool residualReentry = false,
			bool residualExternalTerminal = false)
		{
			const auto cache = ThreadPoolTestAccess::Snapshot(pool);
			// Build evidence on the test thread and insert one complete line, so
			// asynchronous worker logging cannot split JSON between individual fields.
			std::ostringstream observation;
			observation << "\nRMS_OBSERVATION {\"caseId\":\"Engine.RuntimeMemorySafety.TaskCache."
				<< caseName << "\",\"actualWorkers\":" << actualWorkers << ",\"destroyedCaptures\":" << destroyed
				<< ",\"expectedDestroyedCaptures\":" << expected
				<< ",\"cacheGeneration\":" << cache.generation
				<< ",\"outstandingOwners\":" << cache.outstanding
				<< ",\"cachedTasks\":" << cache.cached
				<< ",\"recyclingOpen\":" << (cache.recycling ? "true" : "false")
				<< ",\"drainComplete\":" << (pool.getThreadCount() == 0 ? "true" : "false")
				<< ",\"residualQueueReentry\":" << (residualReentry ? "true" : "false")
				<< ",\"residualExternalFutureTerminal\":" << (residualExternalTerminal ? "true" : "false")
				<< "}\n";
			std::cout << observation.str();
			if(cache.outstanding != 0 || cache.cached != 0 || cache.recycling || actualWorkers != 1) {
				throw TestFailure("Shutdown must balance actual external owners and close an empty cache after one-worker execution");
			}
		}
		// Captured probes expose actual callable destruction, rather than treating
		// future readiness as proof that a cached task released its captures.
		struct CacheLifetimeProbe {
			explicit CacheLifetimeProbe(Correctness::CompletionCounter value) : destroyed(std::move(value)) {}
			~CacheLifetimeProbe() { destroyed.Increment(); }
			Correctness::CompletionCounter destroyed;
		};

		struct CacheTestSession {
			ThreadPool& pool = ThreadPool::instance();
			std::size_t actualWorkers = 0;
			CacheTestSession() { pool.shutdown(); pool.initialize(1); actualWorkers = pool.getThreadCount(); }
			~CacheTestSession() { pool.shutdown(); }
		};

		// Some standard-library mutexes reject same-thread recursive locking with
		// an exception, which ReleaseVoidTask deliberately catches. Retire the
		// captured inner handle on an already-started external test driver instead:
		// a locked outer destruction now creates real cross-thread contention on
		// every host. The driver strongly owns its shared state until this capture
		// joins it; the independent process watchdog bounds old code.
		struct CapturedCacheOwner {
			struct State { std::shared_ptr<Task<void>> inner; };
			std::future<void> driver;
			Correctness::ControlledGate release;
			std::shared_ptr<State> state;

			explicit CapturedCacheOwner(std::shared_ptr<Task<void>> value)
				: state(std::make_shared<State>(State{std::move(value)})) {
				driver = std::async(std::launch::async,
					[state = state, wait = release.MakeWaiter(std::chrono::seconds(30))] {
						(void)wait();
						state->inner.reset();
					});
				if(!release.WaitForArrivals(1)) {
					release.Release();
					throw std::runtime_error("Captured-owner retirement driver did not start");
				}
			}
			~CapturedCacheOwner() {
				release.Release();
				driver.get();
			}
		};

		// Return only the outer handle: its callable is the sole owner of the
		// stable capture that owns/joins the inner handle's retirement driver.
		// Both callables retain independently counted lifetime probes.
		std::shared_ptr<Task<void>> MakeNestedCacheTask(ThreadPool& pool,
			const Correctness::CompletionCounter& destroyed)
		{
			auto inner = pool.createTask([probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {});
			return pool.createTask([owner = std::make_shared<CapturedCacheOwner>(std::move(inner)),
				probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {});
		}

		// A shutdown owner can observe admission closure while its sole worker is
		// held. No sleep or scheduler-speed assumption establishes this ordering.
		bool ObserveClosedAdmission(ThreadPool& pool)
		{
			const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			while(std::chrono::steady_clock::now() < deadline) {
				try { pool.enqueue(pool.createTask([] { return true; })); }
				catch(const std::runtime_error&) { return true; }
				std::this_thread::yield();
			}
			return false;
		}

		void CheckNestedCacheShutdown(TestContext& context, bool dispatch)
		{
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			auto outer = MakeNestedCacheTask(session.pool, destroyed);
			if(dispatch) {
				auto completed = outer->getFuture();
				session.pool.enqueue(outer);
				context.Expect(completed.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
					"The real outer callable must finish before testing cache retirement.");
				completed.get();
			}
			outer.reset();
			// The independent installed-process watchdog bounds the original lock
			// cycle here; an in-process timeout cannot cancel this production lock.
			session.pool.shutdown();
			EmitCacheObservation(dispatch ? "CompletedNestedCapture" : "NeverDispatchedNestedCapture",
				session.pool, session.actualWorkers, destroyed.Value(), 2);
			context.ExpectEqual(2U, destroyed.Value(), "Both nested callable lifetimes must end during shutdown.");
		}

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
		registry.Add("Engine.ThreadPool.CallableFailureStillRunsDependentAndInlineTask", [](TestContext& context) {
			ThreadPoolSession session;
			auto& pool = ThreadPool::instance();
			auto parent = pool.createTask([] { return 42; });
			parent->addDependency(pool.createTask([] { throw std::runtime_error("ordinary dependency body failure"); }));
			auto result = parent->getFuture();
			pool.enqueue(parent);
			context.Expect(result.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"An ordinary dependency callable error must not acquire infrastructure cancellation semantics.");
			context.ExpectEqual(42, result.get(), "Existing parent execution after a callable failure must remain valid.");
			Task<int> inlineTask([] { return 73; });
			auto inlineFuture = inlineTask.getFuture();
			inlineTask.run();
			context.ExpectEqual(73, inlineFuture.get(), "Resource-style direct inline execution must retain typed results.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.CompletedNestedCapture", [](TestContext& context) {
			CheckNestedCacheShutdown(context, true);
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.NeverDispatchedNestedCapture", [](TestContext& context) {
			CheckNestedCacheShutdown(context, false);
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.DeepCapturedOwnership", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			std::shared_ptr<Task<void>> chain;
			constexpr unsigned depth = 128;
			for(unsigned index = 0; index < depth; ++index) {
				chain = session.pool.createTask([previous = std::move(chain),
					probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {});
			}
			chain.reset();
			session.pool.shutdown();
			EmitCacheObservation("DeepCapturedOwnership", session.pool, session.actualWorkers, destroyed.Value(), depth);
			context.ExpectEqual(depth, destroyed.Value(), "Every level must retire exactly once without recursive lock acquisition.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.ReleaseDuringShutdown", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			Correctness::ControlledGate held;
			auto running = session.pool.createTask([wait = held.MakeWaiter()] { return wait(); });
			auto finished = running->getFuture();
			session.pool.enqueue(running);
			const bool arrived = held.WaitForArrivals(1);
			auto late = MakeNestedCacheTask(session.pool, destroyed);
			auto drain = std::async(std::launch::async, [&] { session.pool.shutdown(); });
			const bool closed = ObserveClosedAdmission(session.pool);
			late.reset();
			const bool retiredDuringJoin = destroyed.Value() == 2;
			held.Release();
			drain.get();
			EmitCacheObservation("ReleaseDuringShutdown", session.pool, session.actualWorkers, destroyed.Value(), 2);
			context.Expect(arrived && closed && retiredDuringJoin,
				"Closed recycling must destroy late owners while worker join is still held.");
			context.Expect(finished.get() == Correctness::GateResult::Released, "Shutdown must drain admitted work.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.ReleaseAfterShutdown", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			auto late = session.pool.createTask([probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {});
			session.pool.shutdown();
			context.ExpectEqual(0U, destroyed.Value(), "A retained external handle must keep its callable alive.");
			late.reset();
			const auto observed = destroyed.Value();
			session.pool.shutdown();
			EmitCacheObservation("ReleaseAfterShutdown", session.pool, session.actualWorkers, observed, 1);
			context.ExpectEqual(1U, observed, "Release after shutdown must destroy immediately instead of repopulating the cache.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.PriorGenerationAfterRestart", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			const auto previousGeneration = ThreadPoolTestAccess::Snapshot(session.pool).generation;
			auto previous = session.pool.createTask([probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {});
			session.pool.shutdown();
			session.pool.initialize(1);
			const auto currentGeneration = ThreadPoolTestAccess::Snapshot(session.pool).generation;
			previous.reset();
			const bool retiredBeforeNewAllocation = destroyed.Value() == 1;
			auto healthy = session.pool.createTask([] { return 42; });
			auto result = healthy->getFuture();
			session.pool.enqueue(healthy);
			context.Expect(result.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"The new cache generation must permit healthy dispatch.");
			context.ExpectEqual(42, result.get(), "Restart must preserve results.");
			session.pool.shutdown();
			EmitCacheObservation("PriorGenerationAfterRestart", session.pool, session.actualWorkers, destroyed.Value(), 1);
			context.Expect(currentGeneration > previousGeneration, "Successful restart must advance the cache generation.");
			context.Expect(retiredBeforeNewAllocation, "A stale return must never be reused by the next generation.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.QueuedCaptureReentry", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			// This capture creates/releases another pooled owner in its destructor.
			// Queue drain and subsequent cache retirement must run that destructor
			// outside both queue_mutex and poolMutex.
			struct ReentrantCapture {
				ThreadPool& pool;
				Correctness::CompletionCounter destroyed;
				ReentrantCapture(ThreadPool& value, Correctness::CompletionCounter counter)
					: pool(value), destroyed(std::move(counter)) {}
				~ReentrantCapture() {
					auto nested = pool.createTask([] {});
					nested.reset();
					destroyed.Increment();
				}
			};
			Correctness::ControlledGate held;
			auto first = session.pool.createTask([wait = held.MakeWaiter()] { return wait(); });
			auto firstResult = first->getFuture();
			session.pool.enqueue(first);
			const bool arrived = held.WaitForArrivals(1);
			auto queued = session.pool.createTask([capture = std::make_shared<ReentrantCapture>(session.pool, destroyed)] {});
			auto result = queued->getFuture();
			session.pool.enqueue(queued);
			queued.reset();
			auto drain = std::async(std::launch::async, [&] { session.pool.shutdown(); });
			const bool closed = ObserveClosedAdmission(session.pool);
			held.Release();
			drain.get();
			result.get();
			context.Expect(arrived && closed && firstResult.get() == Correctness::GateResult::Released,
				"The captured destructor must belong to genuinely queued work drained at shutdown.");
			context.ExpectEqual(1U, destroyed.Value(), "Reentrant drained ownership must retire once outside executor locks.");

			// Also exercise actual residual queue retirement, which normal workers
			// would otherwise drain before shutdown reaches its cleanup branch.
			std::atomic<bool> residualReentry{false};
			struct ResidualCapture {
				ThreadPool& pool;
				std::atomic<bool>& closed;
				Correctness::CompletionCounter destroyed;
				ResidualCapture(ThreadPool& owner, std::atomic<bool>& observed,
					Correctness::CompletionCounter counter) : pool(owner), closed(observed), destroyed(counter) {}
				~ResidualCapture() {
					try { pool.enqueue(pool.createTask([] { return true; })); }
					catch(const std::runtime_error&) { closed.store(true); }
					destroyed.Increment();
				}
			};
			auto residual = session.pool.createTask([
				capture = std::make_shared<ResidualCapture>(session.pool, residualReentry, destroyed)] {});
			auto residualFuture = residual->getFuture();
			ThreadPoolTestAccess::StageUnstartedTask(session.pool, residual);
			residual.reset();
			auto retained = session.pool.createTask([] { return true; });
			auto retainedFuture = retained->getFuture();
			session.pool.enqueue(retained);
			session.pool.shutdown();
			const bool retainedTerminal = retainedFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			bool cancelled = false;
			if(retainedTerminal) {
				try { (void)retainedFuture.get(); }
				catch(const std::runtime_error& error) {
					cancelled = std::string_view(error.what()).find("shut down") != std::string_view::npos;
				}
			}
			bool residualTerminal = false;
			if(residualFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
				try { residualFuture.get(); }
				catch(const std::runtime_error&) { residualTerminal = true; }
			}
			EmitCacheObservation("QueuedCaptureReentry", session.pool, session.actualWorkers, destroyed.Value(), 2,
				residualReentry.load(), retainedTerminal && cancelled);
			context.Expect(residualReentry.load() && retainedTerminal && cancelled && residualTerminal,
				"Residual captures must reenter outside queue locks; retained unstarted tasks must already have terminal cancellation.");
			context.ExpectEqual(2U, destroyed.Value(), "Both drained and residual capture destructors must run exactly once.");
		});
		registry.Add("Engine.RuntimeMemorySafety.TaskCache.OutstandingAccountingAndReinitialize", [](TestContext& context) {
			CacheTestSession session;
			Correctness::CompletionCounter destroyed;
			std::vector<std::shared_ptr<Task<void>>> previous;
			std::vector<std::shared_ptr<Task<void>>> current;
			constexpr unsigned count = 96;
			for(unsigned index = 0; index < count; ++index) {
				previous.push_back(session.pool.createTask([probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {}));
			}
			session.pool.shutdown();
			session.pool.initialize(1);
			for(unsigned index = 0; index < count; ++index) {
				current.push_back(session.pool.createTask([probe = std::make_shared<CacheLifetimeProbe>(destroyed)] {}));
			}
			previous.clear();
			const bool oldOwnersRetired = destroyed.Value() == count;
			current.clear();
			session.pool.shutdown();
			session.pool.shutdown();
			EmitCacheObservation("OutstandingAccountingAndReinitialize", session.pool, session.actualWorkers, destroyed.Value(), count * 2);
			context.Expect(oldOwnersRetired, "Outstanding old-generation owners must retire without consuming the live generation cache.");
			context.ExpectEqual(count * 2, destroyed.Value(), "Actual captured ownership must balance across generations and repeated shutdown.");
			session.pool.initialize(1);
			auto healthy = session.pool.createTask([] {});
			auto result = healthy->getFuture();
			session.pool.enqueue(healthy);
			context.Expect(result.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Accounting and cache retirement must not strand a later generation.");
			result.get();
		});

		registry.Add("Engine.ThreadPool.ExplicitWorkerCountIsNotHardwareCapped", [](TestContext& context) {
			ThreadPool& pool = ThreadPool::instance();
			pool.shutdown();
			pool.initialize(1);
			context.ExpectEqual(1ULL, static_cast<unsigned long long>(pool.getThreadCount()), "An explicit one-worker request should start exactly one worker.");
			pool.shutdown();
			pool.initialize(2);
			context.ExpectEqual(2ULL, static_cast<unsigned long long>(pool.getThreadCount()),
				"Explicit stress-matrix worker requests must not be silently capped by hardware concurrency.");
			pool.shutdown();
		});

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
