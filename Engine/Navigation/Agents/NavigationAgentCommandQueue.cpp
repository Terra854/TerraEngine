// NavigationAgentCommandQueue.cpp - bounded copied command ownership.

#include "NavigationAgentCommandQueue.h"

#include <algorithm>

namespace TerraEngine::Navigation {
	NavigationAgentCommandQueue::NavigationAgentCommandQueue(std::size_t capacity)
		: capacity_(capacity), receiptCapacity_(std::max<std::size_t>(capacity * 2U, 1U)) {
		pending_.reserve(capacity_);
		receipts_.reserve(receiptCapacity_);
	}

	void NavigationAgentCommandQueue::RetainReceiptLocked(NavigationAgentReceipt receipt) {
		if(receipts_.size() >= receiptCapacity_) receipts_.erase(receipts_.begin());
		receipts_.push_back(receipt);
	}

	NavigationAgentReceipt NavigationAgentCommandQueue::Enqueue(NavigationAgentCommand command) {
		std::lock_guard lock(mutex_);
		NavigationAgentReceipt receipt{command.identity, NavigationAgentReceiptState::Rejected,
			NavigationOutcome::InvalidRequest, NavigationReason::None, command.correlationId, 0U};
		if(command.correlationId == 0U || !command.identity.IsValid()) {
			RetainReceiptLocked(receipt);
			return receipt;
		}
		for(const auto& old : receipts_) if(old.identity == command.identity && old.correlationId == command.correlationId) {
			// Correlation IDs identify one producer operation. Do not retain a second
			// rejected record that would hide the original queued/applied receipt.
			return receipt;
		}
		if(command.kind != NavigationAgentCommandKind::Stop
			&& !ValidateNavigationRequestedDestination(command.destination).accepted) {
			RetainReceiptLocked(receipt);
			return receipt;
		}
		if(command.kind != NavigationAgentCommandKind::Stop) {
			for(auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
				if(it->identity != command.identity) continue;
				if(it->kind == NavigationAgentCommandKind::Stop) break;
				for(auto& old : receipts_) if(old.identity == it->identity && old.correlationId == it->correlationId) {
					old.state = NavigationAgentReceiptState::Superseded;
					old.outcome = NavigationOutcome::Cancelled;
					old.reason = NavigationReason::SupersededRequest;
				}
				pending_.erase(std::next(it).base());
				++coalescedCount_;
				break;
			}
		}
		if(pending_.size() >= capacity_) {
			receipt.outcome = NavigationOutcome::RejectedCapacity;
			receipt.reason = NavigationReason::OutputLimit;
			RetainReceiptLocked(receipt);
			return receipt;
		}
		receipt.state = NavigationAgentReceiptState::Queued;
		receipt.outcome = NavigationOutcome::Queued;
		pending_.push_back(std::move(command));
		RetainReceiptLocked(receipt);
		return receipt;
	}

	std::vector<NavigationAgentCommand> NavigationAgentCommandQueue::Drain() {
		std::lock_guard lock(mutex_);
		auto commands = std::move(pending_);
		pending_.clear();
		pending_.reserve(capacity_);
		return commands;
	}

	std::optional<NavigationAgentReceipt> NavigationAgentCommandQueue::ReadReceipt(NavigationAgentIdentity identity, std::uint64_t correlationId) const {
		std::lock_guard lock(mutex_);
		for(auto it = receipts_.rbegin(); it != receipts_.rend(); ++it)
			if(it->identity == identity && it->correlationId == correlationId) return *it;
		if(identity.IsValid() && correlationId != 0U)
			// Receipt retention expiry is distinct from a stale owner identity. Keep
			// the copied native record consistent with managed and editor expiry reads.
			return NavigationAgentReceipt{identity, NavigationAgentReceiptState::Expired, NavigationOutcome::Expired, NavigationReason::Expired, correlationId, 0U};
		return {};
	}

	void NavigationAgentCommandQueue::CompleteReceipt(NavigationAgentReceipt completed) {
		std::lock_guard lock(mutex_);
		for(auto& receipt : receipts_) if(receipt.identity == completed.identity && receipt.correlationId == completed.correlationId) {
			receipt = completed;
			return;
		}
		RetainReceiptLocked(std::move(completed));
	}

	std::size_t NavigationAgentCommandQueue::ExpireReceipts(std::size_t limit) {
		std::lock_guard lock(mutex_);
		const auto count = std::min(limit, receipts_.size());
		receipts_.erase(receipts_.begin(), receipts_.begin() + static_cast<std::ptrdiff_t>(count));
		return count;
	}

	void NavigationAgentCommandQueue::Clear() {
		std::lock_guard lock(mutex_);
		for(const auto& command : pending_) for(auto& receipt : receipts_)
			if(receipt.identity == command.identity && receipt.correlationId == command.correlationId) {
				receipt.state = NavigationAgentReceiptState::Superseded;
				receipt.outcome = NavigationOutcome::Cancelled;
				receipt.reason = NavigationReason::SupersededRequest;
			}
		pending_.clear();
	}

	std::size_t NavigationAgentCommandQueue::PendingCount() const { std::lock_guard lock(mutex_); return pending_.size(); }
	std::size_t NavigationAgentCommandQueue::PendingRouteDemandCount() const {
		std::lock_guard lock(mutex_);
		return static_cast<std::size_t>(std::count_if(pending_.begin(), pending_.end(),
			[](const NavigationAgentCommand& command) {
				return command.kind == NavigationAgentCommandKind::SetLocationDestination
					|| command.kind == NavigationAgentCommandKind::SetObjectDestination;
			}));
	}
	std::size_t NavigationAgentCommandQueue::ReceiptCount() const { std::lock_guard lock(mutex_); return receipts_.size(); }
	std::size_t NavigationAgentCommandQueue::MemoryBytes() const { std::lock_guard lock(mutex_); return pending_.capacity() * sizeof(NavigationAgentCommand) + receipts_.capacity() * sizeof(NavigationAgentReceipt); }
	std::uint64_t NavigationAgentCommandQueue::CoalescedCount() const { std::lock_guard lock(mutex_); return coalescedCount_; }
}
