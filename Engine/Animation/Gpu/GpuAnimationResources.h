// GpuAnimationResources.h - Backend-neutral lifetime controller for animation GPU work.
//
// The controller owns only logical allocation ids, byte budgets, generations, and
// completion proof. Backend code owns actual buffers and fences. Keeping that
// boundary explicit prevents worker preparation from retaining graphics-API objects or
// touching live scene state.

#pragma once

#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Core/Export.h"
#include "Renderer/RendererRecovery.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace TerraEngine::Animation {
	// A zero byte budget deliberately disables GPU animation. It is a valid
	// configuration because the caller can immediately select CPU evaluation.
	struct GpuAnimationResourceBudget {
		std::uint64_t maximumBytes = 0U;
	};

	// These outcomes are stable, backend-neutral reasons for choosing the CPU
	// path. They never expose an API object or a mutable scene reference.
	enum class GpuAnimationResourceStatus : std::uint8_t {
		GpuPrepared,
		GpuPublished,
		CpuFallbackRendererLost,
		CpuFallbackBudgetExhausted,
		CpuFallbackAllocationFailure,
		RejectedStaleCompletion,
		InvalidArgument,
		RendererGenerationRejected,
		ResourceNotFound
	};

	// A ticket is copied into a backend submission and returned with its
	// completion. All fields must match before a result becomes visible.
	struct GpuAnimationResourceTicket {
		GpuAnimationGenerationRecord generations{};
		std::uint64_t resourceId = 0U;
		std::uint64_t resourceGeneration = 0U;
		std::uint64_t allocatedBytes = 0U;

		auto operator<=>(const GpuAnimationResourceTicket&) const = default;
	};

	// Publication is one immutable value so render consumers cannot observe a
	// result whose resource identity and animation generations came from separate
	// submissions. publishedGeneration increases for every successful publish.
	struct GpuAnimationResourcePublication {
		GpuAnimationResourceTicket ticket{};
		std::uint64_t publishedGeneration = 0U;
	};

	struct GpuAnimationResourceResult {
		GpuAnimationResourceStatus status = GpuAnimationResourceStatus::InvalidArgument;
		GpuAnimationResourceTicket ticket{};

		[[nodiscard]] bool RequiresCpuFallback() const noexcept;
		[[nodiscard]] bool HasGpuTicket() const noexcept;
	};

	// A retired allocation remains charged to the bounded budget until the
	// renderer proves its last submitted frame is complete. No native handle is
	// required for this safety rule.
	struct GpuAnimationRetiredResource {
		GpuAnimationResourceTicket ticket{};
		std::uint64_t retireAfterFrameId = 0U;
	};

	// This controller is called at the renderer's protected submission and
	// completion boundaries. Worker jobs prepare immutable generation records,
	// then the boundary owner calls PrepareGpuWork/CompleteGpuWork; no live scene
	// object or component storage is reachable from this class.
	class TERRAENGINE_API GpuAnimationResourceController {
	public:
		explicit GpuAnimationResourceController(
			GpuAnimationResourceBudget budget = {},
			std::uint64_t initialRendererGeneration = 1U) noexcept;

		[[nodiscard]] GpuAnimationResourceResult PrepareGpuWork(
			const GpuAnimationGenerationRecord& generations,
			std::uint64_t requestedBytes) noexcept;
		[[nodiscard]] GpuAnimationResourceResult CompleteGpuWork(
			const GpuAnimationResourceTicket& completion) noexcept;

		// Device loss invalidates every outstanding backend allocation immediately.
		// The CPU fallback result is observable before any later submission attempt.
		void CancelForRendererLoss(const Renderer::RendererRecoverySnapshot& recovery) noexcept;

		// Recreation does not rebuild a renderer. It only accepts a newer, already
		// active renderer generation supplied by the renderer owner after recovery.
		[[nodiscard]] GpuAnimationResourceStatus BeginRecoveredRenderer(
			const Renderer::RendererRecoverySnapshot& recovery) noexcept;

		// The completion fence is supplied as a monotonically increasing frame id.
		// Retiring a missing or already-retired ticket fails closed without changing
		// allocation accounting.
		[[nodiscard]] GpuAnimationResourceStatus RetireGpuResource(
			const GpuAnimationResourceTicket& ticket,
			std::uint64_t retireAfterFrameId) noexcept;
		[[nodiscard]] std::size_t RetireCompletedResources(std::uint64_t completedFrameId) noexcept;

		// This narrow injection point lets deterministic tests exercise allocation
		// failure without depending on machine memory pressure.
		void FailNextAllocationForTesting() noexcept { failNextAllocation_ = true; }

		[[nodiscard]] GpuAnimationResourceBudget Budget() const noexcept { return budget_; }
		[[nodiscard]] std::uint64_t BytesInUse() const noexcept { return bytesInUse_; }
		[[nodiscard]] std::uint64_t RendererGeneration() const noexcept { return rendererGeneration_; }
		[[nodiscard]] std::uint64_t ResourceGeneration() const noexcept { return resourceGeneration_; }
		[[nodiscard]] std::size_t PendingSubmissionCount() const noexcept { return pending_.size(); }
		[[nodiscard]] std::size_t PendingRetirementCount() const noexcept { return retired_.size(); }
		[[nodiscard]] GpuAnimationResourceStatus LastStatus() const noexcept { return lastStatus_; }
		[[nodiscard]] bool RequiresCpuFallback() const noexcept;
		[[nodiscard]] std::shared_ptr<const GpuAnimationResourcePublication> Published() const noexcept;

	private:
		[[nodiscard]] GpuAnimationResourceResult MakeFallback(
			GpuAnimationResourceStatus status) noexcept;
		[[nodiscard]] bool HasCurrentRendererGeneration(
			const GpuAnimationGenerationRecord& generations) const noexcept;
		[[nodiscard]] bool TicketMatchesCurrentAllocation(
			const GpuAnimationResourceTicket& ticket) const noexcept;
		void ClearLostAllocations() noexcept;

		GpuAnimationResourceBudget budget_{};
		Renderer::RendererRecoverySnapshot recovery_{};
		std::uint64_t rendererGeneration_ = 1U;
		std::uint64_t resourceGeneration_ = 0U;
		std::uint64_t nextResourceId_ = 1U;
		std::uint64_t bytesInUse_ = 0U;
		std::uint64_t publishedGeneration_ = 0U;
		bool failNextAllocation_ = false;
		GpuAnimationResourceStatus lastStatus_ = GpuAnimationResourceStatus::GpuPrepared;
		std::vector<GpuAnimationResourceTicket> pending_;
		std::vector<GpuAnimationResourceTicket> allocated_;
		std::vector<GpuAnimationRetiredResource> retired_;
		std::shared_ptr<const GpuAnimationResourcePublication> published_;
	};
}
