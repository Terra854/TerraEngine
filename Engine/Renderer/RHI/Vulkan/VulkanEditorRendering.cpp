// VulkanEditorRendering.cpp — Editor presentation and offscreen viewport resources
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"


TerraEngine::Renderer::Vulkan::EditorPresentResult VulkanContext::PresentEditor(
	std::function<void()> prepareUI,
	std::function<void(void*)> renderUI) {
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	using namespace TerraEngine::Renderer::Vulkan;
	EditorPresentResult result{};
	if (!impl->editorMode || !impl->initialized || impl->deviceLost) {
		result.outcome = impl->deviceLost ? EditorPresentOutcome::DeviceLost : EditorPresentOutcome::Skipped;
		return result;
	}
	if (impl->currentFrame >= impl->editorCommandPools.size()
		|| impl->currentFrame >= impl->editorCommandBuffers.size()
		|| impl->editorCommandBuffers[impl->currentFrame] == VK_NULL_HANDLE) {
		result.outcome = EditorPresentOutcome::RecoverableFailure;
		result.stage = EditorUiFrameStage::AllocateCommandBuffer;
		result.nativeResult = static_cast<std::int32_t>(VK_ERROR_INITIALIZATION_FAILED);
		return result;
	}

	// PresentEditor can run without RenderSystem::BeginFrame while the project
	// hub is open or a scene has no camera. Wait for this UI frame slot directly,
	// then reset its dedicated pool so command-buffer storage remains bounded.
	const VkResult uiFenceWaitResult = vkWaitForFences(
		impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);
	if (impl->MarkDeviceLost(uiFenceWaitResult, "editor UI command pool fence wait")) {
		result.outcome = EditorPresentOutcome::DeviceLost;
		return result;
	}
	if (uiFenceWaitResult != VK_SUCCESS) {
		result.outcome = EditorPresentOutcome::RecoverableFailure;
		result.stage = EditorUiFrameStage::AllocateCommandBuffer;
		result.nativeResult = static_cast<std::int32_t>(uiFenceWaitResult);
		return result;
	}
	const VkResult uiPoolResetResult = vkResetCommandPool(
		impl->device, impl->editorCommandPools[impl->currentFrame], 0);
	if (impl->MarkDeviceLost(uiPoolResetResult, "editor UI command pool reset")) {
		result.outcome = EditorPresentOutcome::DeviceLost;
		return result;
	}
	if (uiPoolResetResult != VK_SUCCESS) {
		result.outcome = EditorPresentOutcome::RecoverableFailure;
		result.stage = EditorUiFrameStage::AllocateCommandBuffer;
		result.nativeResult = static_cast<std::int32_t>(uiPoolResetResult);
		return result;
	}

	auto fail = [&](EditorUiFrameStage stage, VkResult nativeResult, std::string_view operation) {
		result.stage = stage;
		result.nativeResult = static_cast<std::int32_t>(nativeResult);
		if (impl->MarkDeviceLost(nativeResult, operation)) {
			impl->presentationLifecycle.MarkDeviceLost();
			impl->editorCompletionTracker.MarkDeviceLost();
			result.outcome = EditorPresentOutcome::DeviceLost;
		} else {
			result.outcome = EditorPresentOutcome::RecoverableFailure;
			TERRAENGINE_LOG(LogLevel::Error, "Editor UI frame stopped at "
				<< EditorUiFrameStageName(stage) << ": " << nativeResult);
		}
		return result;
	};

	auto consumeOutstandingSemaphores = [&](bool acquired) -> VkResult {
		std::array<VkSemaphore, 2> waits{};
		std::array<VkPipelineStageFlags, 2> stages{
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
		};
		std::uint32_t waitCount = 0;
		if (acquired) waits[waitCount++] = impl->imageAvailableSemaphores[impl->currentFrame];
		if (impl->editorRenderSubmitted) waits[waitCount++] = impl->renderFinishedSemaphores[impl->currentFrame];
		if (waitCount == 0) return VK_SUCCESS;

		VkSubmitInfo drain{};
		drain.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		drain.waitSemaphoreCount = waitCount;
		drain.pWaitSemaphores = waits.data();
		drain.pWaitDstStageMask = stages.data();
		VkResult drainResult = impl->SubmitGraphicsQueue(1, &drain, VK_NULL_HANDLE);
		if (drainResult == VK_SUCCESS) drainResult = impl->WaitGraphicsQueueIdle();
		if (drainResult == VK_SUCCESS) impl->editorRenderSubmitted = false;
		return drainResult;
	};

	// Swapchain acquisition is intentionally bounded so a busy compositor cannot
	// freeze the editor. When that retryable timeout follows an offscreen render,
	// consume its binary semaphore with this slot's fence and move on to the next
	// slot. The next reuse waits only for this exact submission instead of forcing
	// the entire graphics queue idle in BeginFrame.
	auto deferSkippedEditorRenderCompletion = [&]() -> VkResult {
		if (!impl->editorRenderSubmitted) return VK_SUCCESS;

		TERRAENGINE_PROFILE_SCOPE(
			"Editor Present: Defer Skipped Render Completion",
			TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		const std::uint32_t frameSlot = impl->currentFrame;
		const VkResult resetResult = vkResetFences(
			impl->device, 1, &impl->inFlightFences[frameSlot]);
		if (resetResult != VK_SUCCESS) {
			// Failed fence admission revokes lighting reuse, even when later UI
			// recovery repairs the native fence and allows this slot to retry.
			impl->ResetLightingUploadMirrors();
			return resetResult;
		}

		VkSemaphore renderSemaphore = impl->renderFinishedSemaphores[frameSlot];
		VkPipelineStageFlags renderStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo completionSubmit{};
		completionSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		completionSubmit.waitSemaphoreCount = 1;
		completionSubmit.pWaitSemaphores = &renderSemaphore;
		completionSubmit.pWaitDstStageMask = &renderStage;
		const VkResult submitResult = impl->SubmitGraphicsQueue(
			1, &completionSubmit, impl->inFlightFences[frameSlot]);
		if (submitResult != VK_SUCCESS) {
			impl->ResetLightingUploadMirrors();
			// A failed submit consumes no semaphore and leaves the reset fence
			// unsignaled. Keep the rare recovery path conservative: consume the
			// signal synchronously, then replace the unusable fence before retrying.
			if (submitResult == VK_ERROR_DEVICE_LOST) return submitResult;
			const VkResult drainResult = consumeOutstandingSemaphores(false);
			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkFence replacement = VK_NULL_HANDLE;
			const VkResult repairResult = vkCreateFence(
				impl->device, &fenceInfo, nullptr, &replacement);
			if (repairResult == VK_SUCCESS) {
				vkDestroyFence(impl->device, impl->inFlightFences[frameSlot], nullptr);
				impl->inFlightFences[frameSlot] = replacement;
			}
			if (drainResult != VK_SUCCESS) return drainResult;
			if (repairResult != VK_SUCCESS) return repairResult;
			return submitResult;
		}

		// The queued wait now owns the render-finished signal and the slot fence
		// proves when its command buffers are reusable. No UI submission serial is
		// published because skipped ImGui draw data did not reach the GPU.
		impl->editorRenderSubmitted = false;
		impl->currentFrame = (frameSlot + 1) % impl->MAX_FRAMES_IN_FLIGHT;
		return VK_SUCCESS;
	};

	// BeginFrame waited this slot's previous UI fence before recording began.
	// Do not reset or free the pool here: EndFrame may have just submitted
	// Scene/Game command buffers from it, and the UI submission below extends
	// their semaphore chain to the fence that the next BeginFrame will wait.

	if (impl->framebufferResized) {
		if (!impl->ClaimFramebufferResizeRebuild()) {
			const VkResult completionResult = deferSkippedEditorRenderCompletion();
			if (completionResult != VK_SUCCESS) {
				return fail(EditorUiFrameStage::DrainSemaphore, completionResult,
					"deferred editor resize render completion");
			}
			result.outcome = EditorPresentOutcome::Skipped;
			return result;
		}
		const VkResult drainResult = consumeOutstandingSemaphores(false);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "editor resize semaphore drain");
		}
		impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		return result;
	}

	std::uint32_t imageIndex = 0;
	VkResult acquireResult = VK_SUCCESS;
	const VkFence unextendedAcquireProof = impl->PrepareUnextendedAcquireProof();
	{
		// Swapchain acquisition can wait for the presentation engine to release an
		// image. Editor interaction must not inherit an unbounded compositor wait:
		// allow one millisecond, then retry on a later frame. BeginFrame already
		// drains a stranded editor-render semaphore when this UI pass is skipped.
		TERRAENGINE_PROFILE_SCOPE("Editor Present: Acquire Image", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		acquireResult = vkAcquireNextImageKHR(
			impl->device, impl->swapchain, 1'000'000ULL,
			impl->imageAvailableSemaphores[impl->currentFrame], unextendedAcquireProof, &imageIndex);
	}
	impl->ResolveUnextendedAcquireProof(unextendedAcquireProof, acquireResult, imageIndex);
	if (acquireResult == VK_TIMEOUT || acquireResult == VK_NOT_READY) {
		// No image or acquire semaphore was published. Retire any offscreen render
		// asynchronously through its frame-slot fence, then retry presentation from
		// another slot without a queue-wide idle wait.
		const VkResult completionResult = deferSkippedEditorRenderCompletion();
		if (completionResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, completionResult,
				"deferred editor acquire render completion");
		}
		result.outcome = EditorPresentOutcome::Skipped;
		result.stage = EditorUiFrameStage::AcquireImage;
		result.nativeResult = static_cast<std::int32_t>(acquireResult);
		return result;
	}
	if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
		const VkResult drainResult = consumeOutstandingSemaphores(false);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "editor outdated acquire semaphore drain");
		}
		impl->RequestOutOfDateRecovery();
		if (impl->ClaimFramebufferResizeRebuild()) impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		result.stage = EditorUiFrameStage::AcquireImage;
		result.nativeResult = static_cast<std::int32_t>(acquireResult);
		return result;
	}
	if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
		return fail(EditorUiFrameStage::AcquireImage, acquireResult, "editor swapchain image acquire");
	}

	AcquiredPresentationImage acquired{
		impl->currentFrame,
		imageIndex,
		impl->presentationLifecycle.Generation()
	};
	impl->acquiredPresentationImage = acquired;
	result.acquiredImage = acquired;
	if (!impl->presentationLifecycle.Validate(acquired)
		|| imageIndex >= impl->framebuffers.size()
		|| imageIndex >= impl->uiSemaphores.size()) {
		const VkResult drainResult = consumeOutstandingSemaphores(true);
		if (drainResult != VK_SUCCESS) {
			return fail(EditorUiFrameStage::DrainSemaphore, drainResult, "invalid editor image semaphore drain");
		}
		impl->presentationLifecycle.RequestRecreation();
		result.outcome = EditorPresentOutcome::RecreationRequested;
		result.stage = EditorUiFrameStage::AcquireImage;
		return result;
	}

	// Each frame slot owns one command buffer for the lifetime of its pool. Reuse
	// it after the fence and pool reset above instead of asking the Vulkan driver
	// for a fresh allocation on every editor frame.
	VkCommandBuffer uiCommandBuffer = impl->editorCommandBuffers[impl->currentFrame];

	try {
		// Dynamic ImGui font textures use a backend-owned transfer command pool
		// and hidden queue submit/wait calls. Process those operations only after
		// this frame slot's fence completed, and before beginning the swapchain UI
		// command buffer, so mouse-triggered glyph uploads cannot overlap an active
		// render pass or retire descriptors still referenced by this frame slot.
		TERRAENGINE_PROFILE_SCOPE("Editor Present: Prepare UI Textures", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		std::lock_guard<std::mutex> queueLock(impl->queueAccessMutex);
		if (prepareUI) prepareUI();
	} catch (...) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::RecordDrawData, VK_ERROR_INITIALIZATION_FAILED,
			"editor UI backend texture preparation");
	}

	VkCommandBufferBeginInfo begin{};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	const VkResult beginResult = vkBeginCommandBuffer(uiCommandBuffer, &begin);
	if (beginResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::BeginCommandBuffer, beginResult, "editor UI command buffer begin");
	}
	impl->RecordDiagnosticCheckpoint(uiCommandBuffer, "editor.ui.begin");
	if (impl->validationLayersEnabled && vkSetDebugUtilsObjectNameEXT != nullptr) {
		VkDebugUtilsObjectNameInfoEXT name{};
		name.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
		name.objectType = VK_OBJECT_TYPE_COMMAND_BUFFER;
		name.objectHandle = reinterpret_cast<std::uint64_t>(uiCommandBuffer);
		name.pObjectName = "Editor UI Present Command Buffer";
		const VkResult namingResult = vkSetDebugUtilsObjectNameEXT(impl->device, &name);
		if (namingResult != VK_SUCCESS) {
			consumeOutstandingSemaphores(true);
			return fail(EditorUiFrameStage::DiagnosticNaming, namingResult, "editor UI diagnostic naming");
		}
	}

	VkRenderPassBeginInfo renderPass{};
	renderPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPass.renderPass = impl->renderPass;
	renderPass.framebuffer = impl->framebuffers[imageIndex];
	renderPass.renderArea.extent = impl->swapchainExtent;
	VkClearValue clear{};
	clear.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
	renderPass.clearValueCount = 1;
	renderPass.pClearValues = &clear;

	try {
		TERRAENGINE_PROFILE_SCOPE("Editor Present: Record UI Draw Data", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		vkCmdBeginRenderPass(uiCommandBuffer, &renderPass, VK_SUBPASS_CONTENTS_INLINE);
		{
			// Dear ImGui records descriptor-backed draw calls here. The queue
			// mutex is also the editor's protected descriptor/presentation boundary.
			std::lock_guard<std::mutex> queueLock(impl->queueAccessMutex);
			if (renderUI) renderUI(uiCommandBuffer);
		}
		vkCmdEndRenderPass(uiCommandBuffer);
	} catch (...) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::RecordDrawData, VK_ERROR_INITIALIZATION_FAILED,
			"editor UI backend draw recording");
	}
	impl->RecordDiagnosticCheckpoint(uiCommandBuffer, "editor.ui.end");
	const VkResult endResult = vkEndCommandBuffer(uiCommandBuffer);
	if (endResult != VK_SUCCESS) {
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::EndCommandBuffer, endResult, "editor UI command buffer end");
	}

	std::array<VkSemaphore, 2> waits{
		impl->imageAvailableSemaphores[impl->currentFrame],
		VK_NULL_HANDLE
	};
	std::array<VkPipelineStageFlags, 2> waitStages{
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
	};
	std::uint32_t waitCount = 1;
	if (impl->editorRenderSubmitted) {
		waits[waitCount++] = impl->renderFinishedSemaphores[impl->currentFrame];
	}
	VkSemaphore presentReady = impl->uiSemaphores[imageIndex];
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = waitCount;
	submit.pWaitSemaphores = waits.data();
	submit.pWaitDstStageMask = waitStages.data();
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &uiCommandBuffer;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &presentReady;

	const VkResult resetFenceResult = vkResetFences(
		impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
	if (resetFenceResult != VK_SUCCESS) {
		impl->ResetLightingUploadMirrors();
		consumeOutstandingSemaphores(true);
		return fail(EditorUiFrameStage::ResetFrameFence, resetFenceResult, "editor UI frame fence reset");
	}
	VkResult submitResult = VK_SUCCESS;
	{
		TERRAENGINE_PROFILE_SCOPE("Editor Present: Submit UI", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		submitResult = impl->SubmitGraphicsQueue(
			1, &submit, impl->inFlightFences[impl->currentFrame]);
	}
	if (submitResult != VK_SUCCESS) {
		impl->ResetLightingUploadMirrors();
		// Failed submission consumes no waits. Drain them, then replace the now
		// unsignaled fence so the frame slot remains reusable.
		const VkResult drainResult = consumeOutstandingSemaphores(true);
		if (drainResult == VK_SUCCESS) {
			VkFenceCreateInfo fenceInfo{};
			fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			VkFence replacement = VK_NULL_HANDLE;
			const VkResult repairResult = vkCreateFence(impl->device, &fenceInfo, nullptr, &replacement);
			if (repairResult == VK_SUCCESS) {
				vkDestroyFence(impl->device, impl->inFlightFences[impl->currentFrame], nullptr);
				impl->inFlightFences[impl->currentFrame] = replacement;
			} else {
				return fail(EditorUiFrameStage::RepairFence, repairResult, "editor UI frame fence repair");
			}
		}
		return fail(EditorUiFrameStage::Submit, submitResult, "editor UI queue submit");
	}

	result.submissionSerial = impl->editorCompletionTracker.RecordSubmission(impl->currentFrame);
	result.outcome = EditorPresentOutcome::Submitted;
	result.stage = EditorUiFrameStage::Submit;
	impl->editorRenderSubmitted = false;

	VkPresentInfoKHR present{};
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &presentReady;
	present.swapchainCount = 1;
	present.pSwapchains = &impl->swapchain;
	present.pImageIndices = &imageIndex;
	VkSwapchainPresentFenceInfoKHR presentCompletionInfo{};
	const bool presentCompletionFencePrepared = impl->PreparePresentationCompletionFence(
		imageIndex, present, presentCompletionInfo);
	if (impl->presentationFencesEnabled && !presentCompletionFencePrepared) {
		impl->presentationFencesEnabled = false;
	}
	VkResult presentResult = VK_SUCCESS;
	{
		// FIFO presentation may apply back-pressure independently of image acquire;
		// record it separately so responsiveness gates can diagnose the exact wait.
		TERRAENGINE_PROFILE_SCOPE("Editor Present: Queue Present", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		presentResult = impl->PresentSwapchain(&present);
	}
	impl->ResolveUnenqueuedPresentationFence(presentCompletionFencePrepared, presentResult);
	impl->RecordUnextendedPresent(imageIndex, presentResult);
	result.stage = EditorUiFrameStage::Present;
	result.nativeResult = static_cast<std::int32_t>(presentResult);
	if ((presentResult == VK_SUCCESS || presentResult == VK_SUBOPTIMAL_KHR)
		&& impl->editorPresentationReadyLoggedGeneration != impl->presentationLifecycle.Generation()) {
		// Queue-present success confirms the editor submitted a real native
		// presentation operation for this generation. Log once so automation does
		// not mistake a resize callback or image acquisition for visual readiness.
		impl->editorPresentationReadyLoggedGeneration = impl->presentationLifecycle.Generation();
		TERRAENGINE_LOG(LogLevel::Info, "[Vulkan] Editor presentation ready generation="
			<< impl->presentationLifecycle.Generation() << " extent="
			<< impl->swapchainExtent.width << "x" << impl->swapchainExtent.height);
	}
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
		impl->RequestOutOfDateRecovery();
	} else if (presentResult == VK_SUBOPTIMAL_KHR) {
		impl->RequestSuboptimalRecoveryIfNeeded();
	}
	if (impl->framebufferResized && impl->ClaimFramebufferResizeRebuild()) {
		impl->RecreateSwapChain();
		result.outcome = EditorPresentOutcome::RecreationRequested;
	} else if (presentResult != VK_SUCCESS && presentResult != VK_SUBOPTIMAL_KHR) {
		return fail(EditorUiFrameStage::Present, presentResult, "editor queue present");
	}

	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	return result;
}

TerraEngine::Renderer::Vulkan::EditorSubmissionProgress VulkanContext::GetEditorSubmissionProgress() const {
	if (!impl) return {};
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	return impl->editorCompletionTracker.Snapshot();
}

std::int32_t VulkanContext::FreeEditorDescriptorSet(void* descriptorSet) {
	if (!impl || descriptorSet == nullptr) return static_cast<std::int32_t>(VK_SUCCESS);
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (impl->deviceLost) return static_cast<std::int32_t>(VK_ERROR_DEVICE_LOST);
	std::lock_guard<std::recursive_mutex> descriptorLock(impl->descriptorHostAccessMutex);
	VkDescriptorSet descriptor = reinterpret_cast<VkDescriptorSet>(descriptorSet);
	return static_cast<std::int32_t>(vkFreeDescriptorSets(
		impl->device, impl->editorDescriptorPool, 1, &descriptor));
}

void VulkanContext::AcknowledgeViewportTextureGeneration(
	ViewType view, std::uint64_t generation, std::uint64_t finalUseSerial) {
	if (!impl) return;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	for (auto& retired : impl->retiredOffscreenResources) {
		if (retired.editorView == view && retired.replacementGeneration == generation) {
			retired.editorDescriptorAcknowledged = true;
			retired.retireAfterSerial = std::max(retired.retireAfterSerial, finalUseSerial);
		}
	}
	impl->DrainRetiredOffscreenResources();
}

// Impl Extensions

void VulkanContext::Impl::InitEditorMode() {
	if (editorMode) return;
	editorMode = true;

	// Rebuild the swapchain render pass after flipping editorMode so the ImGui
	// presentation pass does not inherit the runtime VRS attachment.
	RecreateSwapChain();
	CreateOffscreenResources(sceneView);
	CreateOffscreenResources(gameView);
}


void VulkanContext::Impl::DestroyOffscreenResourceSet(OffscreenResources& resources)
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if (resources.image != VK_NULL_HANDLE && resources.image == readbackValidationSourceImage) {
		readbackValidationSnapshot.sourceImageDestroyObserved = true;
		readbackValidationSnapshot.sourceImageDestroyedBeforeRetirement |= HasInFlightReadbackForImage(resources.image);
		RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::SourceImageDestroyed);
	}
