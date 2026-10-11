// VulkanFrameSubmission.cpp — Frame queue submission, presentation, and completion publication
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

// These receipt operations run only at the existing protected queue/fence
// boundaries. Their clocks describe actual submissions, never CPU recording.
void VulkanContext::Impl::ConfirmShadowWritesSubmitted(std::uint32_t frameIndex,
	std::span<const VkCommandBuffer> commands)
{
	if (frameIndex >= shadowSubmitReceiptsByFrame.size()) return;
	const bool hasNewCommands = std::any_of(commands.begin(), commands.end(), [&](auto command) {
		const auto found = shadowCommandOwnership.find(command);
		return found != shadowCommandOwnership.end() && found->second.frameIndex == frameIndex && found->second.submitSerial == 0U;
	});
	if (!hasNewCommands) return;
	if (nextShadowSubmitSerial == 0U) return; // Exhausted receipts never recycle.
	const auto receipt = nextShadowSubmitSerial++;
	// Native work is already accepted. Preserve staging with no allocations before
	// building CPU lists; even an allocation failure must retain its GPU lifetime.
	for (auto command : commands)
	{
		const auto found = shadowCommandOwnership.find(command);
		if (found != shadowCommandOwnership.end() && found->second.frameIndex == frameIndex && found->second.submitSerial == 0U)
		{
			found->second.submitSerial = receipt;
			// Fixed storage preserves the actual command-to-submit join even when
			// later diagnostic allocation fails after native work was accepted.
			staticShadowAcceptedSubmissions[staticShadowAcceptedCursor++ % staticShadowAcceptedSubmissions.size()]
				= {found->second.serial, receipt, frameIndex};
			shadowValidationReadback.PreserveSubmittedCommand(found->second.serial, receipt);
			lightingDepthEvidence.PreserveSubmittedCommand(found->second.serial, receipt);
			// No allocation follows accepted queue work in this lifetime handoff.
			staticShadowLifetime.PreserveSubmittedUse(found->second.serial, receipt);
			for (auto& entry : staticShadowEntries)
				if (entry->initializationCommand == found->second.serial) entry->layoutEstablished = true;
		}
	}
	const auto invalidateLedger = [&]() {
		shadowPublicationLedger.Reset(renderSessionGeneration);
		staticShadowPublicationLedger.Reset(renderSessionGeneration);
		shadowPhysicalTokenHints.clear(); shadowCacheEntries.clear();
	};
	try
	{
		std::vector<std::uint64_t> ordered;
		std::vector<std::uint64_t> validationCommands;
		std::vector<std::uint64_t> staticOrdered;
		ordered.reserve(commands.size());
		validationCommands.reserve(commands.size());
		staticOrdered.reserve(commands.size());
		for (auto command : commands)
		{
			const auto found = shadowCommandOwnership.find(command);
			if (found != shadowCommandOwnership.end() && found->second.frameIndex == frameIndex && found->second.submitSerial == receipt)
			{
				validationCommands.push_back(found->second.serial);
				if (shadowPublicationLedger.HasRecordedCandidate(found->second.serial)) ordered.push_back(found->second.serial);
				if (staticShadowPublicationLedger.HasRecordedCandidate(found->second.serial)) staticOrdered.push_back(found->second.serial);
			}
		}
		if (validationCommands.empty()) return; // Shadow-free primaries do not invent coverage.
		// The command map remains a nonallocating completion fallback if this push
		// fails. Normal resets retain its submitted entries until actual completion.
		shadowSubmitReceiptsByFrame[frameIndex].push_back(receipt);
		shadowValidationReadback.ConfirmSubmitted(validationCommands, receipt);
		lightingDepthEvidence.ConfirmSubmitted(validationCommands, receipt);
		if (!staticOrdered.empty() && !staticShadowPublicationLedger.ConfirmSubmissionBatch(staticOrdered, receipt, true))
			staticShadowPublicationLedger.Reset(renderSessionGeneration);
		if (!ordered.empty() && !shadowPublicationLedger.ConfirmSubmissionBatch(ordered, receipt, true))
		{
			invalidateLedger();
			return;
		}
	}
	catch (const std::bad_alloc&)
	{
		// GPU work was accepted, but missing CPU proof conservatively disables reuse.
		invalidateLedger();
	}
}

