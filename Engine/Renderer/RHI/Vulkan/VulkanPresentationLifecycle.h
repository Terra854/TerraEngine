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
