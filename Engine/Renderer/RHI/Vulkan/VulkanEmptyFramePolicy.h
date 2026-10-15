#pragma once

// CPU policy shared by the native backend and its tests. Native handles remain
// in Impl: this helper only proves ownership and operation ordering.
#include "VulkanPresentationLifecycle.h"
#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <utility>
#include <type_traits>

namespace TerraEngine::Renderer::Vulkan
{
	struct EmptyFrameResourceCounts
	{
		std::size_t frameFences = 0, acquireSemaphores = 0, presentSemaphores = 0;
	};

	[[nodiscard]] inline bool ValidateEmptyFrameAcquisition(const PresentationLifecycle& lifecycle,
		const AcquiredPresentationImage& image, EmptyFrameResourceCounts counts) noexcept
	{
		return lifecycle.Validate(image) && image.frameSlot < counts.frameFences
			&& image.frameSlot < counts.acquireSemaphores && image.imageIndex < counts.presentSemaphores;
	}
	[[nodiscard]] constexpr bool EmptyFrameNeedsAcquireWait(bool consumed) noexcept { return !consumed; }
	struct EmptyFrameColorPassPlan
	{
		std::uint32_t colorAttachmentCount = 1, samples = 1;
		bool clear = true, store = true, discardInitialContents = true, presentFinalLayout = true;
		bool hasDepth = false, needsTransferUsage = false;
		std::array<float, 4> clearColor{0.0f, 0.0f, 0.0f, 1.0f};
	};
	[[nodiscard]] constexpr EmptyFrameColorPassPlan MakeEmptyFrameColorPassPlan() noexcept { return {}; }
	struct EmptyFramePublicationPlan
	{
		bool discardUnsubmittedShadow = true, discardUnsubmittedHiZ = true;
		bool publishScene = false, publishShadow = false, publishHiZ = false;
		bool preserveSubmittedPrefix = false;
	};
	[[nodiscard]] constexpr EmptyFramePublicationPlan MakeEmptyFramePublicationPlan(bool submittedPrefix) noexcept
	{
		EmptyFramePublicationPlan plan;
		plan.preserveSubmittedPrefix = submittedPrefix;
		return plan;
	}

	// Keep only copied metadata for a genuinely submitted pyramid. A later
	// unsubmitted recording may change CPU metadata, but cannot revoke this proof.
	template<class Metadata>
	struct EmptyFrameSubmittedHiZState
	{
		static_assert(std::is_trivially_copyable_v<Metadata>, "Submitted Hi-Z receipts must never allocate after native submission.");
		bool pending = false;
		Metadata history{};
		void Confirm(const Metadata& submitted) noexcept { history = submitted; pending = true; }
		bool DiscardUnsubmitted(bool preservePrefix, Metadata& recordedHistory) noexcept
		{
			if (preservePrefix && pending) { recordedHistory = history; return true; }
			pending = false;
			return false;
		}
		void Reset() noexcept { pending = false; history = {}; }
	};

	// Reserve retirement storage while active owners are still installed. Native
	// handoff then commits into this slot without allocating. An unused reservation
	// is removed when candidate creation restores the previous active bundle.
	template<class Queue>
	class EmptyFrameRetirementReservation
	{
	public:
		explicit EmptyFrameRetirementReservation(Queue& queue) : queue_(queue) { queue_.emplace_back(); }
		EmptyFrameRetirementReservation(const EmptyFrameRetirementReservation&) = delete;
		EmptyFrameRetirementReservation& operator=(const EmptyFrameRetirementReservation&) = delete;
		~EmptyFrameRetirementReservation() { if (!committed_) queue_.pop_back(); }
		template<class Bundle>
		void Commit(Bundle& bundle, std::uint64_t generation) noexcept
		{
			static_assert(std::is_nothrow_move_assignable_v<Bundle> && std::is_nothrow_default_constructible_v<Bundle>);
			queue_.back().resources = std::move(bundle);
			bundle = {};
			queue_.back().generation = generation;
			committed_ = true;
		}
	private:
		Queue& queue_;
		bool committed_ = false;
	};

