#pragma once

#include "NavigationAgentTypes.h"
#include <mutex>
#include <optional>
#include <vector>

namespace TerraEngine::Navigation {
	class TERRAENGINE_API NavigationAgentCommandQueue final {
	public:
		explicit NavigationAgentCommandQueue(std::size_t capacity);
		NavigationAgentReceipt Enqueue(NavigationAgentCommand command);
		std::vector<NavigationAgentCommand> Drain();
		std::optional<NavigationAgentReceipt> ReadReceipt(NavigationAgentIdentity identity, std::uint64_t correlationId) const;
		void CompleteReceipt(NavigationAgentReceipt receipt);
		std::size_t ExpireReceipts(std::size_t limit);
		void Clear();
		[[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }
		[[nodiscard]] std::size_t PendingCount() const;
		// Stops deliberately do not create a path-search request. The early command
		// scheduler uses this count only to reserve declaration liveness for actual
		// destination commands; protected admission still owns every real request.
		[[nodiscard]] std::size_t PendingRouteDemandCount() const;
		[[nodiscard]] std::size_t ReceiptCount() const;
		[[nodiscard]] std::size_t MemoryBytes() const;
		// This monotonic count lets diagnostics expose command replacement pressure
		// without copying receipts or inspecting producer-owned command data.
		[[nodiscard]] std::uint64_t CoalescedCount() const;

	private:
		void RetainReceiptLocked(NavigationAgentReceipt receipt);
		std::size_t capacity_;
		std::size_t receiptCapacity_;
		mutable std::mutex mutex_;
		std::vector<NavigationAgentCommand> pending_;
		std::vector<NavigationAgentReceipt> receipts_;
		std::uint64_t coalescedCount_ = 0U;
	};
}
