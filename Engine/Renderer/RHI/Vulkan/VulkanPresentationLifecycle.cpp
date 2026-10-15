// VulkanPresentationLifecycle.cpp - Deterministic presentation state model.

#include "VulkanPresentationLifecycle.h"

#include <algorithm>

namespace TerraEngine::Renderer::Vulkan
{
	void PresentationRecreationCoordinator::SetActiveExtent(PresentationExtent extent) noexcept
	{
		activeExtent_ = extent;
		requestedExtent_ = extent;
	}

	void PresentationRecreationCoordinator::Request(PresentationRecreationReason reason) noexcept
	{
		// Every explicit recovery cause represents a new protected attempt, even
		// when the compositor has not reported a different pixel extent yet.
		reason_ = reason;
		recoveryPending_ = true;
		++revision_;
	}

	void PresentationRecreationCoordinator::RequestFramebufferExtent(PresentationExtent extent) noexcept
	{
		if (extent == requestedExtent_)
		{
			return;
		}

		requestedExtent_ = extent;
		Request(PresentationRecreationReason::FramebufferExtentChanged);
	}

	void PresentationRecreationCoordinator::RequestOutOfDate() noexcept
	{
		Request(PresentationRecreationReason::OutOfDate);
	}

	bool PresentationRecreationCoordinator::RequestSuboptimalIfExtentChanged() noexcept
	{
		// VK_SUBOPTIMAL_KHR is advisory. Replacing an otherwise usable chain only
		// makes sense when the currently requested presentation key differs.
		// While retirement proof is pending, another advisory revision would keep
		// retrying the same full backlog and prevent the active chain from presenting.
		if (retirementProofPending_ || requestedExtent_.IsZero() || requestedExtent_ == activeExtent_)
		{
			return false;
		}

		Request(PresentationRecreationReason::SuboptimalExtentChanged);
		return true;
	}

	std::optional<PresentationRecreationRequest> PresentationRecreationCoordinator::Claim() noexcept
	{
		if (!recoveryPending_ || requestedExtent_.IsZero() || claimedRevision_ == revision_)
		{
			return std::nullopt;
		}

		claimedRevision_ = revision_;
		return PresentationRecreationRequest{ requestedExtent_, reason_, revision_ };
	}

	void PresentationRecreationCoordinator::PublishSuccess(PresentationExtent extent) noexcept
	{
		activeExtent_ = extent;
		requestedExtent_ = extent;
		recoveryPending_ = false;
		oldSwapchainWasConsumed_ = false;
		retirementProofPending_ = false;
	}

	void PresentationRecreationCoordinator::PublishFailure(bool oldSwapchainWasConsumed) noexcept
	{
		// Leave the claimed revision consumed. Retrying it every frame would turn
		// a recoverable driver failure or minimized window into an idle-loop.
		oldSwapchainWasConsumed_ = oldSwapchainWasConsumed;
		recoveryPending_ = true;
		retirementProofPending_ = false;
	}

	void PresentationRecreationCoordinator::PublishReservationFailure() noexcept
	{
		// No native owner moved, but the suspended frame cannot acquire another
		// image to generate a new recovery event. Arm a fresh revision for the
		// next protected frame boundary; Claim consumes it once, so persistent
		// memory pressure cannot recurse or spin within this failed attempt.
		PublishFailure(false);
		Request(PresentationRecreationReason::CandidateFailure);
	}

	void PresentationRecreationCoordinator::PublishRetirementBlocked(bool oldSwapchainWasConsumed) noexcept
	{
		// This failure is different from a bad candidate: retrying becomes useful
		// once explicit present-fence proof releases the old bundle or backlog slot.
		oldSwapchainWasConsumed_ = oldSwapchainWasConsumed;
		recoveryPending_ = true;
		retirementProofPending_ = true;
	}

	bool PresentationRecreationCoordinator::RequestRetryAfterRetirement() noexcept
	{
		if (!recoveryPending_ || !retirementProofPending_)
		{
			return false;
		}

		// The caller has verified the required retired bundle or capacity is now
		// available. Publish one new revision so Claim performs one bounded retry.
		retirementProofPending_ = false;
		oldSwapchainWasConsumed_ = false;
		Request(PresentationRecreationReason::CandidateFailure);
		return true;
	}

	bool PresentationRecreationCoordinator::IsSuspendedForZeroExtent() const noexcept
	{
		return recoveryPending_ && requestedExtent_.IsZero();
	}

	bool PresentationRecreationCoordinator::IsRecoveryPending() const noexcept
	{
		return recoveryPending_;
	}

