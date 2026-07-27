// GpuAnimationResources.cpp - Generation-safe, backend-neutral GPU allocation control.

#include "Animation/Gpu/GpuAnimationResources.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool IsCpuFallbackStatus(GpuAnimationResourceStatus status) noexcept
		{
			return status == GpuAnimationResourceStatus::CpuFallbackRendererLost
				|| status == GpuAnimationResourceStatus::CpuFallbackBudgetExhausted
				|| status == GpuAnimationResourceStatus::CpuFallbackAllocationFailure;
		}

		[[nodiscard]] bool SameTicket(
			const GpuAnimationResourceTicket& left,
			const GpuAnimationResourceTicket& right) noexcept
		{
			return left == right;
		}
	}

	bool GpuAnimationResourceResult::RequiresCpuFallback() const noexcept
	{
		return IsCpuFallbackStatus(status);
	}

	bool GpuAnimationResourceResult::HasGpuTicket() const noexcept
	{
		return status == GpuAnimationResourceStatus::GpuPrepared && ticket.resourceId != 0U;
	}

	GpuAnimationResourceController::GpuAnimationResourceController(
		GpuAnimationResourceBudget budget,
		std::uint64_t initialRendererGeneration) noexcept
		: budget_(budget)
		, recovery_(Renderer::MakeActiveRecoverySnapshot(initialRendererGeneration))
		, rendererGeneration_(recovery_.renderSessionGeneration)
	{
	}

	GpuAnimationResourceResult GpuAnimationResourceController::PrepareGpuWork(
		const GpuAnimationGenerationRecord& generations,
		std::uint64_t requestedBytes) noexcept
	{
		if(!Renderer::CanSubmitNormalGpuWork(recovery_)) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackRendererLost);
		}
		if(!IsGpuAnimationGenerationRecordValid(generations) || requestedBytes == 0U) {
			lastStatus_ = GpuAnimationResourceStatus::InvalidArgument;
			return {lastStatus_, {}};
		}
		if(!HasCurrentRendererGeneration(generations)) {
			lastStatus_ = GpuAnimationResourceStatus::RejectedStaleCompletion;
			return {lastStatus_, {}};
		}
		if(failNextAllocation_) {
			failNextAllocation_ = false;
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		}
		// Subtraction avoids an overflowing addition when callers provide an
		// untrusted byte count from a worker-prepared packet.
		if(bytesInUse_ > budget_.maximumBytes
			|| requestedBytes > budget_.maximumBytes - bytesInUse_) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackBudgetExhausted);
		}
		if(resourceGeneration_ == std::numeric_limits<std::uint64_t>::max()
			|| nextResourceId_ == 0U) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		}

		try {
			GpuAnimationResourceTicket ticket;
			ticket.generations = generations;
			ticket.resourceId = nextResourceId_++;
			ticket.resourceGeneration = ++resourceGeneration_;
			ticket.allocatedBytes = requestedBytes;
			// Reserve both containers before changing logical state. The two later
			// appends therefore cannot leave a pending ticket without its allocation.
			pending_.reserve(pending_.size() + 1U);
			allocated_.reserve(allocated_.size() + 1U);
			pending_.push_back(ticket);
			allocated_.push_back(ticket);
			bytesInUse_ += requestedBytes;
			lastStatus_ = GpuAnimationResourceStatus::GpuPrepared;
			return {lastStatus_, ticket};
		} catch(const std::bad_alloc&) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		} catch(...) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		}
	}

	GpuAnimationResourceResult GpuAnimationResourceController::CompleteGpuWork(
		const GpuAnimationResourceTicket& completion) noexcept
	{
		if(!Renderer::CanSubmitNormalGpuWork(recovery_)
			|| !HasCurrentRendererGeneration(completion.generations)
			|| !TicketMatchesCurrentAllocation(completion)) {
			lastStatus_ = GpuAnimationResourceStatus::RejectedStaleCompletion;
			return {lastStatus_, {}};
		}

		const auto pending = std::find_if(pending_.begin(), pending_.end(), [&](const GpuAnimationResourceTicket& ticket) {
			return SameTicket(ticket, completion);
		});
		if(pending == pending_.end()) {
			lastStatus_ = GpuAnimationResourceStatus::RejectedStaleCompletion;
			return {lastStatus_, {}};
		}

		try {
			// Construct before changing state: allocation failure cannot remove a
			// pending submission or expose a partial generation to readers.
			auto publication = std::make_shared<GpuAnimationResourcePublication>();
			publication->ticket = completion;
			publication->publishedGeneration = ++publishedGeneration_;
			std::atomic_store_explicit(&published_, std::shared_ptr<const GpuAnimationResourcePublication>(publication), std::memory_order_release);
			pending_.erase(pending);
			lastStatus_ = GpuAnimationResourceStatus::GpuPublished;
			return {lastStatus_, completion};
		} catch(const std::bad_alloc&) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		} catch(...) {
			return MakeFallback(GpuAnimationResourceStatus::CpuFallbackAllocationFailure);
		}
	}

	void GpuAnimationResourceController::CancelForRendererLoss(
		const Renderer::RendererRecoverySnapshot& recovery) noexcept
	{
		// A terminal renderer loss has no trustworthy completion fence. The backend
		// destroys its allocations as part of loss handling, so retaining their byte
		// charge or waiting for a fence would deadlock future CPU-only frames.
		if(Renderer::CanSubmitNormalGpuWork(recovery)) return;
		recovery_ = recovery;
		ClearLostAllocations();
		std::atomic_store_explicit(&published_, std::shared_ptr<const GpuAnimationResourcePublication>{}, std::memory_order_release);
		lastStatus_ = GpuAnimationResourceStatus::CpuFallbackRendererLost;
	}

	GpuAnimationResourceStatus GpuAnimationResourceController::BeginRecoveredRenderer(
		const Renderer::RendererRecoverySnapshot& recovery) noexcept
	{
		if(!Renderer::CanAcceptRecoveredGpuResources(recovery_, recovery)) {
			lastStatus_ = GpuAnimationResourceStatus::RendererGenerationRejected;
			return lastStatus_;
		}
		if(recovery.renderSessionGeneration > std::numeric_limits<std::uint32_t>::max()) {
			// GpuAnimationGenerationRecord is a frozen uint32 shader ABI. Reject an
			// unrepresentable renderer generation instead of truncating it.
			lastStatus_ = GpuAnimationResourceStatus::RendererGenerationRejected;
			return lastStatus_;
		}
		// The renderer supplied a newer active generation after it performed its
		// own recreation. All prior animation resource ids remain retired forever.
		recovery_ = recovery;
		rendererGeneration_ = recovery.renderSessionGeneration;
		ClearLostAllocations();
		std::atomic_store_explicit(&published_, std::shared_ptr<const GpuAnimationResourcePublication>{}, std::memory_order_release);
		lastStatus_ = GpuAnimationResourceStatus::GpuPrepared;
		return lastStatus_;
	}

	GpuAnimationResourceStatus GpuAnimationResourceController::RetireGpuResource(
		const GpuAnimationResourceTicket& ticket,
		std::uint64_t retireAfterFrameId) noexcept
	{
		const auto allocation = std::find_if(allocated_.begin(), allocated_.end(), [&](const GpuAnimationResourceTicket& candidate) {
			return SameTicket(candidate, ticket);
		});
		if(allocation == allocated_.end()) {
			lastStatus_ = GpuAnimationResourceStatus::ResourceNotFound;
			return lastStatus_;
		}
		try {
			retired_.push_back({*allocation, retireAfterFrameId});
			pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const GpuAnimationResourceTicket& pending) {
				return SameTicket(pending, ticket);
			}), pending_.end());
			allocated_.erase(allocation);
			lastStatus_ = GpuAnimationResourceStatus::GpuPrepared;
			return lastStatus_;
		} catch(const std::bad_alloc&) {
			lastStatus_ = GpuAnimationResourceStatus::CpuFallbackAllocationFailure;
			return lastStatus_;
		} catch(...) {
			lastStatus_ = GpuAnimationResourceStatus::CpuFallbackAllocationFailure;
			return lastStatus_;
		}
	}

	std::size_t GpuAnimationResourceController::RetireCompletedResources(std::uint64_t completedFrameId) noexcept
	{
		std::size_t retiredCount = 0U;
		const auto end = std::remove_if(retired_.begin(), retired_.end(), [&](const GpuAnimationRetiredResource& retired) {
			if(retired.retireAfterFrameId > completedFrameId) return false;
			bytesInUse_ -= retired.ticket.allocatedBytes;
			++retiredCount;
			return true;
		});
		retired_.erase(end, retired_.end());
		return retiredCount;
	}

	bool GpuAnimationResourceController::RequiresCpuFallback() const noexcept
	{
		return IsCpuFallbackStatus(lastStatus_);
	}

	std::shared_ptr<const GpuAnimationResourcePublication> GpuAnimationResourceController::Published() const noexcept
	{
		return std::atomic_load_explicit(&published_, std::memory_order_acquire);
	}

	GpuAnimationResourceResult GpuAnimationResourceController::MakeFallback(
		GpuAnimationResourceStatus status) noexcept
	{
		lastStatus_ = status;
		return {status, {}};
	}

	bool GpuAnimationResourceController::HasCurrentRendererGeneration(
		const GpuAnimationGenerationRecord& generations) const noexcept
	{
		return generations.rendererGeneration == rendererGeneration_
			&& Renderer::IsRendererGenerationCurrent(recovery_, rendererGeneration_);
	}

	bool GpuAnimationResourceController::TicketMatchesCurrentAllocation(
		const GpuAnimationResourceTicket& ticket) const noexcept
	{
		return std::any_of(allocated_.begin(), allocated_.end(), [&](const GpuAnimationResourceTicket& candidate) {
			return SameTicket(candidate, ticket);
		});
	}

	void GpuAnimationResourceController::ClearLostAllocations() noexcept
	{
		pending_.clear();
		allocated_.clear();
		retired_.clear();
		bytesInUse_ = 0U;
	}
}
