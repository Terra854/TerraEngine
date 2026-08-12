// PhysicsJobAdapter.cpp - bounded provider job pumping through the shared pool.

#include "PhysicsJobAdapter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>
#include <vector>

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
		barrierWaitCount.fetch_add(1U, std::memory_order_relaxed);
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
		diagnostics.batchCount = batchCount.load(std::memory_order_acquire);
		diagnostics.lockAcquisitionCount = lockAcquisitionCount.load(std::memory_order_acquire);
		diagnostics.pumpSubmissionCount = pumpSubmissionCount.load(std::memory_order_acquire);
		diagnostics.barrierWaitCount = barrierWaitCount.load(std::memory_order_acquire);
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

	PhysicsIndependentRangeExecution PhysicsJobAdapter::ExecuteIndependentRanges(
		std::size_t itemCount,
		std::size_t batchSize,
		std::size_t parallelThreshold,
		bool providerAdvertisesSafeReads,
		const std::function<void(std::size_t, std::size_t)>& work) const {
		PhysicsIndependentRangeExecution execution;
		if(itemCount == 0U) return execution;
		if(batchSize == 0U || !work) {
			execution.succeeded = false;
			return execution;
		}

		const std::size_t rangeCount = (itemCount + batchSize - 1U) / batchSize;
		execution.rangeCount = static_cast<std::uint32_t>(std::min<std::size_t>(
			rangeCount, std::numeric_limits<std::uint32_t>::max()));
		const bool canRunParallel = providerAdvertisesSafeReads
			&& itemCount >= parallelThreshold
			&& rangeCount > 1U
			&& sharedPool.getThreadCount() > 1U;
		if(!canRunParallel) {
			execution.serialFallback = rangeCount > 1U;
			try {
				for(std::size_t rangeIndex = 0U; rangeIndex < rangeCount; ++rangeIndex) {
					const std::size_t begin = rangeIndex * batchSize;
					work(begin, std::min(begin + batchSize, itemCount));
				}
			} catch(...) {
				execution.succeeded = false;
			}
			return execution;
		}

		execution.parallel = true;
		std::atomic<std::size_t> nextRange{0U};
		std::atomic<bool> failed{false};
		auto consumeRanges = [&]() {
			for(;;) {
				const std::size_t rangeIndex = nextRange.fetch_add(1U, std::memory_order_relaxed);
				if(rangeIndex >= rangeCount) return;
				const std::size_t begin = rangeIndex * batchSize;
				try {
					work(begin, std::min(begin + batchSize, itemCount));
				} catch(...) {
					// Every lane keeps draining its deterministic assignments. The caller
					// receives one failure bit after all tasks have joined, so no task can
					// outlive provider records captured by the extraction call.
					failed.store(true, std::memory_order_release);
				}
			}
		};

		const std::size_t laneCount = std::min(rangeCount, sharedPool.getThreadCount());
		std::vector<std::future<void>> futures;
		futures.reserve(laneCount - 1U);
		for(std::size_t lane = 1U; lane < laneCount; ++lane) {
			auto task = sharedPool.createTask(consumeRanges);
			futures.push_back(task->getFuture());
			sharedPool.enqueue(std::move(task));
		}
		consumeRanges();
		for(std::future<void>& future : futures) {
			try {
				future.get();
			} catch(...) {
				failed.store(true, std::memory_order_release);
			}
		}
		execution.succeeded = !failed.load(std::memory_order_acquire);
		return execution;
	}

	void PhysicsJobAdapter::QueueJob(Job* job) {
		batchCount.fetch_add(1U, std::memory_order_relaxed);
		Job* jobs[] = {job};
		QueueBatch(jobs, 1U);
	}

	void PhysicsJobAdapter::QueueJobs(Job** jobs, JPH::uint jobCount) {
		batchCount.fetch_add(1U, std::memory_order_relaxed);
		QueueBatch(jobs, jobCount);
	}

	void PhysicsJobAdapter::FreeJob(Job* job) {
		delete job;
		liveJobs.fetch_sub(1U, std::memory_order_acq_rel);
	}

	void PhysicsJobAdapter::QueueBatch(Job* const* jobs, JPH::uint jobCount) {
		if(jobs == nullptr || jobCount == 0U) return;
		std::uint32_t accepted = 0U;
		// Acquire every provider reference before publication. The queue then owns
		// those references as one batch until a bounded pump chunk releases them.
		for(JPH::uint index = 0U; index < jobCount; ++index) {
			if(jobs[index] == nullptr) continue;
			jobs[index]->AddRef();
			++accepted;
		}
		if(accepted == 0U) return;
		submitted.fetch_add(accepted, std::memory_order_relaxed);
		queued.fetch_add(accepted, std::memory_order_release);
		std::uint32_t pumpsToSubmit = 0U;
		{
			lockAcquisitionCount.fetch_add(1U, std::memory_order_relaxed);
			std::lock_guard<std::mutex> lock(dispatchMutex);
			for(JPH::uint index = 0U; index < jobCount; ++index) {
				if(jobs[index] != nullptr) pendingJobs.push_back(jobs[index]);
			}
			const std::uint32_t usefulPumps = static_cast<std::uint32_t>(std::min<std::size_t>(
				EffectiveWorkerLimit(), static_cast<std::size_t>(activePumps) + pendingJobs.size()));
			while(activePumps < usefulPumps) {
				++activePumps;
				++pumpsToSubmit;
				std::uint32_t maximum = maximumActivePumps.load(std::memory_order_relaxed);
				while(maximum < activePumps
					&& !maximumActivePumps.compare_exchange_weak(
						maximum, activePumps, std::memory_order_relaxed, std::memory_order_relaxed)) {
				}
			}
		}
		pumpSubmissionCount.fetch_add(pumpsToSubmit, std::memory_order_relaxed);
		for(std::uint32_t pump = 0U; pump < pumpsToSubmit; ++pump) SubmitPump();
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
			constexpr std::size_t jobsPerChunk = 32U;
			std::array<Job*, jobsPerChunk> chunk{};
			std::size_t chunkSize = 0U;
			{
				lockAcquisitionCount.fetch_add(1U, std::memory_order_relaxed);
				std::lock_guard<std::mutex> lock(dispatchMutex);
				if(pendingJobs.empty()) {
					--activePumps;
					idleCondition.notify_all();
					return;
				}
				while(chunkSize < chunk.size() && !pendingJobs.empty()) {
					chunk[chunkSize++] = pendingJobs.front();
					pendingJobs.pop_front();
				}
			}

			const auto begin = std::chrono::steady_clock::now();
			for(std::size_t index = 0U; index < chunkSize; ++index) {
				chunk[index]->Execute();
				chunk[index]->Release();
			}
			const std::uint64_t elapsed = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - begin).count());
			jobExecutionMicroseconds.fetch_add(elapsed, std::memory_order_relaxed);
			completed.fetch_add(chunkSize, std::memory_order_release);
			queued.fetch_sub(static_cast<std::uint32_t>(chunkSize), std::memory_order_acq_rel);
			idleCondition.notify_all();
		}
	}

	std::uint32_t PhysicsJobAdapter::EffectiveWorkerLimit() const noexcept {
		const std::size_t available = std::max<std::size_t>(1U, sharedPool.getThreadCount());
		return static_cast<std::uint32_t>(std::min<std::size_t>(configuration.maxPhysicsWorkers, available));
	}
}