	void PresentationRetirementTracker::BeginRetirement(std::uint64_t generation, std::uint32_t presentCount)
	{
		generation_ = generation;
		completionByPresent_.assign(presentCount, false);
		completionCoverageValid_ = true;
	}

	void PresentationRetirementTracker::RecordPresentFence(
		std::uint64_t generation, std::uint32_t presentIndex, bool signaled)
	{
		if (generation == generation_ && presentIndex < completionByPresent_.size())
		{
			completionByPresent_[presentIndex] = signaled;
		}
	}

	void PresentationRetirementTracker::RecordUnfencedPresent(std::uint64_t generation) noexcept
	{
		// A single untracked present invalidates the whole generation: older signaled
		// fences cannot prove that the presentation engine released this later wait.
		if (generation == generation_) completionCoverageValid_ = false;
	}

	bool PresentationRetirementTracker::CanDestroyRetiredBundle(std::uint64_t generation) const noexcept
	{
		return generation == generation_
			&& completionCoverageValid_
			&& !completionByPresent_.empty()
			&& std::all_of(completionByPresent_.begin(), completionByPresent_.end(), [](bool complete) { return complete; });
	}

	void UnextendedPresentationRetirementTracker::BeginGeneration(
		std::uint64_t generation, bool retiredBundlesPending) noexcept
	{
		// Ignore stale notifications. A replacement can occur before its first
		// present, so preserve the pending old bundles and re-anchor them to the
		// first present of the newest generation.
		if (generation < activeGeneration_)
		{
			return;
		}

		if (generation == activeGeneration_)
		{
			retiredBundlesPending_ = retiredBundlesPending_ || retiredBundlesPending;
			return;
		}

		activeGeneration_ = generation;
		retiredBundlesPending_ = retiredBundlesPending_ || retiredBundlesPending;
		firstPresentAnchored_ = false;
		anchorGeneration_ = 0u;
		anchorImageIndex_ = 0u;
	}

	void UnextendedPresentationRetirementTracker::RecordPresent(
		std::uint64_t generation, std::uint32_t imageIndex) noexcept
	{
		if (!retiredBundlesPending_ || generation != activeGeneration_ || firstPresentAnchored_)
		{
			return;
		}

		// Do not require an image count here: the production acquire result is
		// authoritative, and this model's job is only to preserve identity.
		anchorGeneration_ = generation;
		anchorImageIndex_ = imageIndex;
		firstPresentAnchored_ = true;
	}

	bool UnextendedPresentationRetirementTracker::NeedsAcquireProof() const noexcept
	{
		return retiredBundlesPending_ && firstPresentAnchored_;
	}

	bool UnextendedPresentationRetirementTracker::RecordCompletedAcquire(
		std::uint64_t generation, std::uint32_t imageIndex) noexcept
	{
		if (!NeedsAcquireProof() || generation != anchorGeneration_
			|| imageIndex != anchorImageIndex_)
		{
			return false;
		}

		retiredBundlesPending_ = false;
		firstPresentAnchored_ = false;
		return true;
	}

	void UnextendedPresentationRetirementTracker::Reset() noexcept
	{
		activeGeneration_ = 0u;
		anchorGeneration_ = 0u;
		anchorImageIndex_ = 0u;
		retiredBundlesPending_ = false;
		firstPresentAnchored_ = false;
	}

	bool PresentationLifecycle::Configure(std::uint32_t concurrentFrames, std::uint32_t surfaceMinimum,
		std::uint32_t requestedImages, std::uint32_t actualImages)
	{
		// Vulkan may return more images than requested, but never accept zero-sized
		// sets or a result below either the surface minimum or requested minimum.
		if (concurrentFrames == 0 || surfaceMinimum == 0 || requestedImages < surfaceMinimum
			|| actualImages < requestedImages)
		{
			state_ = PresentationLifecycleState::RecreationRequested;
			return false;
		}

		concurrentFrames_ = concurrentFrames;
		surfaceMinimum_ = surfaceMinimum;
		requestedImages_ = requestedImages;
		actualImages_ = actualImages;
		++generation_;
		state_ = PresentationLifecycleState::Ready;
		return true;
	}

	bool PresentationLifecycle::Validate(const AcquiredPresentationImage& image) const noexcept
	{
		return state_ == PresentationLifecycleState::Ready
			&& image.presentationGeneration == generation_
			&& image.frameSlot < concurrentFrames_
			&& image.imageIndex < actualImages_;
	}

