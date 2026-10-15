// VulkanPresentationLifecycle.h - CPU-side presentation ownership contracts.
//
// These types deliberately avoid Vulkan handles so synchronization decisions
// can be exhaustively tested without a GPU. The Vulkan backend stores the real
// handles, while this model validates which frame, image, and generation owns
// each operation before a collection is indexed.

#pragma once

#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	// MaintenanceExtensionLineage identifies which spelling of the promoted
	// surface/swapchain maintenance contract should be enabled together.
	enum class MaintenanceExtensionLineage
	{
		None,
		Ext,
		Khr
	};

	// Prefer the original EXT lineage when both spellings are advertised. The
	// structures have identical behavior, while older validation layers only
	// recognize the EXT extension and otherwise reject the aliased pNext nodes.
	[[nodiscard]] constexpr MaintenanceExtensionLineage SelectMaintenanceExtensionLineage(
		bool extAvailable, bool khrAvailable) noexcept
	{
		if (extAvailable) return MaintenanceExtensionLineage::Ext;
		if (khrAvailable) return MaintenanceExtensionLineage::Khr;
		return MaintenanceExtensionLineage::None;
	}

	enum class PresentationLifecycleState
	{
		Uninitialized,
		Ready,
		RecreationRequested,
		DeferredZeroExtent,
		DeviceLost,
		Shutdown
	};

	enum class PresentationRecreationOutcome
	{
		Success,
		DeferredZeroExtent,
		RecoverableFailure,
		DeviceLost
	};

	enum class EditorUiFrameStage
	{
		None,
		WaitFrameFence,
		AcquireImage,
		ResetCommandPool,
		AllocateCommandBuffer,
		BeginCommandBuffer,
		DiagnosticNaming,
		RecordDrawData,
		EndCommandBuffer,
		ResetFrameFence,
		Submit,
		Present,
		DrainSemaphore,
		RepairFence
	};

	enum class EditorPresentOutcome
	{
		Submitted,
		Skipped,
		RecreationRequested,
		RecoverableFailure,
		DeviceLost
	};

	struct AcquiredPresentationImage
	{
		std::uint32_t frameSlot = 0;
		std::uint32_t imageIndex = 0;
		std::uint64_t presentationGeneration = 0;
	};

	struct EditorSubmissionProgress
	{
		std::uint64_t lastSubmittedSerial = 0;
		std::uint64_t completedSerial = 0;
		std::uint64_t presentationGeneration = 0;
		bool deviceLost = false;
	};

	struct EditorPresentResult
	{
		EditorPresentOutcome outcome = EditorPresentOutcome::Skipped;
		EditorUiFrameStage stage = EditorUiFrameStage::None;
		std::int32_t nativeResult = 0;
		std::uint64_t submissionSerial = 0;
		std::optional<AcquiredPresentationImage> acquiredImage;

		[[nodiscard]] bool Submitted() const noexcept
		{
			return outcome == EditorPresentOutcome::Submitted;
		}
	};

	// PresentationRecreationReason records why the coordinator must replace the
	// native presentation chain. Keeping this CPU-only makes resize policy
	// deterministic in tests instead of depending on a window manager callback.
	enum class PresentationRecreationReason
	{
		FramebufferExtentChanged,
		OutOfDate,
		SuboptimalExtentChanged,
		CandidateFailure
	};

	struct PresentationExtent
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		[[nodiscard]] bool IsZero() const noexcept { return width == 0 || height == 0; }
		[[nodiscard]] bool operator==(const PresentationExtent&) const noexcept = default;
	};

	struct PresentationRecreationRequest
	{
		PresentationExtent extent{};
		PresentationRecreationReason reason = PresentationRecreationReason::FramebufferExtentChanged;
		std::uint64_t revision = 0;
	};

	// PresentationRecreationCoordinator coalesces native resize bursts into one
	// protected recreation attempt. Native candidate failures require a new event
	// or explicit retirement proof. Pre-handoff host reservation failures arm
	// one later attempt; zero extents still prevent a minimized rebuild loop.
	class PresentationRecreationCoordinator
	{
	public:
		void SetActiveExtent(PresentationExtent extent) noexcept;
		void RequestFramebufferExtent(PresentationExtent extent) noexcept;
		void RequestOutOfDate() noexcept;
		[[nodiscard]] bool RequestSuboptimalIfExtentChanged() noexcept;
		[[nodiscard]] std::optional<PresentationRecreationRequest> Claim() noexcept;
		void PublishSuccess(PresentationExtent extent) noexcept;
		void PublishFailure(bool oldSwapchainWasConsumed) noexcept;
		// A host reservation failure precedes native handoff. Keep the current
		// extent and arm one next-frame attempt without waiting for a resize.
		void PublishReservationFailure() noexcept;
		void PublishRetirementBlocked(bool oldSwapchainWasConsumed) noexcept;
		[[nodiscard]] bool RequestRetryAfterRetirement() noexcept;

		[[nodiscard]] bool IsSuspendedForZeroExtent() const noexcept;
		[[nodiscard]] bool IsRecoveryPending() const noexcept;
		[[nodiscard]] bool IsWaitingForRetirementProof() const noexcept { return retirementProofPending_; }
		[[nodiscard]] bool OldSwapchainWasConsumed() const noexcept { return oldSwapchainWasConsumed_; }
		[[nodiscard]] PresentationExtent RequestedExtent() const noexcept { return requestedExtent_; }
		[[nodiscard]] std::uint64_t Revision() const noexcept { return revision_; }

	private:
		void Request(PresentationRecreationReason reason) noexcept;

		PresentationExtent activeExtent_{};
		PresentationExtent requestedExtent_{};
		PresentationRecreationReason reason_ = PresentationRecreationReason::FramebufferExtentChanged;
		std::uint64_t revision_ = 0;
		std::uint64_t claimedRevision_ = 0;
		bool recoveryPending_ = false;
		bool oldSwapchainWasConsumed_ = false;
		bool retirementProofPending_ = false;
	};

	// PresentationRetirementTracker is a fake-dispatch equivalent for the
	// per-present maintenance-fence contract. Tests drive fence completion and
	// coverage loss explicitly so retirement never relies on a real compositor.
	class PresentationRetirementTracker
	{
	public:
		void BeginRetirement(std::uint64_t generation, std::uint32_t presentCount);
		void RecordPresentFence(std::uint64_t generation, std::uint32_t presentIndex, bool signaled);
		void RecordUnfencedPresent(std::uint64_t generation) noexcept;
		[[nodiscard]] bool CanDestroyRetiredBundle(std::uint64_t generation) const noexcept;

	private:
		std::uint64_t generation_ = 0u;
		std::vector<bool> completionByPresent_;
		bool completionCoverageValid_ = false;
	};

	// UnextendedPresentationRetirementTracker models the Khronos fallback used
	// only when swapchain-maintenance present fences are unavailable. A completed
	// acquire of the first image presented by the newest chain proves that the
	// presentation engine has moved beyond every retired chain. It deliberately
	// has no Vulkan handles so callers can test the ownership rule deterministically.
	class UnextendedPresentationRetirementTracker
	{
	public:
		// Start or replace the active swapchain generation. A newer generation
		// folds any older retired bundles under its first future presentation.
		void BeginGeneration(std::uint64_t generation, bool retiredBundlesPending) noexcept;
		// The first present in the active generation becomes the acquire proof
		// anchor. Later presents do not replace that anchor.
		void RecordPresent(std::uint64_t generation, std::uint32_t imageIndex) noexcept;
		[[nodiscard]] bool NeedsAcquireProof() const noexcept;
		// Returns true exactly once when a completed acquire matches the newest
		// first-present anchor and can therefore release all retired bundles.
		[[nodiscard]] bool RecordCompletedAcquire(std::uint64_t generation,
			std::uint32_t imageIndex) noexcept;
		void Reset() noexcept;

	private:
		std::uint64_t activeGeneration_ = 0u;
		std::uint64_t anchorGeneration_ = 0u;
		std::uint32_t anchorImageIndex_ = 0u;
		bool retiredBundlesPending_ = false;
		bool firstPresentAnchored_ = false;
	};

	// PresentationLifecycle validates collection sizes and rejects stale image
	// records before the Vulkan backend touches an image-owned handle.
	class TERRAENGINE_API PresentationLifecycle
	{
	public:
		bool Configure(std::uint32_t concurrentFrames, std::uint32_t surfaceMinimum,
			std::uint32_t requestedImages, std::uint32_t actualImages);
		[[nodiscard]] bool Validate(const AcquiredPresentationImage& image) const noexcept;
		[[nodiscard]] bool CanReleasePresentSynchronization(std::uint64_t presentCompletionGeneration) const noexcept;
		void RequestRecreation() noexcept;
		void PublishRecreation(PresentationRecreationOutcome outcome, std::uint32_t surfaceMinimum,
			std::uint32_t requestedImages, std::uint32_t actualImages);
		void MarkDeviceLost() noexcept;

		[[nodiscard]] PresentationLifecycleState State() const noexcept { return state_; }
		[[nodiscard]] std::uint32_t ConcurrentFrames() const noexcept { return concurrentFrames_; }
		[[nodiscard]] std::uint32_t ActualImages() const noexcept { return actualImages_; }
		[[nodiscard]] std::uint64_t Generation() const noexcept { return generation_; }

	private:
		PresentationLifecycleState state_ = PresentationLifecycleState::Uninitialized;
		std::uint32_t concurrentFrames_ = 0;
		std::uint32_t surfaceMinimum_ = 0;
		std::uint32_t requestedImages_ = 0;
		std::uint32_t actualImages_ = 0;
		std::uint64_t generation_ = 0;
	};

	// EditorFrameCompletionTracker assigns monotonically increasing serials only
	// to successful submissions and advances completion only from fence evidence.
	class TERRAENGINE_API EditorFrameCompletionTracker
	{
	public:
		explicit EditorFrameCompletionTracker(std::size_t frameCount = 0);
		void ResetFrameCount(std::size_t frameCount);
		[[nodiscard]] std::uint64_t RecordSubmission(std::size_t frameSlot);
		bool RecordFenceCompletion(std::size_t frameSlot);
		void RecordProtectedIdle();
		void SetPresentationGeneration(std::uint64_t generation) noexcept;
		void MarkDeviceLost() noexcept;
		[[nodiscard]] EditorSubmissionProgress Snapshot() const noexcept;

	private:
		std::vector<std::uint64_t> serialByFrame_;
		std::uint64_t lastSubmittedSerial_ = 0;
		std::uint64_t completedSerial_ = 0;
		std::uint64_t presentationGeneration_ = 0;
		bool deviceLost_ = false;
	};

	[[nodiscard]] TERRAENGINE_API std::string_view EditorUiFrameStageName(EditorUiFrameStage stage) noexcept;
}
