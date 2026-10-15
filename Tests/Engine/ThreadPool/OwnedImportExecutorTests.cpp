// OwnedImportExecutorTests.cpp — Isolated capacity and physical executor drain.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "ThreadPool/ThreadPool.h"
#include <typeinfo>

namespace TerraEngine::Tests {
	void RegisterOwnedImportExecutorTests(TestRegistry& registry)
	{
		registry.Add("Engine.OwnedImportExecutor.InitialGraphRejectionSettlesUnsubmittedBranches", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1);
			pool->shutdown();
			auto bodies = std::make_shared<std::atomic<unsigned>>(0);
			auto root = pool->createTask([bodies] { ++*bodies; return 42; });
			auto branch = pool->createTask([bodies] { ++*bodies; });
			auto leaf = pool->createTask([bodies] { ++*bodies; });
			auto sibling = pool->createTask([bodies] { ++*bodies; });
			// Observe actual identities instead of addDependency's fresh clones.
			root->dependenciesList = {branch, sibling};
			branch->setParentTask(root); sibling->setParentTask(root);
			branch->dependenciesList = {leaf}; leaf->setParentTask(branch);
			auto rootFuture = root->getFuture();
			auto branchFuture = branch->getFuture();
			auto leafFuture = leaf->getFuture();
			auto siblingFuture = sibling->getFuture();
			std::exception_ptr original;
			std::string originalMessage;
			try { pool->enqueue(root); }
			catch(const std::runtime_error& error) {
				original = std::current_exception();
				originalMessage = error.what();
				context.Expect(typeid(error) == typeid(std::runtime_error), "The initiating admission error must retain its exact runtime type.");
			}
			context.Expect(static_cast<bool>(original), "Closed admission must retain its caller-visible original exception.");
			context.ExpectEqual(std::string("ThreadPool admission is closed"), originalMessage,
				"The rejected request must identify the actual closed-admission cause.");
			bool preserved = true;
			auto consume = [&](auto& future) {
				if(future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { preserved = false; return; }
				try { (void)future.get(); preserved = false; }
				catch(const std::runtime_error& error) {
					// Standard-library exception_ptr rethrows can copy the exception.
					// Compare exact type and the initiating request's full cause text.
					preserved = preserved && typeid(error) == typeid(std::runtime_error)
						&& std::string(error.what()) == originalMessage;
				}
				catch(...) { preserved = false; }
			};
			consume(rootFuture); consume(branchFuture); consume(leafFuture); consume(siblingFuture);
			context.Expect(preserved && bodies->load() == 0,
				"Unsubmitted branches must settle without callable execution or a whole-graph rollback claim.");
		});
		registry.Add("Engine.OwnedImportExecutor.PositiveCapacityAndStartupRollback", [](TestContext& context) {
			const auto frameWorkers = ThreadPool::instance().getThreadCount();
			bool zeroRejected = false, startupRejected = false;
			try { (void)ThreadPool::CreateOwned(0); }
			catch(const std::invalid_argument&) { zeroRejected = true; }
			try {
				(void)ThreadPool::CreateOwned(3, [](std::size_t index) {
					if(index == 1) throw std::runtime_error("partial startup");
				});
			} catch(const std::runtime_error& error) { startupRejected = std::string(error.what()) == "partial startup"; }
			auto pool = ThreadPool::CreateOwned(2);
			auto task = pool->createTask([] { return true; });
			auto future = task->getFuture(); pool->enqueue(task);
			context.Expect(zeroRejected && startupRejected && future.get(), "Construction rejects zero and joins partial startup before retry.");
			context.ExpectEqual(frameWorkers, ThreadPool::instance().getThreadCount(), "Owned capacity never resizes frame workers.");
		});
		registry.Add("Engine.OwnedImportExecutor.CompletionUsesSubmittingPool", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1);
			auto parent = pool->createTask([] { return true; });
			parent->addDependency(pool->createTask([] {}));
			auto result = parent->getFuture();
			pool->enqueue(parent);
			const bool completed = result.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			context.Expect(completed && result.get(), "A dependency completion must release its parent on the same owned pool.");
		});
		registry.Add("Engine.OwnedImportExecutor.DrainClosesAdmissionAndRetainedHandlesAreSafe", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1);
			auto handle = pool->createTask([] {});
			auto completion = handle->getFuture(); pool->enqueue(handle);
			pool->shutdown();
			completion.get();
			bool rejected = false;
			try { pool->enqueue(pool->createTask([] { return true; })); }
			catch(const std::runtime_error&) { rejected = true; }
			pool.reset();
			handle.reset(); // No reuse-cache deleter may access the destroyed owner.
			context.Expect(rejected, "Physical drain closes external admission before executor destruction.");
		});
		registry.Add("Engine.OwnedImportExecutor.HeldDependencyCompletesDuringDrain", [](TestContext& context) {
			auto pool = ThreadPool::CreateOwned(1);
			std::future<void> drain;
			Correctness::ControlledGate dependency;
			auto parent = pool->createTask([] { return true; });
			parent->addDependency(pool->createTask([wait = dependency.MakeWaiter()] { (void)wait(); }));
			auto completed = parent->getFuture(); pool->enqueue(parent);
			context.Expect(dependency.WaitForArrivals(1), "A real dependency must remain physically owned when drain starts.");
			drain = std::async(std::launch::async, [&] { pool->shutdown(); });
			// Observe actual admission closure while the dependency remains held.
			// Any probes admitted before closure are real queued tasks and drain too.
			bool closed = false;
			const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			while(!closed && std::chrono::steady_clock::now() < deadline) {
				try { pool->enqueue(pool->createTask([] { return true; })); }
				catch(const std::runtime_error&) { closed = true; }
				if(!closed) std::this_thread::yield();
			}
			const bool heldDrain = drain.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout;
			dependency.Release(); drain.get();
			context.Expect(closed && heldDrain && completed.get(),
				"Closed external admission must still allow the owned dependency to release and execute its parent before drain returns.");
		});
		registry.Add("Engine.OwnedImportExecutor.SingleFrameWorkerProgress", [](TestContext& context) {
			auto& frame = ThreadPool::instance();
			const bool frameOwned = frame.getThreadCount() == 0;
			if(frameOwned) frame.initialize(1);
			struct FrameCleanup {
				bool owned;
				~FrameCleanup() { if(owned) ThreadPool::instance().shutdown(); }
			} cleanup{frameOwned};
			auto imports = ThreadPool::CreateOwned(1);
			Correctness::ControlledGate compiler;
			auto import = imports->createTask([wait = compiler.MakeWaiter()] { return wait() == Correctness::GateResult::Released; });
			auto imported = import->getFuture(); imports->enqueue(import);
			const bool arrived = compiler.WaitForArrivals(1);
			auto sentinel = frame.createTask([] { return true; });
			auto progressed = sentinel->getFuture(); frame.enqueue(sentinel);
			const bool beforeRelease = progressed.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			compiler.Release();
			const bool importFinished = imported.get();
			const bool frameFinished = progressed.get();
			context.Expect(arrived && beforeRelease && importFinished && frameFinished,
				"A frame sentinel must run while an isolated compiler worker remains held.");
		});
	}
}