	// These generic ownership helpers are used with Vulkan handles in production
	// and integer handles in CPU tests. Partial candidate cleanup follows exactly
	// the same framebuffer-before-pass order as complete bundle retirement.
	template<class Buffers, class Pass>
	[[nodiscard]] bool CompleteEmptyFrameBundle(std::size_t imageCount, const Buffers& framebuffers, Pass pass) noexcept
	{
		return imageCount != 0 && pass != Pass{} && framebuffers.size() == imageCount
			&& std::all_of(framebuffers.begin(), framebuffers.end(), [](auto handle) { return handle != decltype(handle){}; });
	}
	template<class Buffers, class Pass, class DestroyFramebuffer, class DestroyPass>
	void DestroyEmptyFrameBundle(Buffers& framebuffers, Pass& pass, DestroyFramebuffer destroyFramebuffer, DestroyPass destroyPass)
	{
		for (auto framebuffer : framebuffers) if (framebuffer != decltype(framebuffer){}) destroyFramebuffer(framebuffer);
		framebuffers.clear();
		if (pass != Pass{}) destroyPass(std::exchange(pass, Pass{}));
	}
	[[nodiscard]] constexpr std::uint32_t EmptyFramePresentSignalIndex(const AcquiredPresentationImage& image) noexcept
	{
		return image.imageIndex;
	}

	// A successful record/end is necessary before a live slot fence may reset.
	// This progression will also guard the backend's fallback submit boundary.
	class EmptyFrameSubmissionProgress
	{
	public:
		void RecordEnded() noexcept { recorded_ = true; }
		[[nodiscard]] bool MayResetFence() const noexcept { return recorded_ && !reset_; }
		[[nodiscard]] bool FenceResetAccepted() noexcept
		{
			if (!MayResetFence()) return false;
			reset_ = true;
			return true;
		}
		[[nodiscard]] bool MaySubmit() const noexcept { return recorded_ && reset_ && !submitted_; }
		[[nodiscard]] bool SubmitAccepted() noexcept
		{
			if (!MaySubmit()) return false;
			submitted_ = true;
			return true;
		}
	private:
		bool recorded_ = false, reset_ = false, submitted_ = false;
	};

	// A failed frame cannot borrow an ordinary slot until replacement sync owners
	// belong to a newer complete generation. Device loss remains a separate policy.
	class EmptyFrameRecoveryState
	{
	public:
		void Suspend(std::uint64_t generation) noexcept { suspended_ = true; generation_ = generation; }
		[[nodiscard]] bool Suspended() const noexcept { return suspended_; }
		[[nodiscard]] bool CompleteReplacement(std::uint64_t generation, bool complete) noexcept
		{
			if (!suspended_ || !complete || generation <= generation_) return false;
			suspended_ = false;
			return true;
		}
	private:
		bool suspended_ = false;
		std::uint64_t generation_ = 0;
	};

	enum class EmptyFrameCandidateCheckpoint { None, BeforeSwapchain, AfterFallbackPass };
	// Native validation only: no automation control or synthetic Vulkan result.
	struct EmptyFrameValidationRequest
	{
		bool forceEmpty = false;
		std::uint64_t correlationId = 0;
		bool consumeAcquire = false;
		enum class Checkpoint { None, BeforeRecording, BeforeFenceReset, BeforeQueueSubmit } checkpoint = Checkpoint::None;
	};
	struct EmptyFrameValidationReceipt
	{
		std::uint64_t correlationId = 0, sequence = 0, generation = 0;
		std::uint32_t frameSlot = 0, imageIndex = 0, commandBufferCount = 0;
		// Counts are captured with this acquisition's generation and indices.
		// Candidate rollback/cleanup must not replace them with later bundle sizes.
		std::uint32_t swapchainImageCount = 0, frameSlotCount = 0;
		std::uint64_t validationErrorsBefore = 0, validationErrorsAfter = 0;
		bool initialized = false, validationActuallyEnabled = false, acquired = false;
		bool forcedEmpty = false, acquireWaitConsumedBefore = false;
		bool recordEnded = false, fenceResetAccepted = false, submitAccepted = false, presentAttempted = false;
		bool acquisitionDrainSubmitted = false, acquisitionDrainCompleted = false, recoverySuspended = false;
		bool ordinarySubmitAccepted = false, ordinaryCompletionObserved = false, candidateRejected = false;
		bool maintenanceEnabled = false;
		bool resetAttempted = false, submitAttempted = false, waitIdleAttempted = false, waitIdleCompletionObserved = false;
		std::int32_t waitIdleResult = 0;
		EmptyFrameCandidateCheckpoint candidateCheckpoint = EmptyFrameCandidateCheckpoint::None;
		bool candidateFallbackPassCreated = false, candidatePartialCleanupCompleted = false, candidateOldSwapchainConsumed = false;
		std::uint64_t ordinarySubmissionSerial = 0;
		std::int32_t resetResult = 0, submitResult = 0, presentResult = 0;
		EmptyFrameValidationRequest::Checkpoint rejectedCheckpoint = EmptyFrameValidationRequest::Checkpoint::None;
	};
}