#endif
	if (resources.sampler != VK_NULL_HANDLE) vkDestroySampler(device, resources.sampler, nullptr);
	if (resources.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, resources.framebuffer, nullptr);
	DestroyConventionalVisibilityIdentityAttachment(resources.visibilityIdentity);
	if (resources.view != VK_NULL_HANDLE) vkDestroyImageView(device, resources.view, nullptr);
	if (resources.depthView != VK_NULL_HANDLE) vkDestroyImageView(device, resources.depthView, nullptr);
	if (resources.image != VK_NULL_HANDLE && resources.allocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, resources.image, resources.allocation);
	}
	if (resources.depthImage != VK_NULL_HANDLE && resources.depthAllocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, resources.depthImage, resources.depthAllocation);
	}
	resources = {};
}

void VulkanContext::Impl::DrainRetiredOffscreenResources()
{
	const std::uint64_t completedSerial = editorCompletionTracker.Snapshot().completedSerial;
	auto iterator = retiredOffscreenResources.begin();
	while (iterator != retiredOffscreenResources.end()) {
		const bool readbackComplete = !HasInFlightReadbackForImage(iterator->resources.image)
			&& !HasInFlightReadbackForImage(iterator->resources.visibilityIdentity.image);
		if (!iterator->editorDescriptorAcknowledged
			|| iterator->retireAfterSerial > completedSerial || !readbackComplete) {
			++iterator;
			continue;
		}
		DestroyOffscreenResourceSet(iterator->resources);
		iterator = retiredOffscreenResources.erase(iterator);
	}
}


