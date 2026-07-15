// PhysicsJobAdapter.h - lowers Jolt jobs onto TerraEngine's shared ThreadPool.
//
// This header is provider-private and may include Jolt. No gameplay-facing or
// serialized header includes it. The adapter owns provider job bookkeeping but
// never creates a recurring worker thread or a second thread pool.

#pragma once

#include "Core/Export.h"
#include "ThreadPool/ThreadPool.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Core/Memory.h>

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace TerraEngine::Physics::Backend {
	struct PhysicsJobAdapterConfig {
		// Zero means use every worker currently owned by the shared engine pool.
		std::uint32_t maxPhysicsWorkers = 0;
		std::uint32_t maxBarriers = 8U;
		std::uint32_t maxJobs = 2048U;
	};

	struct PhysicsJobAdapterDiagnostics {
		std::uint64_t submitted = 0;
		std::uint64_t completed = 0;
		std::uint64_t rejected = 0;
		std::uint32_t queued = 0;
		std::uint32_t activePumps = 0;
		std::uint32_t maxActivePumps = 0;
		std::uint32_t workerLimit = 0;
		std::uint32_t observedWorkerThreads = 0;
		std::uint64_t barrierWaitMicroseconds = 0;
		std::uint64_t maximumBarrierWaitMicroseconds = 0;
		std::uint64_t jobExecutionMicroseconds = 0;
	};

	struct PhysicsIndependentRangeExecution {
		bool succeeded = true;
		bool parallel = false;
		bool serialFallback = false;
		std::uint32_t rangeCount = 0U;
	};

	class TERRAENGINE_API PhysicsJobAdapter final : public JPH::JobSystemWithBarrier {
	public:
		PhysicsJobAdapter(ThreadPool& sharedPool, PhysicsJobAdapterConfig configuration = {});
		~PhysicsJobAdapter() override;

		PhysicsJobAdapter(const PhysicsJobAdapter&) = delete;
		PhysicsJobAdapter& operator=(const PhysicsJobAdapter&) = delete;

		int GetMaxConcurrency() const override;
		JobHandle CreateJob(
			const char* name,
			JPH::ColorArg color,
			const JobFunction& function,
			JPH::uint32 dependencyCount = 0) override;
		void WaitForJobs(Barrier* barrier) override;

		void RequestStop() noexcept;
		bool IsStopRequested() const noexcept;
		bool IsAcceptingJobs() const noexcept;
		void WaitForIdle();
		const PhysicsJobAdapterConfig& Config() const noexcept;
		PhysicsJobAdapterDiagnostics Diagnostics() const;
		// Provider-neutral extraction work writes each deterministic output range
		// independently. One lane stays on the caller, preventing nested extraction
		// from waiting behind every worker in the shared pool.
		PhysicsIndependentRangeExecution ExecuteIndependentRanges(
			std::size_t itemCount,
			std::size_t batchSize,
			std::size_t parallelThreshold,
			bool providerAdvertisesSafeReads,
			const std::function<void(std::size_t, std::size_t)>& work) const;

	protected:
		void QueueJob(Job* job) override;
		void QueueJobs(Job** jobs, JPH::uint jobCount) override;
		void FreeJob(Job* job) override;

	private:
		void QueueOne(Job* job);
		void SubmitPump();
		void PumpJobs();
		std::uint32_t EffectiveWorkerLimit() const noexcept;

		ThreadPool& sharedPool;
		PhysicsJobAdapterConfig configuration;
		std::atomic<bool> acceptingJobs{true};
		std::atomic<bool> stopRequested{false};
		std::atomic<std::uint32_t> liveJobs{0};
		std::atomic<std::uint64_t> submitted{0};
		std::atomic<std::uint64_t> completed{0};
		std::atomic<std::uint64_t> rejected{0};
		std::atomic<std::uint32_t> queued{0};
		std::atomic<std::uint32_t> maximumActivePumps{0};
		std::atomic<std::uint64_t> barrierWaitMicroseconds{0};
		std::atomic<std::uint64_t> maximumBarrierWaitMicroseconds{0};
		std::atomic<std::uint64_t> jobExecutionMicroseconds{0};
		std::deque<Job*> pendingJobs;
		std::uint32_t activePumps = 0;
		mutable std::mutex dispatchMutex;
		std::condition_variable idleCondition;
		mutable std::mutex workerIdentityMutex;
		std::unordered_set<std::thread::id> workerThreadIds;
	};
}
