// PhysicsJobAdapter.cpp - bounded provider job pumping through the shared pool.

#include "PhysicsJobAdapter.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace TerraEngine::Physics::Backend {
	PhysicsJobAdapter::PhysicsJobAdapter(ThreadPool& sharedPool, PhysicsJobAdapterConfig configuration)
		: JPH::JobSystemWithBarrier(configuration.maxBarriers),
		sharedPool(sharedPool),
		configuration(configuration) {
		if(JPH::Allocate == nullptr || JPH::Free == nullptr) {
			throw std::invalid_argument("PhysicsJobAdapter requires installed Jolt allocation callbacks.");
		}
		if(sharedPool.getThreadCount() == 0U) {
			throw std::invalid_argument("PhysicsJobAdapter requires an initialized TerraEngine ThreadPool.");
		}
		if(this->configuration.maxBarriers == 0U || this->configuration.maxJobs == 0U) {
			throw std::invalid_argument("PhysicsJobAdapter capacities must be greater than zero.");
		}
		if(this->configuration.maxPhysicsWorkers == 0U) {
			this->configuration.maxPhysicsWorkers = static_cast<std::uint32_t>(sharedPool.getThreadCount());
		}
	}

	PhysicsJobAdapter::~PhysicsJobAdapter() {
		// PhysicsWorld owns the adapter and destroys it before ThreadPool shutdown.
		// Draining here prevents queued callbacks from retaining this object.
		RequestStop();
		WaitForIdle();
	}

	int PhysicsJobAdapter::GetMaxConcurrency() const {
		if(sharedPool.getThreadCount() == 1U) {
			// Serial mode runs ready provider jobs directly in QueueOne, so Jolt must
			// not expect an additional worker or barrier-waiter execution lane.
			return 1;
		}
		// Jolt's barrier waiter can execute a ready job while it waits. Report that
		// caller lane in addition to the bounded number of shared worker pumps.
		return static_cast<int>(EffectiveWorkerLimit() + 1U);
	}

	JPH::JobSystem::JobHandle PhysicsJobAdapter::CreateJob(
		const char* name,
		JPH::ColorArg color,
		const JobFunction& function,
		JPH::uint32 dependencyCount) {
		if(!acceptingJobs.load(std::memory_order_acquire)) {
			rejected.fetch_add(1U, std::memory_order_relaxed);
			throw std::runtime_error("Physics provider attempted to create a job after adapter shutdown began.");
		}

		std::uint32_t observed = liveJobs.load(std::memory_order_acquire);
		for(;;) {
			if(observed >= configuration.maxJobs) {
				rejected.fetch_add(1U, std::memory_order_relaxed);
				throw std::runtime_error("Physics provider exceeded the configured fixed job capacity.");
			}
			if(liveJobs.compare_exchange_weak(
				observed,
				observed + 1U,
				std::memory_order_acq_rel,
				std::memory_order_acquire)) {
				break;
			}
		}

		Job* job = nullptr;
		try {
			job = new Job(name, color, this, function, dependencyCount);
		} catch(...) {
			liveJobs.fetch_sub(1U, std::memory_order_acq_rel);
			throw;
		}

		JobHandle handle(job);
		if(dependencyCount == 0U) {
			QueueJob(job);
		}
		return handle;
	}

	void PhysicsJobAdapter::WaitForJobs(Barrier* barrier) {
		if(barrier == nullptr) {
			return;
		}

		const auto begin = std::chrono::steady_clock::now();
		JPH::JobSystemWithBarrier::WaitForJobs(barrier);
		// The caller may have executed a job before a queued pump consumed its
		// reference. Drain those pump references before the barrier can be destroyed.
		WaitForIdle();
		const std::uint64_t elapsed = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now() - begin).count());
		barrierWaitMicroseconds.fetch_add(elapsed, std::memory_order_relaxed);
		std::uint64_t maximum = maximumBarrierWaitMicroseconds.load(std::memory_order_relaxed);
		while(maximum < elapsed
			&& !maximumBarrierWaitMicroseconds.compare_exchange_weak(
				maximum, elapsed, std::memory_order_relaxed, std::memory_order_relaxed)) {
		}
	}

	void PhysicsJobAdapter::RequestStop() noexcept {
		stopRequested.store(true, std::memory_order_release);
		acceptingJobs.store(false, std::memory_order_release);
	}

	bool PhysicsJobAdapter::IsStopRequested() const noexcept {
		return stopRequested.load(std::memory_order_acquire);
	}

	bool PhysicsJobAdapter::IsAcceptingJobs() const noexcept {
		return acceptingJobs.load(std::memory_order_acquire);
	}

	void PhysicsJobAdapter::WaitForIdle() {
		std::unique_lock<std::mutex> lock(dispatchMutex);
		idleCondition.wait(lock, [this] {
			return pendingJobs.empty()
				&& queued.load(std::memory_order_acquire) == 0U
				&& activePumps == 0U;
		});
	}

	const PhysicsJobAdapterConfig& PhysicsJobAdapter::Config() const noexcept {
		return configuration;
	}

	PhysicsJobAdapterDiagnostics PhysicsJobAdapter::Diagnostics() const {
		PhysicsJobAdapterDiagnostics diagnostics;
		diagnostics.submitted = submitted.load(std::memory_order_acquire);
		diagnostics.completed = completed.load(std::memory_order_acquire);
		diagnostics.rejected = rejected.load(std::memory_order_acquire);
		diagnostics.queued = queued.load(std::memory_order_acquire);
		diagnostics.maxActivePumps = maximumActivePumps.load(std::memory_order_acquire);
		diagnostics.workerLimit = EffectiveWorkerLimit();
		diagnostics.barrierWaitMicroseconds = barrierWaitMicroseconds.load(std::memory_order_acquire);
		diagnostics.maximumBarrierWaitMicroseconds = maximumBarrierWaitMicroseconds.load(std::memory_order_acquire);
		diagnostics.jobExecutionMicroseconds = jobExecutionMicroseconds.load(std::memory_order_acquire);
		{
			std::lock_guard<std::mutex> lock(dispatchMutex);
			diagnostics.activePumps = activePumps;
		}
		{
			std::lock_guard<std::mutex> lock(workerIdentityMutex);
			diagnostics.observedWorkerThreads = static_cast<std::uint32_t>(workerThreadIds.size());
		}
		return diagnostics;
	}

	void PhysicsJobAdapter::QueueJob(Job* job) {
		QueueOne(job);
	}

	void PhysicsJobAdapter::QueueJobs(Job** jobs, JPH::uint jobCount) {
		for(JPH::uint index = 0; index < jobCount; ++index) {
			QueueOne(jobs[index]);
		}
	}

	void PhysicsJobAdapter::FreeJob(Job* job) {
		delete job;
		liveJobs.fetch_sub(1U, std::memory_order_acq_rel);
	}

	void PhysicsJobAdapter::QueueOne(Job* job) {
		if(job == nullptr) {
			return;
		}

		// The queue owns one Jolt reference. If the barrier waiter executes the job
		// first, the pump's Execute call becomes a safe no-op before releasing it.
		job->AddRef();
		submitted.fetch_add(1U, std::memory_order_relaxed);
		queued.fetch_add(1U, std::memory_order_release);

		bool submitPump = false;
		{
			std::lock_guard<std::mutex> lock(dispatchMutex);
			pendingJobs.push_back(job);
			if(activePumps < EffectiveWorkerLimit()) {
				++activePumps;
				std::uint32_t maximum = maximumActivePumps.load(std::memory_order_relaxed);
				while(maximum < activePumps
					&& !maximumActivePumps.compare_exchange_weak(
						maximum, activePumps, std::memory_order_relaxed, std::memory_order_relaxed)) {
				}
				submitPump = true;
			}
		}

		if(submitPump) {
			SubmitPump();
		}
	}

	void PhysicsJobAdapter::SubmitPump() {
		// One engine task drains multiple provider jobs, bounding scheduler overhead
		// and preserving workers for unrelated frame systems.
		if(sharedPool.getThreadCount() == 1U) {
			// The scheduler can run PhysicsSimulate on this same sole worker. Queueing
			// its provider pump behind the caller would self-starve WaitForIdle, so
			// serial mode executes the ready bounded queue immediately instead.
			PumpJobs();
			return;
		}
		auto task = sharedPool.createTask([this] {
			PumpJobs();
		});
		sharedPool.enqueue(std::move(task));
	}

	void PhysicsJobAdapter::PumpJobs() {
		{
			std::lock_guard<std::mutex> lock(workerIdentityMutex);
			workerThreadIds.insert(std::this_thread::get_id());
		}

		for(;;) {
			Job* job = nullptr;
			{
				std::lock_guard<std::mutex> lock(dispatchMutex);
				if(pendingJobs.empty()) {
					--activePumps;
					idleCondition.notify_all();
					return;
				}
				job = pendingJobs.front();
				pendingJobs.pop_front();
			}

			const auto begin = std::chrono::steady_clock::now();
			job->Execute();
			const std::uint64_t elapsed = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - begin).count());
			jobExecutionMicroseconds.fetch_add(elapsed, std::memory_order_relaxed);
			job->Release();
			completed.fetch_add(1U, std::memory_order_release);
			queued.fetch_sub(1U, std::memory_order_acq_rel);
			idleCondition.notify_all();
		}
	}

	std::uint32_t PhysicsJobAdapter::EffectiveWorkerLimit() const noexcept {
		const std::size_t available = std::max<std::size_t>(1U, sharedPool.getThreadCount());
		return static_cast<std::uint32_t>(std::min<std::size_t>(configuration.maxPhysicsWorkers, available));
	}
}
