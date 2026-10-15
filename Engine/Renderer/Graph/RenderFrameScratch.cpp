// RenderFrameScratch.cpp - Protected reset and worker-safe episode retirement.
#include "RenderFrameScratch.h"

#include <atomic>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace TerraEngine::Renderer {
	namespace {
		// Only owner creation touches this process-wide identity source. Resource
		// allocation and frame preparation keep their normal typed, local path.
		std::atomic<std::uint64_t> nextLifetimeId{1U};
		std::uint64_t AcquireLifetimeId()
		{
			auto candidate = nextLifetimeId.load(std::memory_order_relaxed);
			for(;;) {
				if(candidate == std::numeric_limits<std::uint64_t>::max())
					throw std::overflow_error("Renderer scratch lifetime identities exhausted");
				if(nextLifetimeId.compare_exchange_weak(candidate, candidate + 1U,
					std::memory_order_relaxed, std::memory_order_relaxed)) return candidate;
			}
		}
	}

	RenderFrameScratch::RenderFrameScratch(std::size_t capacityBytes, std::pmr::memory_resource* upstream)
		: resource_(capacityBytes, upstream), lifetimeId_(AcquireLifetimeId()) {}
	std::shared_ptr<RenderFrameScratch> RenderFrameScratch::Create(std::size_t capacityBytes, std::pmr::memory_resource* upstream)
	{
		if(upstream == nullptr) throw std::invalid_argument("Renderer scratch requires an upstream resource");
		return std::shared_ptr<RenderFrameScratch>(new RenderFrameScratch(capacityBytes, upstream));
	}
	RenderFrameScratch::Lease::Lease(std::shared_ptr<RenderFrameScratch> owner) noexcept
		: owner_(std::move(owner)), uncaughtAtStart_(std::uncaught_exceptions()) {}
	RenderFrameScratch::Lease::Lease(Lease&& other) noexcept
		: owner_(std::move(other.owner_)), uncaughtAtStart_(other.uncaughtAtStart_), outcome_(other.outcome_) {}
	RenderFrameScratch::Lease& RenderFrameScratch::Lease::operator=(Lease&& other) noexcept
	{
		if(this != &other) {
			Release();
			owner_ = std::move(other.owner_);
			uncaughtAtStart_ = other.uncaughtAtStart_;
			outcome_ = other.outcome_;
		}
		return *this;
	}
	RenderFrameScratch::Lease::~Lease() noexcept { Release(); }
	std::pmr::memory_resource* RenderFrameScratch::Lease::Resource() const
	{
		if(!owner_) throw std::logic_error("An empty renderer scratch lease has no resource");
		return &owner_->resource_;
	}
	void RenderFrameScratch::Lease::MarkCompleted() noexcept { outcome_ = RenderFrameScratchOutcome::Completed; }
	void RenderFrameScratch::Lease::MarkFailed() noexcept { outcome_ = RenderFrameScratchOutcome::Failed; }
	void RenderFrameScratch::Lease::Release() noexcept
	{
		if(!owner_) return;
		// Retain the owner through publication, then drop the last strong reference
		// after the admission lock is released. This may run on a worker unwind;
		// the episode release itself never resets storage. Lifecycle code keeps
		// its owner until workers retire, so final arena destruction stays protected.
		auto owner = std::move(owner_);
		const auto outcome = std::uncaught_exceptions() > uncaughtAtStart_
			? RenderFrameScratchOutcome::Failed : outcome_;
		owner->ReleaseEpisode(outcome);
	}
	RenderFrameScratch::Lease RenderFrameScratch::BeginEpisode(std::uint64_t frameId)
	{
		// Acquire the strong owner before changing state so a failed ownership
		// acquisition cannot leave an episode active without a returned lease.
		auto owner = shared_from_this();
		std::lock_guard lock(mutex_);
		if(active_) throw std::logic_error("Renderer scratch episode is already active");
		if(episodeId_ == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Renderer scratch episode identities exhausted");
		// The previous vectors and borrowers have retired before this protected
		// entry. The terminal snapshot is preserved independently in last_.
		resource_.Reset();
		const auto& statistics = resource_.Statistics();
		reuseBaseline_ = statistics.reuseAllocationCount;
		fallbackBaseline_ = statistics.fallbackAllocationCount;
		++episodeId_;
		frameId_ = frameId;
		active_ = true;
		return Lease(std::move(owner));
	}
	void RenderFrameScratch::Retire()
	{
		std::lock_guard lock(mutex_);
		if(active_) throw std::logic_error("Retire renderer scratch only after its episode settles");
		// Lifecycle teardown is another protected reclamation boundary. Never
		// replace the saved failed/cancelled/completed episode with zeroed use.
		resource_.Reset();
	}
	bool RenderFrameScratch::IsActive() const
	{
		std::lock_guard lock(mutex_);
		return active_;
	}
	RenderFrameScratchDiagnostics RenderFrameScratch::Snapshot() const
	{
		std::lock_guard lock(mutex_);
		return active_ ? CurrentSnapshot(RenderFrameScratchOutcome::Active)
			: episodeId_ == 0U ? CurrentSnapshot(RenderFrameScratchOutcome::None) : last_;
	}
	RenderFrameScratchDiagnostics RenderFrameScratch::LastSnapshot() const
	{
		std::lock_guard lock(mutex_);
		return last_;
	}
	RenderFrameScratchReclamationDiagnostics RenderFrameScratch::ReclamationSnapshot() const
	{
		// The protected caller has already quiesced allocation; the lock keeps
		// lifetime transitions coherent while copying actual resource counters.
		std::lock_guard lock(mutex_);
		const auto& statistics = resource_.Statistics();
		RenderFrameScratchReclamationDiagnostics result;
		result.lifetimeId = lifetimeId_;
		result.totalFallbackAllocationCount = statistics.fallbackAllocationCount;
		result.totalFallbackDeallocationCount = statistics.fallbackDeallocationCount;
		result.outstandingFallbackAllocationCount = statistics.fallbackOutstandingCount;
		result.outstandingFallbackBytes = statistics.fallbackOutstandingBytes;
		result.resetCount = statistics.resetCount;
		return result;
	}
	void RenderFrameScratch::ReleaseEpisode(RenderFrameScratchOutcome outcome) noexcept
	{
		std::lock_guard lock(mutex_);
		last_ = CurrentSnapshot(outcome);
		active_ = false;
	}
	RenderFrameScratchDiagnostics RenderFrameScratch::CurrentSnapshot(RenderFrameScratchOutcome outcome) const noexcept
	{
		const auto& statistics = resource_.Statistics();
		RenderFrameScratchDiagnostics result;
		result.lifetimeId = lifetimeId_;
		result.episodeId = episodeId_;
		result.frameId = frameId_;
		result.capacityBytes = statistics.capacityBytes;
		result.usedBytes = statistics.usedBytes;
		// Arena use is monotonic within one episode; individual deallocations do
		// not release backing. Its final use therefore equals its episode peak.
		result.peakBytes = statistics.usedBytes;
		result.lifetimeHighWaterBytes = statistics.highWaterBytes;
		result.reuseAllocationCount = statistics.reuseAllocationCount - reuseBaseline_;
		result.fallbackAllocationCount = statistics.fallbackAllocationCount - fallbackBaseline_;
		result.resetCount = statistics.resetCount;
		result.outcome = outcome;
		return result;
	}
}
