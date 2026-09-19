// NavigationAsyncPreparation.cpp - immutable ThreadPool task coordination.

#include "Navigation/NavigationAsyncPreparation.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace TerraEngine::Navigation
{
	NavigationAsyncBuildCoordinator::NavigationAsyncBuildCoordinator(std::size_t capacity)
		: capacity_(std::max<std::size_t>(1U, capacity))
	{
		status_.capacity = capacity_;
	}

	bool NavigationAsyncBuildCoordinator::TrySubmit(NavigationAsyncBuildIdentity identity,
		std::shared_ptr<NavigationBudget> budget, std::size_t chargedBytes,
		WorkFunction work, std::string& rejectionReason)
	{
		std::vector<Submission> submissions;
		submissions.push_back({identity, std::move(budget), chargedBytes, std::move(work)});
		return TrySubmitBatch(std::move(submissions), rejectionReason);
	}

	bool NavigationAsyncBuildCoordinator::TrySubmitBatch(std::vector<Submission> submissions,
		std::string& rejectionReason)
	{
		if(submissions.empty() || std::any_of(submissions.begin(), submissions.end(),
			[](const Submission& submission) { return !submission.budget || !submission.work; })) {
			rejectionReason = "NavigationAsyncBuildInvalidSubmission";
			++status_.admissionRejectedCount;
			return false;
		}
		const std::size_t occupied = current_.size() + retired_.size();
		if(occupied >= capacity_ || submissions.size() > capacity_ - occupied) {
			rejectionReason = "NavigationAsyncBuildCapacity";
			++status_.admissionRejectedCount;
			return false;
		}

		// These detached entries own all provisional reservations. If a later child
		// exceeds the budget, destruction rolls back the entire wave before dispatch.
		std::vector<Entry> entries;
		entries.reserve(submissions.size());
		for(const Submission& submission : submissions) {
			NavigationBudgetReservation reservation = submission.budget->TryReserve(
				NavigationBudgetCategory::BuildScratch, submission.chargedBytes, "navigation asynchronous build packet");
			if(!reservation.accepted) {
				rejectionReason = reservation.reason.empty() ? "NavigationAsyncBuildBudget" : reservation.reason;
				++status_.admissionRejectedCount;
				return false;
			}
			Entry entry{};
			entry.identity = submission.identity;
			entry.permit = std::make_shared<Permit>();
			entry.permit->reservation = std::move(reservation);
			entry.permit->chargedBytes = submission.chargedBytes;
			entry.timing = std::make_shared<Entry::Timing>();
			entry.timing->admitted = std::chrono::steady_clock::now();
			entries.push_back(std::move(entry));
		}

		std::vector<std::shared_ptr<Task<void>>> tasks;
		tasks.reserve(entries.size());
		for(std::size_t index = 0U; index < entries.size(); ++index) {
			Entry& entry = entries[index];
			// Unlike recycled void tasks, a detached task destroys its promise and
			// captures immediately if dispatch fails. This keeps batch rollback bounded.
			auto task = std::make_shared<Task<void>>([permitForWorker = entry.permit,
				timing = entry.timing, work = std::move(submissions[index].work)]() mutable {
				timing->started = std::chrono::steady_clock::now();
				// Future readiness synchronizes timing and result reads at protected poll.
				struct FinishTiming
				{
					Entry::Timing& value;
					~FinishTiming() { value.finished = std::chrono::steady_clock::now(); }
				} finishTiming{*timing};
				// Release copied input before the promise completes, including exceptions.
				[[maybe_unused]] auto ownedPermit = std::move(permitForWorker);
				auto ownedWork = std::move(work);
				ownedWork();
			});
			entry.completion = task->getFuture();
			tasks.push_back(std::move(task));
		}
		const std::size_t previousCount = current_.size();
		try {
			for(Entry& entry : entries) current_.push_back(std::move(entry));
		}
		catch(...) {
			// No task has been dispatched: restore ownership before propagating an
			// allocation failure instead of leaving futures that can never execute.
			while(current_.size() > previousCount) current_.pop_back();
			throw;
		}
		status_.admittedCount += tasks.size();
		RefreshCounts();
		for(const auto& task : tasks) {
			try { ThreadPool::instance().enqueue(task, 1U); }
			catch(...) {
				// Admission already succeeded. Keep all entries owned: queued children
				// finish normally; destruction of unqueued tasks makes their futures
				// report broken-promise failures. The caller can reject the whole result
				// group without losing permits or publishing an untracked partial wave.
				break;
			}
		}
		rejectionReason.clear();
		return true;
	}

	void NavigationAsyncBuildCoordinator::RetireCurrent()
	{
		while(!current_.empty()) {
			retired_.push_back(std::move(current_.front()));
			current_.pop_front();
		}
		RefreshCounts();
	}

	std::vector<NavigationAsyncBuildCompletion> NavigationAsyncBuildCoordinator::PollQueue(
		std::deque<Entry>& queue, bool retired)
	{
		std::vector<NavigationAsyncBuildCompletion> completions;
		for(auto entry = queue.begin(); entry != queue.end();) {
			if(entry->completion.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
				++entry;
				continue;
			}
			NavigationAsyncBuildCompletion completion{};
			completion.identity = entry->identity;
			completion.retired = retired;
			try {
				entry->completion.get();
				completion.succeeded = true;
			}
			catch(const std::exception& exception) {
				completion.failureDetail = exception.what();
			}
			catch(...) {
				completion.failureDetail = "NavigationAsyncBuildUnhandledException";
			}
			const auto observed = std::chrono::steady_clock::now();
			const auto nanos = [](auto duration) {
				return static_cast<std::uint64_t>(std::max<std::int64_t>(0,
					std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
			};
			const bool executed = entry->timing->finished != std::chrono::steady_clock::time_point{};
			const std::uint64_t queueWait = executed ? nanos(entry->timing->started - entry->timing->admitted) : 0U;
			const std::uint64_t workerTime = executed ? nanos(entry->timing->finished - entry->timing->started) : 0U;
			const std::uint64_t publicationWait = executed ? nanos(observed - entry->timing->finished) : 0U;
			if(entry->identity.dynamicRebuild) ++status_.dynamicCompletedCount;
			else ++status_.groundCompletedCount;
			status_.totalQueueWaitNanoseconds += queueWait;
			status_.maxQueueWaitNanoseconds = std::max(status_.maxQueueWaitNanoseconds, queueWait);
			status_.totalWorkerNanoseconds += workerTime;
			status_.maxWorkerNanoseconds = std::max(status_.maxWorkerNanoseconds, workerTime);
			status_.totalPublicationWaitNanoseconds += publicationWait;
			status_.maxPublicationWaitNanoseconds = std::max(status_.maxPublicationWaitNanoseconds, publicationWait);
			completions.push_back(std::move(completion));
			entry = queue.erase(entry);
		}
		return completions;
	}

	std::vector<NavigationAsyncBuildCompletion> NavigationAsyncBuildCoordinator::PollCompleted()
	{
		std::vector<NavigationAsyncBuildCompletion> completions = PollQueue(current_, false);
		std::vector<NavigationAsyncBuildCompletion> retiredCompletions = PollQueue(retired_, true);
		completions.insert(completions.end(), std::make_move_iterator(retiredCompletions.begin()),
			std::make_move_iterator(retiredCompletions.end()));
		for(const NavigationAsyncBuildCompletion& completion : completions) {
			++status_.completedCount;
			if(completion.retired) ++status_.staleDiscardCount;
		}
		RefreshCounts();
		return completions;
	}

	NavigationAsyncBuildStatus NavigationAsyncBuildCoordinator::Status() const noexcept
	{
		return status_;
	}

	void NavigationAsyncBuildCoordinator::RefreshCounts() noexcept
	{
		status_.currentCount = current_.size();
		status_.retiredCount = retired_.size();
		status_.chargedBytes = 0U;
		for(const Entry& entry : current_) status_.chargedBytes += entry.permit->chargedBytes;
		for(const Entry& entry : retired_) status_.chargedBytes += entry.permit->chargedBytes;
		status_.peakChargedBytes = std::max(status_.peakChargedBytes, status_.chargedBytes);
	}
}
