// RenderFrameScratch.h - Renderer-owned storage and exclusive frame episodes.
#pragma once

#include "Core/Export.h"
#include "Core/Memory/ReusableArenaResource.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace TerraEngine::Renderer {
	// Copied facts have no arena pointers and can outlive either episode or owner.
	enum class RenderFrameScratchOutcome : std::uint8_t { None, Active, Completed, Failed, Cancelled };
	struct RenderFrameScratchDiagnostics {
		std::uint64_t lifetimeId = 0U;
		std::uint64_t episodeId = 0U;
		std::uint64_t frameId = 0U;
		std::size_t capacityBytes = 0U;
		std::size_t usedBytes = 0U;
		std::size_t peakBytes = 0U;
		std::size_t lifetimeHighWaterBytes = 0U;
		std::size_t reuseAllocationCount = 0U;
		std::size_t fallbackAllocationCount = 0U;
		std::size_t resetCount = 0U;
		RenderFrameScratchOutcome outcome = RenderFrameScratchOutcome::None;
	};

	// Current lifetime reclamation facts are separate from a terminal episode's
	// pre-reset pressure copy. This typed value contains no resource pointers.
	struct RenderFrameScratchReclamationDiagnostics {
		std::uint64_t lifetimeId = 0U;
		std::size_t totalFallbackAllocationCount = 0U;
		std::size_t totalFallbackDeallocationCount = 0U;
		std::size_t outstandingFallbackAllocationCount = 0U;
		std::size_t outstandingFallbackBytes = 0U;
		std::size_t resetCount = 0U;
	};

	// One protected coordinator allocates. Workers may borrow completed vectors;
	// their jobs must settle before vectors and then the lease are destroyed.
	// Lifecycle code retains its strong owner until those continuations retire:
	// the final owner destroys the arena, so releasing ownership on an active
	// worker is not a substitute for protected lifecycle reclamation.
	class TERRAENGINE_API RenderFrameScratch final : public std::enable_shared_from_this<RenderFrameScratch> {
	public:
		static constexpr std::size_t kDefaultCapacityBytes = 16U * 1024U * 1024U;
		class TERRAENGINE_API Lease final {
		public:
			Lease() = default;
			Lease(const Lease&) = delete;
			Lease& operator=(const Lease&) = delete;
			Lease(Lease&& other) noexcept;
			Lease& operator=(Lease&& other) noexcept;
			~Lease() noexcept;
			[[nodiscard]] std::pmr::memory_resource* Resource() const;
			void MarkCompleted() noexcept;
			void MarkFailed() noexcept;
		private:
			friend class RenderFrameScratch;
			explicit Lease(std::shared_ptr<RenderFrameScratch> owner) noexcept;
			void Release() noexcept;
			std::shared_ptr<RenderFrameScratch> owner_;
			int uncaughtAtStart_ = 0;
			RenderFrameScratchOutcome outcome_ = RenderFrameScratchOutcome::Cancelled;
		};

		// A custom upstream must outlive the owner and every strong episode lease.
		[[nodiscard]] static std::shared_ptr<RenderFrameScratch> Create(
			std::size_t capacityBytes = kDefaultCapacityBytes,
			std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());
		RenderFrameScratch(const RenderFrameScratch&) = delete;
		RenderFrameScratch& operator=(const RenderFrameScratch&) = delete;
		// Begin/Retire are protected-boundary operations, after previous borrowers
		// settle. Snapshot observes active statistics only at a quiescent boundary.
		[[nodiscard]] Lease BeginEpisode(std::uint64_t frameId);
		void Retire();
		[[nodiscard]] bool IsActive() const;
		[[nodiscard]] RenderFrameScratchDiagnostics Snapshot() const;
		// The terminal copy never reads active resource counters; it remains safe
		// to observe while a later episode allocates on its protected coordinator.
		[[nodiscard]] RenderFrameScratchDiagnostics LastSnapshot() const;
		// Protected/quiescent boundary only: no concurrent resource allocation or
		// reset may run. The lifecycle mutex alone does not synchronize allocation.
		[[nodiscard]] RenderFrameScratchReclamationDiagnostics ReclamationSnapshot() const;
	private:
		explicit RenderFrameScratch(std::size_t capacityBytes, std::pmr::memory_resource* upstream);
		void ReleaseEpisode(RenderFrameScratchOutcome outcome) noexcept;
		[[nodiscard]] RenderFrameScratchDiagnostics CurrentSnapshot(RenderFrameScratchOutcome outcome) const noexcept;
		// The admission lock never surrounds per-allocation calls. Resource access
		// itself remains thread-confined to the declared protected capture phase.
		mutable std::mutex mutex_;
		Memory::ReusableArenaResource resource_;
		RenderFrameScratchDiagnostics last_{};
		std::uint64_t lifetimeId_ = 0U;
		std::uint64_t episodeId_ = 0U;
		std::uint64_t frameId_ = 0U;
		std::size_t reuseBaseline_ = 0U;
		std::size_t fallbackBaseline_ = 0U;
		bool active_ = false;
	};
}