void VulkanContext::Impl::CreateOffscreenResources(OffscreenResources& res, VkExtent2D requestedExtent) {
	// Keep the allocation/replacement cost visible beneath the caller's broader
	// target-check marker. Driver allocation stalls can otherwise be mistaken for
	// ordinary per-frame command recording.
	TERRAENGINE_PROFILE_SCOPE("Vulkan: Create Offscreen Resources", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	const std::uint64_t replacementGeneration = res.generation + 1u;
	OffscreenResources candidate{};
	candidate.generation = replacementGeneration;
	auto DestroyOffscreenResourceSet = [&]() {
		this->DestroyOffscreenResourceSet(candidate);
		// Preserve the logical identity only in the temporary candidate. The
		// previously published target remains untouched on creation failure.
		candidate.generation = replacementGeneration;
	};
	VkExtent2D targetExtent = requestedExtent;
	if (targetExtent.width == 0u)
	{
		targetExtent.width = swapchainExtent.width;
	}
	if (targetExtent.height == 0u)
	{
		targetExtent.height = swapchainExtent.height;
	}
	if (targetExtent.width == 0u || targetExtent.height == 0u)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Skipping editor offscreen resource creation because the target extent is zero.");
		return;
	}


	if (offscreenColorFormat == VK_FORMAT_UNDEFINED) {
			// Scene and game views are sampled by the editor after rendering and may
			// be copied into MCP capture staging buffers. The selected format must
			// support all three operations or readback would rely on undefined driver
			// behavior.
			const VkFormatFeatureFlags requiredFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
		const std::array<VkFormat, 3> candidates = {
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_FORMAT_R8G8B8A8_SRGB,
			VK_FORMAT_R8G8B8A8_UNORM
		};


		for (VkFormat candidate : candidates) {
			VkFormatProperties properties{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &properties);

			if ((properties.optimalTilingFeatures & requiredFeatures) == requiredFeatures) {
				offscreenColorFormat = candidate;
				break;
			}
		}


		if (offscreenColorFormat == VK_FORMAT_UNDEFINED) {
				TERRAENGINE_LOG(LogLevel::Warning, "No sampled, copyable color-attachment format reported for editor offscreen rendering; falling back to R8G8B8A8_UNORM.");
				offscreenColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
			}
	}


	if (offscreenRenderPass == VK_NULL_HANDLE) {
		if (UsesVrsAttachment()) {
			VkAttachmentDescription2 colorAttachment{};
			colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			colorAttachment.format = offscreenColorFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentReference2 colorAttachmentRef{};
		colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription2 identityAttachment{};
		identityAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		identityAttachment.format = VK_FORMAT_R32G32B32A32_UINT;
		identityAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		identityAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		identityAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		identityAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		identityAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		identityAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		identityAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference2 identityAttachmentRef{};
		identityAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		identityAttachmentRef.attachment = 1;
		identityAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		const std::array<VkAttachmentReference2, 2> colorAttachmentRefs = {
			colorAttachmentRef, identityAttachmentRef };

			VkAttachmentDescription2 depthAttachment{};
			depthAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			// Hi-Z is generated after the editor render pass, so depth must be
			// stored even though the presented viewport only shows color.
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// Offscreen depth is explicitly initialized and restored to attachment
			// layout after Hi-Z/AO reads. Preserve that real layout here so a new
			// frame does not introduce an unnecessary UNDEFINED layout transition.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference2 depthAttachmentRef{};
			depthAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		depthAttachmentRef.attachment = 2;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription2 vrsAttachment{};
			vrsAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			vrsAttachment.format = VK_FORMAT_R8_UINT;
			vrsAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			vrsAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
			vrsAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			vrsAttachment.initialLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
			vrsAttachment.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;

			VkAttachmentReference2 vrsAttachmentRef{};
			vrsAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
		vrsAttachmentRef.attachment = 3;
			vrsAttachmentRef.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;

			VkFragmentShadingRateAttachmentInfoKHR vrsInfo{};
			vrsInfo.sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
			vrsInfo.pFragmentShadingRateAttachment = &vrsAttachmentRef;
			vrsInfo.shadingRateAttachmentTexelSize = vrs.texelSize;

			VkSubpassDescription2 subpass{};
			subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorAttachmentRefs.size());
		subpass.pColorAttachments = colorAttachmentRefs.data();
			subpass.pDepthStencilAttachment = &depthAttachmentRef;
			subpass.pNext = &vrsInfo;

			std::array<VkSubpassDependency2, 1> dependencies{};
			dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			std::array<VkAttachmentDescription2, 4> attachments = {
				colorAttachment, identityAttachment, depthAttachment, vrsAttachment };
			VkRenderPassCreateInfo2 renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();


			if (vkCreateRenderPass2(device, &renderPassInfo, nullptr, &offscreenRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create offscreen render pass 2!");
				return;
			}
		} else {
			// Create Render Pass using standard RenderPass API (max compatibility)
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = offscreenColorFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription identityAttachment{};
			identityAttachment.format = VK_FORMAT_R32G32B32A32_UINT;
			identityAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			identityAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			identityAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			identityAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			identityAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			identityAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			identityAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentReference identityAttachmentRef{};
			identityAttachmentRef.attachment = 1;
			identityAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			const std::array<VkAttachmentReference, 2> colorAttachmentRefs = {
				colorAttachmentRef, identityAttachmentRef };

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			// Hi-Z is generated after the editor render pass, so depth must be
			// stored even though the presented viewport only shows color.
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			// The editor restores this persistent image after compute reads. Match
			// that layout so loadOp=CLEAR is ordered without an implicit discard
			// transition racing the prior frame's depth write.
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 2;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = static_cast<std::uint32_t>(colorAttachmentRefs.size());
			subpass.pColorAttachments = colorAttachmentRefs.data();
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			// Scene/Game targets are sampled by ImGui and their depth images are read
			// by compute before being restored. Cover both prior consumers and the
			// previous depth restore before this frame clears either attachment.
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			dependencies[1].srcSubpass = 0;
			dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
			// Publish color for ImGui and depth for Hi-Z/AO/capture operations that
			// immediately follow the render pass in the recorded command buffer.
			dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
				| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
				| VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
				| VK_PIPELINE_STAGE_TRANSFER_BIT;
			dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
				| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
			dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT
				| VK_ACCESS_TRANSFER_READ_BIT;
			dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

			std::array<VkAttachmentDescription, 3> attachments = {
				colorAttachment, identityAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
			renderPassInfo.pDependencies = dependencies.data();


			if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create offscreen render pass!");
				return;
			}
		}
	}

	// Create Image
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = targetExtent.width;
	imageInfo.extent.height = targetExtent.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = offscreenColorFormat;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &candidate.image, &candidate.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen color image!");
		DestroyOffscreenResourceSet();
		return;
	}
	if (!CreateConventionalVisibilityIdentityAttachment(targetExtent,
		candidate.visibilityIdentity)) {
		TERRAENGINE_LOG(LogLevel::Error,
			"failed to create the editor conventional visibility identity attachment!");
		DestroyOffscreenResourceSet();
		return;
	}
	vmaSetAllocationName(allocator, candidate.allocation, "Offscreen Render Target Image");
	// Editor scene/game views are registered with ImGui as shader-readable
	// textures immediately after creation. Transition the image up front so a
	// skipped or delayed first scene render cannot leave ImGui sampling an
	// image that still has Vulkan's UNDEFINED layout.
	TransitionImageLayout(candidate.image, offscreenColorFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// Create View
	candidate.view = CreateImageView(candidate.image, offscreenColorFormat, VK_IMAGE_ASPECT_COLOR_BIT);
	if (candidate.view == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen color view!");
		DestroyOffscreenResourceSet();
		return;
	}

	// Create a depth attachment owned by this editor view. Scene View and Game
	// View command buffers are recorded separately and submitted together, so
	// sharing one depth image lets one view clear/write the depth that another
	// view later uses for Hi-Z. Keeping depth per view makes the offscreen
	// framebuffer match the per-view Hi-Z history.
	const VkFormat offscreenDepthFormat = (depthFormat != VK_FORMAT_UNDEFINED) ? depthFormat : FindDepthFormat();
	VkImageCreateInfo depthImageInfo{};
	depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
	depthImageInfo.extent.width = targetExtent.width;
	depthImageInfo.extent.height = targetExtent.height;
	depthImageInfo.extent.depth = 1u;
	depthImageInfo.mipLevels = 1u;
	depthImageInfo.arrayLayers = 1u;
	depthImageInfo.format = offscreenDepthFormat;
	depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	depthImageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo depthAllocInfo{};
	depthAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	if (vmaCreateImage(allocator, &depthImageInfo, &depthAllocInfo, &candidate.depthImage, &candidate.depthAllocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen depth image!");
		DestroyOffscreenResourceSet();
		return;
	}
	vmaSetAllocationName(allocator, candidate.depthAllocation, "Editor Offscreen Depth Image");
	candidate.depthView = CreateImageView(candidate.depthImage, offscreenDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	if (candidate.depthView == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen depth view!");
		DestroyOffscreenResourceSet();
		return;
	}
	TransitionImageLayout(candidate.depthImage, offscreenDepthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

	// Create Framebuffer
	std::vector<VkImageView> fbAttachments = {
		candidate.view,
		candidate.visibilityIdentity.view,
		candidate.depthView
	};

	if (UsesVrsAttachment()) {
		fbAttachments.push_back(vrs.view);
	}

	VkFramebufferCreateInfo framebufferInfo{};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = offscreenRenderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
	framebufferInfo.pAttachments = fbAttachments.data();
	framebufferInfo.width = targetExtent.width;
	framebufferInfo.height = targetExtent.height;
	framebufferInfo.layers = 1;

	if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &candidate.framebuffer) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen framebuffer!");
		DestroyOffscreenResourceSet();
		return;
	}

	// Create Sampler
	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_LINEAR;
	samplerInfo.minFilter = VK_FILTER_LINEAR;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

	if (vkCreateSampler(device, &samplerInfo, nullptr, &candidate.sampler) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor offscreen sampler!");
		DestroyOffscreenResourceSet();
		return;
	}
	candidate.extent = targetExtent;
	OffscreenResources previous = std::move(res);
	res = std::move(candidate);
	const bool previousWasAllocated = previous.image != VK_NULL_HANDLE
		|| previous.depthImage != VK_NULL_HANDLE || previous.framebuffer != VK_NULL_HANDLE;
	if (previousWasAllocated) {
		RetiredOffscreenResourceSet retired{};
		retired.resources = std::move(previous);
		retired.replacementGeneration = replacementGeneration;
		retired.retireAfterSerial = editorCompletionTracker.Snapshot().lastSubmittedSerial;
		if (&res == &sceneView) retired.editorView = VulkanContext::ViewType::Scene;
		else if (&res == &gameView) retired.editorView = VulkanContext::ViewType::Game;
		retired.editorDescriptorAcknowledged = !retired.editorView.has_value();
		retiredOffscreenResources.push_back(std::move(retired));
	}
	DrainRetiredOffscreenResources();
}


void VulkanContext::Impl::RecreateOffscreenResources() {
	if (deviceLost) {
		return;
	}
	// Each target publishes transactionally and retires through completed UI
	// serials/readback fences, so ordinary viewport recreation never idles the device.
	CreateOffscreenResources(sceneView);
	CreateOffscreenResources(gameView);
	if (captureView.image != VK_NULL_HANDLE || captureView.framebuffer != VK_NULL_HANDLE)
	{
		// Keep the dedicated MCP capture target at its requested extent across
		// editor resizes instead of snapping it back to the swapchain extent.
		CreateOffscreenResources(captureView, captureView.extent);
	}
}


void VulkanContext::Impl::ClearResources() {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	std::lock_guard<std::recursive_mutex> descriptorLock(descriptorHostAccessMutex);
	if (!device) return;
	const VkResult idleResult = WaitDeviceIdle();
	if (MarkDeviceLost(idleResult, "clear Vulkan resources device idle")) {
		return;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Vulkan resource clear because the device did not become idle: " << idleResult);
		return;
	}
	// Idle protects previously submitted work, but live descriptor sets must also
	// stop naming these images and alias samplers before their owners are freed.
	// Defaults 0-2 survive this clear. Every allocated runtime slot, including
	// environment slots 3-5 and already-free slots, is reset to the readable white
	// owner before the heap is reused. Keep all owners and indices on rejection.
	if (nextTextureIndex < 3U || nextTextureIndex > MAX_BINDLESS_TEXTURES) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Vulkan resource clear because the texture heap extent is invalid.");
		return;
	}
	std::vector<std::uint32_t> retiredTextureSlots;
	try {
		retiredTextureSlots.reserve(nextTextureIndex - 3U);
		for (std::uint32_t index = 3U; index < nextTextureIndex; ++index)
			retiredTextureSlots.push_back(index);
	} catch (const std::bad_alloc&) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Vulkan resource clear because descriptor retirement preparation could not allocate.");
		return;
	}
	if (!RetireGlobalTextureDescriptorSlotsAfterIdle(retiredTextureSlots)) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping Vulkan resource clear because runtime texture descriptors could not retire safely.");
		return;
	}
	// The fallback writes above removed every alias reference from every frame
	// set. Drop alias lookups before releasing their shared sampler owners, so a
	// future material preparation must publish a fresh slot after heap reset.
	sampledTextureAliases.clear();
	for (auto& [key, cachedSampler] : textureSamplerCache) {
		(void)key;
		if (cachedSampler.handle != VK_NULL_HANDLE)
			vkDestroySampler(device, cachedSampler.handle, nullptr);
	}
	textureSamplerCache.clear();
	textureViewGenerations.clear();
	// The protected idle above permits teardown. Invalidate depth ownership before
	// any mesh or shadow image is destroyed, including partial teardown failures.
	ResetShadowPublication(true);
	DestroyShadowAuthoredPagePayloads(); // The existing device-idle proof covers every page buffer.
	ReleaseAmbiguousUploadsAfterIdle();


	for (auto& [id, mesh] : meshCache) {
		if (!mesh.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(mesh.descriptorSetsByLayout.size());

			for (const auto& [layout, set] : mesh.descriptorSetsByLayout) {

				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}

			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			mesh.descriptorSetsByLayout.clear();
		}

		vmaDestroyBuffer(allocator, mesh.indexBuffer.buffer, mesh.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshShaderVertexBuffer.buffer, mesh.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletVertexRemapBuffer.buffer, mesh.meshletVertexRemapBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshSkinningBuffer.buffer, mesh.meshSkinningBuffer.allocation);
		// Morph deltas share the cached mesh lifetime. The device-idle proof above
		// permits release before clearing the cache drops its allocation handles.
		if (mesh.meshMorphDeltaBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, mesh.meshMorphDeltaBuffer.buffer, mesh.meshMorphDeltaBuffer.allocation);
			mesh.meshMorphDeltaBuffer = {};
		}
		vmaDestroyBuffer(allocator, mesh.meshDecodeInfoBuffer.buffer, mesh.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBuffer.buffer, mesh.meshletBuffer.allocation);
		if (mesh.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, mesh.meshletSectionBuffer.buffer, mesh.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

	// Crowd instance streams are ordinary frame-slot buffers. Device-idle
	// recovery may destroy them directly; normal growth instead retires a slot
	// buffer and waits for that slot's existing fence before reuse.
	for(VulkanBuffer& buffer : groupedInstanceBuffers) {
		DestroyBufferImmediate(buffer);
	}
	groupedInstanceCapacities.fill(0u);
	for(std::vector<RenderInstanceRecord>& mirror : groupedInstanceUploadMirrors) {
		mirror.clear();
	}
	if(groupedInstanceBufferIndex != INVALID_BINDLESS_BUFFER_INDEX) {
		freeBufferIndices.push_back(groupedInstanceBufferIndex);
		groupedInstanceBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	}

	// View-owned virtual geometry packet buffers are renderer-side scratch
	// resources. Clear them while the device is idle so old packet, batch,
	// compacted, generated-submission, and counter buffers cannot survive a
	// swapchain/device resource rebuild.
	for (std::vector<VirtualGeometryGpuPacketFrameResources>& frameResources : virtualGeometryGpuPacketResourcesByFrame) {
		for (VirtualGeometryGpuPacketFrameResources& resources : frameResources) {
			DestroyVirtualGeometryGpuPacketResources(resources);
		}
		frameResources.clear();
	}
	for (VirtualGeometryGpuPacketFrameResources& resources : retiredVirtualGeometryGpuPacketResources) {
		DestroyVirtualGeometryGpuPacketResources(resources);
	}
	retiredVirtualGeometryGpuPacketResources.clear();
	for (PersistentVirtualGeometryPageResource& page : persistentVirtualGeometryPages)
	{
		DestroyBufferImmediate(page.buffer);
		if (page.bindlessIndex != INVALID_BINDLESS_BUFFER_INDEX)
		{
			freeBufferIndices.push_back(page.bindlessIndex);
		}
	}
	persistentVirtualGeometryPages.clear();
	for (auto& candidates : pendingVirtualGeometryPublicationsByFrame)
	{
		candidates.clear();
	}
	for(auto& resources : virtualGeometryProductionDecodeResourcesByFrame)
	{
		DestroyBufferImmediate(resources.requestBuffer);
		DestroyBufferImmediate(resources.encodedBuffer);
		DestroyBufferImmediate(resources.decodedBuffer);
		DestroyBufferImmediate(resources.resultReadbackBuffer);
		resources = {};
	}
	completedVirtualGeometryProductionDecodeEvidence = false;
	publishedVirtualGeometryProductionDecodeGeneration = 0u;
	// The compute pipeline owns a device-local descriptor layout. Tear it down
	// with the retired per-view sets so a recovery cannot retain old handles.
	DestroyVulkanVirtualGeometryExecutorPipeline();
	DestroyVulkanVirtualGeometryProductionDecodePipeline();
	DestroyVulkanVirtualGeometryVisibilityPipelines();


	for (auto& [id, tex] : textureCache) {
		vkDestroySampler(device, tex.sampler, nullptr);
		vkDestroyImageView(device, tex.view, nullptr);
		vmaDestroyImage(allocator, tex.image, tex.allocation);
	}
	textureCache.clear();

	auto DestroyVulkanImage = [&](VulkanImage& image) {

		if (device && image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}

		if (device && image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}

		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
		image.view = VK_NULL_HANDLE;
		image.sampler = VK_NULL_HANDLE;
	};

	auto DestroyFramebuffer = [&](VkFramebuffer& framebuffer) {

		if (framebuffer != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(device, framebuffer, nullptr);
			framebuffer = VK_NULL_HANDLE;
		}
	};


	for (VkFramebuffer& framebuffer : directionalShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}

	for (VkFramebuffer& framebuffer : spotShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}

	for (VkFramebuffer& framebuffer : areaShadowFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}

	for (std::array<VkFramebuffer, 6>& faceFramebuffers : pointShadowFaceFramebuffersBySlot) {

		for (VkFramebuffer& framebuffer : faceFramebuffers) {
			DestroyFramebuffer(framebuffer);
		}
	}

	auto DestroyImageView = [&](VkImageView& imageView) {

		if (imageView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, imageView, nullptr);
			imageView = VK_NULL_HANDLE;
		}
	};

	for (std::array<VkImageView, 6>& faceViews : pointShadowDepthFaceViewsBySlot) {

		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}

	for (std::array<VkImageView, 6>& faceViews : pointShadowColorFaceViewsBySlot) {

		for (VkImageView& faceView : faceViews) {
			DestroyImageView(faceView);
		}
	}


	for (VulkanImage& image : directionalShadowDepths) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : directionalShadowColors) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : spotShadowDepths) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : spotShadowColors) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : areaShadowDepths) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : areaShadowColors) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : pointShadowCubeDepths) {
		DestroyVulkanImage(image);
	}

	for (VulkanImage& image : pointShadowCubeColors) {
		DestroyVulkanImage(image);
	}
	DestroyVulkanImage(ltcAmplitudeTexture);
	DestroyVulkanImage(ltcInverseTexture);
	DestroyVulkanImage(environmentIrradianceTexture);
	DestroyVulkanImage(prefilteredEnvironmentTexture);
	DestroyVulkanImage(brdfIntegrationTexture);
	for (VulkanImage& image : ambientOcclusionRawByFrame) {
		DestroyVulkanImage(image);
	}
	for (VulkanImage& image : ambientOcclusionBlurredByFrame) {
		DestroyVulkanImage(image);
	}
	for (VulkanImage& image : ambientOcclusionHistoryByFrame) {
		DestroyVulkanImage(image);
	}
	for (ViewDepthHistoryState& viewHistory : depthHistoryByView) {
		for (VulkanImage& image : viewHistory.ambientOcclusionDepthByFrame) {
			DestroyVulkanImage(image);
		}
		viewHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}
	ambientOcclusionWidth = 0u;
	ambientOcclusionHeight = 0u;
	ambientOcclusionHistoryValidByFrame.fill(false);

	textureIndexMap.clear();
	freeTextureIndices.clear();
	nextTextureIndex = 3; // Slots 0-2 are reserved for white, flat-normal, and material-mask defaults.

	// Reset shadow texture indices so they get reallocated on reopen.
	directionalShadowTextureIndices.fill(0u);
	spotShadowTextureIndices.fill(0u);
	areaShadowTextureIndices.fill(0u);
	activeShadowResourceRequest = {};
	shadowStorageRegions.fill({});
	ltcAmplitudeTextureIndex = 0;
	ltcInverseTextureIndex = 0;
	environmentIrradianceTextureIndex = 0;
	prefilteredEnvironmentTextureIndex = 0;
	brdfIntegrationTextureIndex = 0;

	// Reset default texture descriptors to prevent stale views from being sampled if they were overwritten

	if (defaultTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(0, defaultTexture);
	}

	if (defaultNormalTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(1, defaultNormalTexture);
	}

	if (defaultMaterialMaskTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(2, defaultMaterialMaskTexture);
	}

	materialIndexMap.clear();
	materialSourceOwners.clear();
	materialTextureSourceOwners.clear();
	freeMaterialIndices.clear();
	// Material slot zero is the permanent fallback material, so rebuilt caches
	// must resume authored material allocation at slot one.
	nextMaterialIndex = 1u;
	pendingAnimationCommandPlan = {};
	pendingAnimationImmutablePlan.reset();
	pendingCpuAnimationPlan.reset();
	pendingAnimationFrameId = 0U;
	animationComputeRecordedForCurrentFrame = false;
	animationComputeSubmissionMarkedForCurrentFrame = false;
	recordedAnimationDispatchCountForCurrentFrame = 0U;
	activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	pendingDeformationCommandPlan = {};
	pendingDeformationImmutablePlan.reset();
	deformationComputeRecordedForCurrentFrame = false;
	recordedDeformationDispatchCountForCurrentFrame = 0u;
	activeDeformationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution = {};
	}
}
