// VulkanPresentationLifecycle.cpp - Deterministic presentation state model.

#include "VulkanPresentationLifecycle.h"

#include <algorithm>

namespace TerraEngine::Renderer::Vulkan
{
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