	bool PresentationLifecycle::CanReleasePresentSynchronization(std::uint64_t presentCompletionGeneration) const noexcept
	{
		// A render-submit fence does not prove that presentation has consumed its
		// wait semaphore. The caller must provide a presentation completion proof.
		return presentCompletionGeneration >= generation_;
	}

	void PresentationLifecycle::RequestRecreation() noexcept
	{
		if (state_ != PresentationLifecycleState::DeviceLost && state_ != PresentationLifecycleState::Shutdown)
		{
			state_ = PresentationLifecycleState::RecreationRequested;
		}
	}

	void PresentationLifecycle::PublishRecreation(PresentationRecreationOutcome outcome,
		std::uint32_t surfaceMinimum, std::uint32_t requestedImages, std::uint32_t actualImages)
	{
		switch (outcome)
		{
		case PresentationRecreationOutcome::Success:
			Configure(concurrentFrames_, surfaceMinimum, requestedImages, actualImages);
			break;
		case PresentationRecreationOutcome::DeferredZeroExtent:
			state_ = PresentationLifecycleState::DeferredZeroExtent;
			break;
		case PresentationRecreationOutcome::RecoverableFailure:
			state_ = PresentationLifecycleState::RecreationRequested;
			break;
		case PresentationRecreationOutcome::DeviceLost:
			MarkDeviceLost();
			break;
		}
	}

	void PresentationLifecycle::MarkDeviceLost() noexcept
	{
		state_ = PresentationLifecycleState::DeviceLost;
	}

	EditorFrameCompletionTracker::EditorFrameCompletionTracker(std::size_t frameCount)
		: serialByFrame_(frameCount, 0)
	{
	}

	void EditorFrameCompletionTracker::ResetFrameCount(std::size_t frameCount)
	{
		// Recreation changes frame resources but does not reset the global serial
		// domain; retired descriptors continue to compare against it safely.
		serialByFrame_.assign(frameCount, 0);
	}

	std::uint64_t EditorFrameCompletionTracker::RecordSubmission(std::size_t frameSlot)
	{
		if (deviceLost_ || frameSlot >= serialByFrame_.size())
		{
			return 0;
		}

		const std::uint64_t serial = ++lastSubmittedSerial_;
		serialByFrame_[frameSlot] = serial;
		return serial;
	}

	bool EditorFrameCompletionTracker::RecordFenceCompletion(std::size_t frameSlot)
	{
		if (frameSlot >= serialByFrame_.size())
		{
			return false;
		}

		completedSerial_ = std::max(completedSerial_, serialByFrame_[frameSlot]);
		serialByFrame_[frameSlot] = 0;
		return true;
	}

	void EditorFrameCompletionTracker::RecordProtectedIdle()
	{
		completedSerial_ = lastSubmittedSerial_;
		std::fill(serialByFrame_.begin(), serialByFrame_.end(), 0);
	}

	void EditorFrameCompletionTracker::SetPresentationGeneration(std::uint64_t generation) noexcept
	{
		presentationGeneration_ = generation;
	}

	void EditorFrameCompletionTracker::MarkDeviceLost() noexcept
	{
		deviceLost_ = true;
	}

	EditorSubmissionProgress EditorFrameCompletionTracker::Snapshot() const noexcept
	{
		return { lastSubmittedSerial_, completedSerial_, presentationGeneration_, deviceLost_ };
	}

	std::string_view EditorUiFrameStageName(EditorUiFrameStage stage) noexcept
	{
		switch (stage)
		{
		case EditorUiFrameStage::None: return "None";
		case EditorUiFrameStage::WaitFrameFence: return "WaitFrameFence";
		case EditorUiFrameStage::AcquireImage: return "AcquireImage";
		case EditorUiFrameStage::ResetCommandPool: return "ResetCommandPool";
		case EditorUiFrameStage::AllocateCommandBuffer: return "AllocateCommandBuffer";
		case EditorUiFrameStage::BeginCommandBuffer: return "BeginCommandBuffer";
		case EditorUiFrameStage::DiagnosticNaming: return "DiagnosticNaming";
		case EditorUiFrameStage::RecordDrawData: return "RecordDrawData";
		case EditorUiFrameStage::EndCommandBuffer: return "EndCommandBuffer";
		case EditorUiFrameStage::ResetFrameFence: return "ResetFrameFence";
		case EditorUiFrameStage::Submit: return "Submit";
		case EditorUiFrameStage::Present: return "Present";
		case EditorUiFrameStage::DrainSemaphore: return "DrainSemaphore";
		case EditorUiFrameStage::RepairFence: return "RepairFence";
		}
		return "Unknown";
	}
}
