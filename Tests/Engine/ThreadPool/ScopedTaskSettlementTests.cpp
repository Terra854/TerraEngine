// ScopedTaskSettlementTests.cpp — Deterministic child ownership under failure.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ScopedTaskSettlement.h"

#include <atomic>

namespace TerraEngine::Tests {
	using Tasks::ScopedTaskSettlement;

	namespace {
		// The harness owns the outer test driver, not an engine execution thread.
		// This lets it release a deliberately held engine child even with one
		// frame worker. Gates always cancel before the driver's future is destroyed.
		struct PoolOwner {
			bool owned = ThreadPool::instance().getThreadCount() == 0;
			PoolOwner() { if(owned) ThreadPool::instance().initialize(1); }
			~PoolOwner() { if(owned) ThreadPool::instance().shutdown(); }
		};

		void CheckHeldCleanup(TestContext& context, bool childError)
		{
			PoolOwner owner;
			std::atomic<bool> inputsDiedAfterChild{false};
			Correctness::CompletionCounter settled;
			std::future<void> driver;
			Correctness::ControlledGate held;
			driver = std::async(std::launch::async, [&] {
				struct InputLifetime {
					Correctness::CompletionCounter settled;
					std::atomic<bool>& observed;
					~InputLifetime() { observed.store(settled.Value() == 1); }
				} inputs{settled, inputsDiedAfterChild};
				std::vector<std::future<void>> futures;
				futures.reserve(2);
				ScopedTaskSettlement<void> group(futures);
				auto& pool = ThreadPool::instance();
				if(childError) {
					group.Enqueue(pool, pool.createTask([] { throw std::runtime_error("primary child"); }));
				}
				group.Enqueue(pool, pool.createTask([wait = held.MakeWaiter(), settled] {
					if(wait() != Correctness::GateResult::Released) throw std::runtime_error("held child canceled");
					settled.Increment();
				}));
				if(childError) group.GetAll();
				else throw std::runtime_error("primary caller");
			});
			const bool arrived = held.WaitForArrivals(1);
			held.Release();
			std::string error;
			try { driver.get(); } catch(const std::runtime_error& failure) { error = failure.what(); }
			context.Expect(arrived, "An admitted production pool child must reach its gate.");
			context.Expect(inputsDiedAfterChild.load(), "Captured input destruction must follow physical child completion.");
			context.ExpectEqual(std::string(childError ? "primary child" : "primary caller"), error,
				"Cleanup must preserve the initiating failure, not a sibling outcome.");
		}
	}

	void RegisterScopedTaskSettlementTests(TestRegistry& registry)
	{
		registry.Add("Engine.NestedTaskSettlement.ProductionClosedAdmissionDoesNotTrackFuture", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1);
			pool->shutdown();
			std::vector<std::future<void>> futures;
			futures.reserve(1);
			ScopedTaskSettlement<void> group(futures);
			auto task = pool->createTask([] {});
			bool rejected = false;
			try { group.Enqueue(*pool, task); }
			catch(const std::runtime_error&) { rejected = true; }
			context.Expect(rejected && futures.empty(),
				"Real dependency-free queue rejection must leave no accepted ownership or tracked pending future.");
		});
		registry.Add("Engine.NestedTaskSettlement.UnqueuedPooledVoidRemoved", [](TestContext& context) {
			PoolOwner owner;
			std::vector<std::future<void>> futures;
			futures.reserve(1);
			ScopedTaskSettlement<void> group(futures);
			auto task = ThreadPool::instance().createTask([] {});
			bool rejected = false;
			try {
				group.TrackAndSubmit(task, [](const auto&) { throw std::runtime_error("failed queue insertion"); });
			} catch(const std::runtime_error&) { rejected = true; }
			task.reset(); // Returns to the pool with an unfulfilled promise.
			context.Expect(rejected && futures.empty(), "An unadmitted pooled promise must not enter the wait set.");
		});
		registry.Add("Engine.NestedTaskSettlement.NoThrowUnwind", [](TestContext& context) {
			CheckHeldCleanup(context, false);
		});
		registry.Add("Engine.NestedTaskSettlement.ConsumedFutureSkipped", [](TestContext& context) {
			std::promise<void> promise;
			std::vector<std::future<void>> futures;
			futures.push_back(promise.get_future());
			promise.set_value();
			futures.front().get();
			ScopedTaskSettlement<void> group(futures);
			group.GetAll();
			context.Expect(!futures.front().valid(), "Consumed futures remain invalid and are skipped.");
		});
		registry.Add("Engine.NestedTaskSettlement.HeldSiblingSettled", [](TestContext& context) {
			CheckHeldCleanup(context, true);
		});
		registry.Add("Engine.NestedTaskSettlement.PrimaryExceptionPreserved", [](TestContext& context) {
			PoolOwner owner;
			std::vector<std::future<void>> futures;
			futures.reserve(2);
			ScopedTaskSettlement<void> group(futures);
			auto& pool = ThreadPool::instance();
			group.Enqueue(pool, pool.createTask([] { throw std::runtime_error("first"); }));
			group.Enqueue(pool, pool.createTask([] { throw std::runtime_error("second"); }));
			std::string error;
			try { group.GetAll(); } catch(const std::runtime_error& failure) { error = failure.what(); }
			context.ExpectEqual(std::string("first"), error, "The first child failure remains actionable.");
			context.Expect(!futures[0].valid() && !futures[1].valid(), "Both exceptions must be observed before escape.");
		});
		registry.Add("Engine.NestedTaskSettlement.SuccessfulCollection", [](TestContext& context) {
			PoolOwner owner;
			std::vector<std::future<int>> futures;
			std::vector<int> results;
			futures.reserve(2); results.reserve(2);
			ScopedTaskSettlement<int> group(futures);
			auto& pool = ThreadPool::instance();
			group.Enqueue(pool, pool.createTask([] { return 11; }));
			group.Enqueue(pool, pool.createTask([] { return 29; }));
			group.Collect(results);
			context.Expect(results == std::vector<int>({11, 29}), "Prepared results preserve admitted order.");
		});
	}
}