void VulkanContext::Impl::DiscardRecordedShadowWrites(std::uint32_t frameIndex) noexcept
{
	try
	{
		for (auto it = shadowCommandOwnership.begin(); it != shadowCommandOwnership.end();)
		{
			if (it->second.frameIndex == frameIndex && it->second.submitSerial == 0U)
			{
				// Release nonallocating depth reservations before shadow telemetry
				// cleanup can allocate and throw. An unsubmitted command owns no
				// GPU work, so its exact fixture token may safely retry next frame.
				lightingDepthEvidence.DiscardCommand(it->second.serial);
				staticShadowLifetime.DiscardUnsubmitted(it->second.serial);
				staticShadowDiscardedCommands[staticShadowDiscardCursor++ % staticShadowDiscardedCommands.size()] = it->second.serial;
				for (auto& entry : staticShadowEntries) if (!entry->layoutEstablished
					&& entry->initializationCommand == it->second.serial) {
					entry->layouts.fill(VK_IMAGE_LAYOUT_UNDEFINED); entry->initializationCommand = 0;
				}
				staticShadowPublicationLedger.Discard(it->second.serial);
				shadowPublicationLedger.Discard(it->second.serial);
				shadowValidationReadback.DiscardCommand(it->second.serial);
				it = shadowCommandOwnership.erase(it);
			}
			else ++it; // UI skips never revoke already accepted queue work.
		}
	}
	catch (const std::bad_alloc&) { ResetShadowPublication(false); }
}

void VulkanContext::Impl::CompleteShadowWrites(std::uint32_t frameIndex) noexcept
{
	if (frameIndex >= shadowSubmitReceiptsByFrame.size()) return;
	auto& receipts = shadowSubmitReceiptsByFrame[frameIndex];
	std::erase_if(receipts, [&](auto receipt) {
		staticShadowLifetime.CompleteSubmission(receipt);
		staticShadowCompletedSubmissions[staticShadowCompletionCursor++ % staticShadowCompletedSubmissions.size()] = receipt;
		try
		{
			shadowPublicationLedger.Complete(receipt, receipt);
			staticShadowPublicationLedger.Complete(receipt, receipt);
			shadowValidationReadback.CompleteSubmission(receipt, receipt);
			lightingDepthEvidence.CompleteSubmission(receipt, receipt);
			return true;
		}
		catch (const std::bad_alloc&) { return false; } // Retry on a later proven boundary.
	});
	std::erase_if(shadowCommandOwnership, [&](const auto& pair) {
		if (pair.second.frameIndex != frameIndex || pair.second.submitSerial == 0U) return false;
		staticShadowLifetime.CompleteSubmission(pair.second.submitSerial);
		if (std::find(staticShadowCompletedSubmissions.begin(), staticShadowCompletedSubmissions.end(), pair.second.submitSerial)
			== staticShadowCompletedSubmissions.end())
			staticShadowCompletedSubmissions[staticShadowCompletionCursor++ % staticShadowCompletedSubmissions.size()] = pair.second.submitSerial;
		try
		{
			// Covers accepted submits whose receipt-vector allocation failed. Repeating
			// a completed receipt for several commands is intentionally idempotent.
			shadowPublicationLedger.Complete(pair.second.submitSerial, pair.second.submitSerial);
			staticShadowPublicationLedger.Complete(pair.second.submitSerial, pair.second.submitSerial);
			shadowValidationReadback.CompleteSubmission(pair.second.submitSerial, pair.second.submitSerial);
			lightingDepthEvidence.CompleteSubmission(pair.second.submitSerial, pair.second.submitSerial);
			return true;
		}
		catch (const std::bad_alloc&) { return false; }
	});
	ReleaseCompletedStaticShadowEntries();
}

