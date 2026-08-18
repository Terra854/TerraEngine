// VulkanFrameSubmission.cpp — Frame queue submission, presentation, and completion publication
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

// VulkanContext::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::EndFrame() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (!impl->initialized || impl->skipFrame || impl->deviceLost) return;

	const auto endFrameStart = BackendClock::now();
	// Submit all recorded camera command buffers together so the swapchain is
	// presented once using the final camera's output.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		return;
	}

	auto& cmds = impl->pendingPrimaryCommandBuffers[impl->currentFrame];
	// RenderSystem can complete more than one editor-facing update before the UI
	// present path advances this frame slot. A primary command buffer recorded
	// with ONE_TIME_SUBMIT must never be submitted again while that slot is
	// pending; BeginFrame clears this marker only after its fence proves reuse.
	if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()
		&& impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] != 0u)
	{
		return;
	}
	if (impl->editorMode && impl->framebufferResized) {
		// A GLFW resize can arrive after scene/game command buffers have been
		// recorded but before they are submitted. Those command buffers may
		// reference old offscreen framebuffers, depth resources, or swapchain-
		// sized attachments, so submitting them can device-lost the queue on
		// some drivers. Drop this frame and rebuild the resized targets before
		// the editor UI pass acquires the next swapchain image.
		impl->DiscardRecordedHiZWrites(impl->currentFrame);
		cmds.clear();
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
		}
		impl->editorRenderSubmitted = false;
		// Claim the latest coalesced extent revision exactly once. Callbacks that
		// arrive during recreation publish a later revision for a later frame.
		if (!impl->ClaimFramebufferResizeRebuild()) {
			return;
		}
		const auto recoveryStart = BackendClock::now();
		impl->RecreateSwapChain();
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate editor swapchain before submit", true, false },
			ElapsedMilliseconds(recoveryStart),
			0.25f,
			"Editor resize was processed before submitting stale scene/game command buffers.");
		return;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cmds.empty()) {
		if (!impl->editorMode && impl->swapchainImageAcquired) {
			// Standalone runtime acquires the swapchain image in BeginFrame.
			// If render graph rejection or startup state leaves no recorded
			// command buffers, still consume the acquire semaphore and present
			// once so the binary semaphore and frame fence cannot be reused
			// while the presentation engine still owns pending work.
			VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
			VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
			VkSemaphore signalSemaphore = impl->uiSemaphores[impl->acquiredImageIndex];

			VkSubmitInfo emptySubmitInfo{};
			emptySubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			if (!impl->swapchainAcquireWaitConsumed) {
				emptySubmitInfo.waitSemaphoreCount = 1;
				emptySubmitInfo.pWaitSemaphores = waitSemaphores;
				emptySubmitInfo.pWaitDstStageMask = waitStages;
			}
			emptySubmitInfo.signalSemaphoreCount = 1;
			emptySubmitInfo.pSignalSemaphores = &signalSemaphore;

			const auto resetFenceStart = BackendClock::now();
			const VkResult resetResult = vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
			if (resetResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(resetResult, "empty runtime frame fence reset")) return;
				TERRAENGINE_LOG(LogLevel::Error, "Empty runtime frame fence reset failed: " << resetResult);
				impl->RecreateSwapChain();
				return;
			}
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::FrameAllocatorReset, RenderViewType::Game, 0u, "Reset frame fence for empty runtime submit", true, false },
				ElapsedMilliseconds(resetFenceStart));

			const auto submitStart = BackendClock::now();
			const VkResult submitResult = impl->SubmitGraphicsQueue(1, &emptySubmitInfo, impl->inFlightFences[impl->currentFrame]);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Empty runtime queue submit", true, false },
				ElapsedMilliseconds(submitStart),
				0.25f);
			if (submitResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(submitResult, "empty runtime queue submit")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] Empty QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
				impl->RecreateSwapChain();
				return;
			}

			VkPresentInfoKHR presentInfo{};
			presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
			presentInfo.waitSemaphoreCount = 1;
			presentInfo.pWaitSemaphores = &signalSemaphore;

			VkSwapchainKHR swapChains[] = {impl->swapchain};
			presentInfo.swapchainCount = 1;
			presentInfo.pSwapchains = swapChains;
			presentInfo.pImageIndices = &impl->acquiredImageIndex;
			VkSwapchainPresentFenceInfoKHR presentCompletionInfo{};
			const bool presentCompletionFencePrepared = impl->PreparePresentationCompletionFence(
				impl->acquiredImageIndex, presentInfo, presentCompletionInfo);
			if (impl->presentationFencesEnabled && !presentCompletionFencePrepared) {
				// Preserve correctness by retaining retired bundles through the bounded
				// fallback if an optional completion fence cannot be prepared.
				impl->presentationFencesEnabled = false;
			}

			const auto presentStart = BackendClock::now();
			const VkResult presentResult = impl->PresentSwapchain(&presentInfo);
			impl->ResolveUnenqueuedPresentationFence(presentCompletionFencePrepared, presentResult);
			impl->RecordUnextendedPresent(impl->acquiredImageIndex, presentResult);
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::Presentation, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0u, "Empty runtime queue present", true, false },
				ElapsedMilliseconds(presentStart),
				0.25f);
			if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
				impl->RequestOutOfDateRecovery();
			} else if (presentResult == VK_SUBOPTIMAL_KHR) {
				impl->RequestSuboptimalRecoveryIfNeeded();
			}
			if (impl->framebufferResized && impl->ClaimFramebufferResizeRebuild()) {
				impl->RecreateSwapChain();
			} else if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
				if (impl->MarkDeviceLost(presentResult, "empty runtime queue present")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "failed to present empty swap chain image! Result: " << presentResult);
			}

			impl->swapchainImageAcquired = false;
			impl->swapchainAcquireWaitConsumed = false;
			impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI End Frame", true, false },
				ElapsedMilliseconds(endFrameStart));
			return;
		}
		return;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode && impl->swapchainImageAcquired && !impl->swapchainAcquireWaitConsumed) {
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
	}

	submitInfo.commandBufferCount = static_cast<uint32_t>(cmds.size());
	submitInfo.pCommandBuffers = cmds.data();

	VkSemaphore signalSemaphore = impl->editorMode
		? impl->renderFinishedSemaphores[impl->currentFrame]
		: impl->uiSemaphores[impl->acquiredImageIndex];

	VkSemaphore signalSemaphores[] = {signalSemaphore};
	// Signal render finished only when command buffers are submitted; PresentEditor
	// checks editorRenderSubmitted before waiting on this binary semaphore.
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (impl->editorMode) {
		// In Editor Mode, we defer the frame fence signal to the UI pass (PresentEditor)
		// so the frame is considered "in flight" until the UI is also done.
		const auto submitStart = BackendClock::now();
		VkResult submitResult = impl->SubmitGraphicsQueue(1, &submitInfo, VK_NULL_HANDLE);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Editor render queue submit", true, false },
			ElapsedMilliseconds(submitStart),
			0.25f);
		if (submitResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(submitResult, "editor render queue submit")) {
				cmds.clear();
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffers! Result: " << submitResult);
			const VkResult idleResult = impl->WaitGraphicsQueueIdle();
			if (idleResult != VK_SUCCESS) {
				if (impl->MarkDeviceLost(idleResult, "editor render recovery queue idle")) {
					return;
				}
				TERRAENGINE_LOG(LogLevel::Error, "Skipping editor render recovery because the graphics queue did not prove idle: " << idleResult);
				return;
			}
			impl->DiscardRecordedHiZWrites(impl->currentFrame);
			cmds.clear();
			if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
				impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 0u;
			}
			impl->editorRenderSubmitted = false;
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after editor submit failure", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Queue submit failed during editor rendering; swapchain recovery was required.");
			return;
		}
		impl->editorRenderSubmitted = true;
		if (impl->animationComputeRecordedForCurrentFrame
			&& !impl->animationComputeSubmissionMarkedForCurrentFrame) {
			// Editor UI extends this queue chain to the frame fence. Mark only after
			// the existing render submission accepted the recorded compute prefix.
			impl->MarkVulkanAnimationGenerationSubmitted(impl->pendingAnimationCommandPlan.rendererGeneration);
			impl->animationComputeSubmissionMarkedForCurrentFrame = true;
		}
		impl->ConfirmHiZWritesSubmitted(impl->currentFrame);
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 1u;
		}
	} else {
		const auto resetFenceStart = BackendClock::now();
		const VkResult resetResult = vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
		if (resetResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(resetResult, "runtime frame fence reset")) return;
			TERRAENGINE_LOG(LogLevel::Error, "Runtime frame fence reset failed: " << resetResult);
			impl->RecreateSwapChain();
			return;
		}
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStage(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::FrameAllocatorReset, RenderViewType::Game, 0u, "Reset frame fence", true, false },
			ElapsedMilliseconds(resetFenceStart));
		const auto submitStart = BackendClock::now();
		VkResult submitResult = impl->SubmitGraphicsQueue(1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::QueueSubmission, TerraEngine::Renderer::BackendSubmitStage::QueueSubmit, RenderViewType::Game, 0u, "Runtime render queue submit", true, false },
			ElapsedMilliseconds(submitStart),
			0.25f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (submitResult != VK_SUCCESS) {
			if (impl->MarkDeviceLost(submitResult, "runtime render queue submit")) {
				return;
			}
			impl->DiscardRecordedHiZWrites(impl->currentFrame);
			TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after runtime submit failure", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Queue submit failed during runtime rendering; swapchain recovery was required.");
			return;
		}
		if (impl->animationComputeRecordedForCurrentFrame
			&& !impl->animationComputeSubmissionMarkedForCurrentFrame) {
			// Runtime compute and graphics share this one fence-bearing submit.
			impl->MarkVulkanAnimationGenerationSubmitted(impl->pendingAnimationCommandPlan.rendererGeneration);
			impl->animationComputeSubmissionMarkedForCurrentFrame = true;
		}
		impl->ConfirmHiZWritesSubmitted(impl->currentFrame);
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 1u;
		}
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (RenderSnapshotOwnerSummary& owner : impl->pendingRenderSnapshotDiagnostics.ownerSummaries) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (owner.state == RenderSnapshotState::Recorded) {
			owner.state = RenderSnapshotState::Submitted;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!impl->editorMode) {
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = {impl->swapchain};
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &impl->acquiredImageIndex;
		VkSwapchainPresentFenceInfoKHR presentCompletionInfo{};
		const bool presentCompletionFencePrepared = impl->PreparePresentationCompletionFence(
			impl->acquiredImageIndex, presentInfo, presentCompletionInfo);
		if (impl->presentationFencesEnabled && !presentCompletionFencePrepared) {
			impl->presentationFencesEnabled = false;
		}

		const auto presentStart = BackendClock::now();
		VkResult result = impl->PresentSwapchain(&presentInfo);
		impl->ResolveUnenqueuedPresentationFence(presentCompletionFencePrepared, result);
		impl->RecordUnextendedPresent(impl->acquiredImageIndex, result);
		TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
			impl->pendingBackendFrameBreakdown,
			{ TerraEngine::Renderer::BackendFrameCostCategory::Presentation, TerraEngine::Renderer::BackendSubmitStage::Present, RenderViewType::Game, 0u, "Queue present", true, false },
			ElapsedMilliseconds(presentStart),
			0.25f);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			impl->RequestOutOfDateRecovery();
		} else if (result == VK_SUBOPTIMAL_KHR) {
			impl->RequestSuboptimalRecoveryIfNeeded();
		}
		if (impl->framebufferResized && impl->ClaimFramebufferResizeRebuild()) {
			const auto recoveryStart = BackendClock::now();
			impl->RecreateSwapChain();
			TerraEngine::Renderer::Vulkan::RecordVulkanFrameStall(
				impl->pendingBackendFrameBreakdown,
				{ TerraEngine::Renderer::BackendFrameCostCategory::ResizeRecreateWait, TerraEngine::Renderer::BackendSubmitStage::SwapchainRecovery, RenderViewType::Game, 0u, "Recreate swapchain after present", true, false },
				ElapsedMilliseconds(recoveryStart),
				0.25f,
				"Present reported resize or suboptimal swapchain state.");
		} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			if (impl->MarkDeviceLost(result, "runtime queue present")) {
				return;
			}
			TERRAENGINE_LOG(LogLevel::Error, "failed to present swap chain image! Result: " << result);
		}

		impl->swapchainImageAcquired = false;
		impl->swapchainAcquireWaitConsumed = false;
		impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	}
	TerraEngine::Renderer::Vulkan::RecordVulkanFrameCost(
		impl->pendingBackendFrameBreakdown,
		{ TerraEngine::Renderer::BackendFrameCostCategory::RhiEndFrame, TerraEngine::Renderer::BackendSubmitStage::Other, RenderViewType::Game, 0u, "RHI End Frame", true, false },
		ElapsedMilliseconds(endFrameStart));
	{
		std::lock_guard<std::mutex> lock(impl->statsMutex);
		impl->latestBackendFrameBreakdown = impl->pendingBackendFrameBreakdown;
	}
}

