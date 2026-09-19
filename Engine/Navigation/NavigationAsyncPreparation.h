// NavigationAsyncPreparation.h - bounded, lifetime-safe background build coordination.

#pragma once

#include "Core/Export.h"
#include "Navigation/NavigationBudget.h"
#include "Navigation/NavigationTypes.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Navigation
{
	// A task identity contains every lifetime fact needed to reject a completed
	// worker result without consulting a mutable world or live scene object.
	struct NavigationAsyncBuildIdentity
	{
		NavigationWorldHandle world{};
		std::uint64_t lifecycleEpoch = 0U;
		std::uint64_t groupSequence = 0U;
		std::uint64_t profileBuildKey = 0U;
		// Distinguish recovery work from initial/profile builds without retaining packets.
		bool dynamicRebuild = false;

		[[nodiscard]] constexpr bool operator==(const NavigationAsyncBuildIdentity&) const noexcept = default;
	};

	// This is a count-and-byte-only observation. It deliberately does not expose
	// packets, futures, or provider output to diagnostics and test consumers.
	struct NavigationAsyncBuildStatus
	{
		std::size_t currentCount = 0U;
		std::size_t retiredCount = 0U;
		std::size_t capacity = 0U;
		std::size_t chargedBytes = 0U;
		std::size_t peakChargedBytes = 0U;
		std::uint64_t admittedCount = 0U;
		std::uint64_t completedCount = 0U;
		std::uint64_t staleDiscardCount = 0U;
		std::uint64_t admissionRejectedCount = 0U;
		// Cumulative observations include failed and retired completions. Queue time
		// starts at admission; publication wait ends when the protected host polls.
		std::uint64_t groundCompletedCount = 0U;
		std::uint64_t dynamicCompletedCount = 0U;
		std::uint64_t totalWorkerNanoseconds = 0U;
		std::uint64_t maxWorkerNanoseconds = 0U;
		std::uint64_t totalQueueWaitNanoseconds = 0U;
		std::uint64_t maxQueueWaitNanoseconds = 0U;
		std::uint64_t totalPublicationWaitNanoseconds = 0U;
		std::uint64_t maxPublicationWaitNanoseconds = 0U;
	};

	// Completion reports only task ownership and execution state. NavigationSystem
	// owns the typed value result beside this coordinator and reads it only after
	// this future has completed, which supplies the required happens-before edge.
	struct NavigationAsyncBuildCompletion
	{
		NavigationAsyncBuildIdentity identity{};
		bool retired = false;
		bool succeeded = false;
		std::string failureDetail;
	};

	// Coordinates a deliberately small number of expensive immutable provider
	// jobs. It never captures NavigationSystem, ECS, a NavigationWorld root, or a
	// mutable provider pointer; callers put those rules in their copied work lambda.
	// Its control methods are intentionally protected-boundary serialized: the
	// worker touches only the copied WorkFunction, never this coordinator state.
	class TERRAENGINE_API NavigationAsyncBuildCoordinator final
	{
	public:
		using WorkFunction = std::function<void()>;
		struct Submission
		{
			NavigationAsyncBuildIdentity identity{};
			std::shared_ptr<NavigationBudget> budget;
			std::size_t chargedBytes = 0U;
			WorkFunction work;
		};

		explicit NavigationAsyncBuildCoordinator(std::size_t capacity = 1U);
		NavigationAsyncBuildCoordinator(const NavigationAsyncBuildCoordinator&) = delete;
		NavigationAsyncBuildCoordinator& operator=(const NavigationAsyncBuildCoordinator&) = delete;

		// Reserves BuildScratch bytes before dispatching. The reservation is shared
		// by the queued worker and the coordinator so retiring a world cannot free
		// admission capacity while the worker still retains its copied packet.
		[[nodiscard]] bool TrySubmit(NavigationAsyncBuildIdentity identity,
			std::shared_ptr<NavigationBudget> budget, std::size_t chargedBytes,
			WorkFunction work, std::string& rejectionReason);
		// A wave reserves every child's capacity and scratch bytes before any task
		// is queued. Rejection releases all provisional permits without running work.
		// Accepted children complete independently; publication remains caller-owned.
		// True means every entry was admitted, not that every enqueue/execution
		// succeeded. Dispatch failure is a failed completion for the admitted child;
		// returning false after another child started would permit unsafe resubmission.
		[[nodiscard]] bool TrySubmitBatch(std::vector<Submission> submissions,
			std::string& rejectionReason);
		// A lifetime transition moves active jobs to the retired ledger. They remain
		// charged and count against capacity until PollCompleted observes completion.
		void RetireCurrent();
		[[nodiscard]] std::vector<NavigationAsyncBuildCompletion> PollCompleted();
		[[nodiscard]] NavigationAsyncBuildStatus Status() const noexcept;

	private:
		struct Permit
		{
			NavigationBudgetReservation reservation{};
			std::size_t chargedBytes = 0U;
		};
		struct Entry
		{
			// The worker writes start/finish exactly once; future readiness provides
			// the happens-before edge before the protected poll reads these values.
			struct Timing
			{
				std::chrono::steady_clock::time_point admitted{};
				std::chrono::steady_clock::time_point started{};
				std::chrono::steady_clock::time_point finished{};
			};
			NavigationAsyncBuildIdentity identity{};
			std::shared_ptr<Timing> timing;
			std::shared_ptr<Permit> permit;
			std::future<void> completion;
		};

		[[nodiscard]] std::vector<NavigationAsyncBuildCompletion> PollQueue(
			std::deque<Entry>& queue, bool retired);
		void RefreshCounts() noexcept;

		std::size_t capacity_ = 1U;
		std::deque<Entry> current_;
		std::deque<Entry> retired_;
		NavigationAsyncBuildStatus status_{};
	};
}