void VulkanContext::Impl::ResetShadowPublication(bool allocationReplaced) noexcept
{
	shadowPublicationLedger.Reset(renderSessionGeneration);
	staticShadowPublicationLedger.Reset(renderSessionGeneration);
	if (allocationReplaced) RetireStaticShadowEntries();
	try { shadowValidationReadback.Reset(renderSessionGeneration, deviceLost); }
	catch (const std::bad_alloc&) { /* Retain staging; completion/shutdown still owns its native lifetime. */ }
	// Shadow-content invalidation cannot cancel unrelated depth work. Device loss
	// is terminal and supplies the existing safe release proof for both owners.
	if (deviceLost) lightingDepthEvidence.Reset(renderSessionGeneration, true);
	shadowPhysicalTokenHints.clear();
	shadowCacheEntries.clear();
	if (deviceLost) shadowCommandOwnership.clear();
	// Ordinary edits invalidate observations but recorded primaries may still be
	// submitted by descriptor-mutation synchronization. Keep all command owners
	// until actual submission/completion or explicit proof that they were dropped.
	if (deviceLost) for (auto& receipts : shadowSubmitReceiptsByFrame) receipts.clear();
	// Zero after exhaustion fails closed rather than recycling an allocation ID.
	if (allocationReplaced && shadowAllocationGeneration != 0U) ++shadowAllocationGeneration;
}

// VulkanContext::EndFrame is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::EndFrame() {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// EndFrame may submit these slices. A later host write requires BeginFrame's
	// next fence proof, never the fact that a mirror previously succeeded.
	impl->lightingUploadFrameSlotSafe[impl->currentFrame] = false;
	if (!impl->initialized || impl->skipFrame || impl->deviceLost)
	{
		impl->DiscardRecordedShadowWrites(impl->currentFrame);
		return;
	}

	const auto endFrameStart = BackendClock::now();
	// Submit all recorded camera command buffers together so the swapchain is
	// presented once using the final camera's output.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
		impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
	for(const auto command : cmds)
	{
		const auto owner = impl->shadowCommandOwnership.find(command);
		if(owner == impl->shadowCommandOwnership.end() || owner->second.validationAbortKey.empty()
			|| owner->second.validationAbortKey == impl->lastShadowValidationAbortKey) continue;
		// The commands are fully recorded but have not reached vkQueueSubmit.
		// Drop the entire multi-view frame, preserving physical GPU contents while
		// revoking every recorded cache candidate. No native failure is simulated.
		impl->lastShadowValidationAbortKey = owner->second.validationAbortKey;
		// Each dropped primary gets its own actual recording receipt. Attach before
		// discard publishes Unavailable rows and releases their unsubmitted staging.
		for (const auto dropped : cmds) {
			const auto droppedOwner = impl->shadowCommandOwnership.find(dropped);
			if (droppedOwner == impl->shadowCommandOwnership.end() || droppedOwner->second.validationAbortKey.empty()) continue;
			TerraEngine::Renderer::RendererEfficiencyFixtureActionReceipt action;
			action.injection = TerraEngine::Renderer::RendererEfficiencyFixtureInjection::RecordedSubmitAbort;
			action.correlation = droppedOwner->second.validationAbortCorrelation;
			action.frameSerial = droppedOwner->second.validationFrameSerial; action.commandSerial = droppedOwner->second.serial;
			action.recorded = true; action.reason = "Actual fully recorded primary was dropped before vkQueueSubmit.";
			impl->shadowValidationReadback.AttachFixtureAction(action.commandSerial, action);
		}
		impl->DiscardRecordedShadowWrites(impl->currentFrame);
		impl->DiscardRecordedHiZWrites(impl->currentFrame);
		cmds.clear();
		impl->editorRenderSubmitted = false;
		break;
	}
#endif
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (cmds.empty()) {
		impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
				impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
				impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
			// A failed protected submission revokes host-mirror reuse proof. Keep
			// the last successful bytes for diagnostics and retry a full slice.
			impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
		impl->ConfirmShadowWritesSubmitted(impl->currentFrame, cmds);
		if (impl->currentFrame < impl->pendingPrimaryCommandBuffersSubmitted.size()) {
			impl->pendingPrimaryCommandBuffersSubmitted[impl->currentFrame] = 1u;
		}
	} else {
		const auto resetFenceStart = BackendClock::now();
		const VkResult resetResult = vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
		if (resetResult != VK_SUCCESS) {
			impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
			impl->ResetLightingUploadMirrors();
			impl->DiscardRecordedShadowWrites(impl->currentFrame);
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
		impl->ConfirmShadowWritesSubmitted(impl->currentFrame, cmds);
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
