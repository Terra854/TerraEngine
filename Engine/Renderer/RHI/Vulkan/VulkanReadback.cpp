// VulkanReadback.cpp — Asynchronous capture and visibility readback lifecycle
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

namespace
{
	static_assert(kMaxVulkanCaptureReadbacks == TerraEngine::Renderer::Vulkan::kReadbackOperationCapacity);
	static_assert(std::is_nothrow_move_constructible_v<TerraEngine::Renderer::RenderCaptureResult>,
		"Accepted acknowledgment and committed delivery must move without allocation.");
	TerraEngine::Renderer::Vulkan::ReadbackFenceObservation ObserveReadbackFence(VkResult result)
	{
		using Observation = TerraEngine::Renderer::Vulkan::ReadbackFenceObservation;
		if (result == VK_SUCCESS) return Observation::Completed;
		if (result == VK_NOT_READY) return Observation::NotReady;
		if (result == VK_ERROR_DEVICE_LOST) return Observation::DeviceLost;
		return Observation::QueryError;
	}
}

TerraEngine::Renderer::CaptureReadbackOwnership VulkanContext::GetCaptureReadbackOwnership(std::string_view requestId) const
{
	using Ownership = TerraEngine::Renderer::CaptureReadbackOwnership;
	if (!impl) return Ownership::Unsupported;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	for (const auto& request : impl->inFlightReadbacks) {
		if (request.admissionId == requestId && request.lifecycle.BlocksSourceImage()) return Ownership::Retained;
	}
	return Ownership::Released;
}

bool VulkanContext::IsShutdownComplete() const
{
	if (!impl) return true;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Capture absence cannot prove device teardown. Observe the actual core
	// owners left intact by Cleanup when idle/presentation proof is unavailable.
	return !impl->initialized && impl->device == VK_NULL_HANDLE
		&& impl->allocator == VK_NULL_HANDLE && impl->instance == VK_NULL_HANDLE;
}

bool VulkanContext::ArmReadbackLifetimeGate(std::string_view requestId)
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (requestId.empty() || !impl->initialized || !impl->validationLayersEnabled || impl->deviceLost
		|| !impl->readbackTimelineSemaphoreEnabled || vkSignalSemaphore == nullptr
		|| impl->device == VK_NULL_HANDLE || impl->readbackValidationGate != VK_NULL_HANDLE
		|| impl->inFlightReadbacks.size() != 0) return false;
	// Allocate the copied correlation identity before creating native ownership.
	TerraEngine::Renderer::Vulkan::ReadbackLifetimeValidationSnapshot receipt;
	receipt.operationId.assign(requestId);
	receipt.renderSessionGeneration = impl->renderSessionGeneration;
	receipt.initialized = true;
	receipt.validationActuallyEnabled = true;
	receipt.validationCallbackInstalled = impl->debugMessenger != VK_NULL_HANDLE;
	receipt.validationErrorsBefore = impl->emptyFrameValidationErrors.load();
	VkSemaphoreTypeCreateInfo type{};
	type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	type.initialValue = 0;
	VkSemaphoreCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	info.pNext = &type;
	const VkResult created = vkCreateSemaphore(impl->device, &info, nullptr, &impl->readbackValidationGate);
	if (created != VK_SUCCESS) return false;
	impl->readbackValidationSnapshot = std::move(receipt);
	impl->readbackValidationSourceImage = VK_NULL_HANDLE;
	impl->readbackValidationCommandPool = VK_NULL_HANDLE;
	impl->readbackValidationSnapshot.timelineSemaphoreEnabled = impl->readbackTimelineSemaphoreEnabled;
	impl->readbackValidationSnapshot.gateTimelineSemaphore = true;
	impl->readbackValidationSnapshot.gateInitialValue = type.initialValue;
	impl->readbackValidationSnapshot.gateCreated = 1;
	impl->RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::GateCreated, created);
	return true;
#else
	(void)requestId;
	return false;
#endif
}

bool VulkanContext::ReleaseReadbackLifetimeGate()
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (impl->readbackValidationSnapshot.hostReleased) return true;
	if (impl->device == VK_NULL_HANDLE || impl->readbackValidationGate == VK_NULL_HANDLE) return false;
	VkSemaphoreSignalInfo signal{};
	signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
	signal.semaphore = impl->readbackValidationGate;
	signal.value = 1;
	const VkResult released = vkSignalSemaphore(impl->device, &signal);
	if (released != VK_SUCCESS) return false;
	impl->readbackValidationSnapshot.hostReleased = true;
	impl->readbackValidationSnapshot.gateSignalValue = signal.value;
	++impl->readbackValidationSnapshot.gateSignaled;
	impl->RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::HostReleased, released);
	// An unsubmitted gate has no native borrower. Submitted gates retire with
	// their fence/idle proof, before vkDestroyDevice, rather than with the fixture.
	if (!impl->readbackValidationSnapshot.submitAccepted) impl->DestroyReadbackValidationGate();
	return true;
#else
	return false;
#endif
}

bool VulkanContext::ArmReadbackPayloadAllocationFailure(std::string_view requestId)
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (impl->readbackValidationGate == VK_NULL_HANDLE
		|| impl->readbackValidationSnapshot.operationId != requestId
		|| impl->readbackValidationSnapshot.submitAccepted) return false;
	impl->readbackValidationSnapshot.payloadAllocationFaultArmed = true;
	return true;
#else
	(void)requestId;
	return false;
#endif
}

TerraEngine::Renderer::Vulkan::ReadbackLifetimeValidationSnapshot VulkanContext::GetReadbackLifetimeValidationSnapshot() const
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	auto snapshot = impl->readbackValidationSnapshot;
	snapshot.timelineSemaphoreEnabled = impl->readbackTimelineSemaphoreEnabled;
	snapshot.validationErrorsAfter = impl->emptyFrameValidationErrors.load();
	return snapshot;
#else
	return {};
#endif
}

void VulkanContext::Impl::RecordReadbackValidationEvent(
	TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind kind, VkResult result) noexcept
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	auto& receipt = readbackValidationSnapshot;
	if (receipt.eventCount == receipt.events.size()) { receipt.eventLogOverflow = true; return; }
	const auto index = receipt.eventCount++;
	receipt.events[index] = { static_cast<std::uint64_t>(index) + 1u, kind, static_cast<std::int32_t>(result) };
#else
	(void)kind;
	(void)result;
#endif
}

void VulkanContext::Impl::DestroyReadbackValidationGate()
{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if (readbackValidationGate == VK_NULL_HANDLE || device == VK_NULL_HANDLE) return;
	vkDestroySemaphore(device, readbackValidationGate, nullptr);
	readbackValidationGate = VK_NULL_HANDLE;
	++readbackValidationSnapshot.gateDestroyed;
	RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::GateDestroyed);
#endif
}

void VulkanContext::Impl::CancelIdentityReadbackRequests(TerraEngine::Renderer::Vulkan::ReadbackRetirementProof proof)
{
	using namespace TerraEngine::Renderer::Vulkan;
	// Identity teardown historically discards its client queue. It still requires
	// the same native proof as color capture before releasing borrowed images.
	struct Adapter final : ReadbackLifecycleAdapter {
		Impl& owner;
		VulkanConventionalVisibilityIdentityReadbackRequest& request;
		Adapter(Impl& context, VulkanConventionalVisibilityIdentityReadbackRequest& operation) : owner(context), request(operation) {}
		ReadbackFenceObservation QueryFence() override { return ReadbackFenceObservation::NotReady; }
		void PublishTerminal(ReadbackTerminalReason) override {}
		void RetireNative() noexcept override { owner.DestroyConventionalVisibilityIdentityReadbackRequestResources(request); }
	};
	for (auto& request : inFlightConventionalVisibilityIdentityReadbacks) {
		Adapter adapter(*this, request);
		request.lifecycle.Cancel(adapter, proof, false);
	}
	if (proof == ReadbackRetirementProof::DeviceIdle || proof == ReadbackRetirementProof::DeviceLost)
		inFlightConventionalVisibilityIdentityReadbacks.clear();
}

TerraEngine::Renderer::RenderCaptureResult VulkanContext::ReadbackCapture(const TerraEngine::Renderer::RenderCaptureRequest& request)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	TerraEngine::Renderer::RenderCaptureResult result;
	result.requestId = request.requestId;
	result.sourceKind = request.sourceKind;
	result.sourceName = request.sourceName;
	result.size = request.targetSize;
	result.frameId = request.submittedFrameId;
	result.freshness = request.freshness;
	result.overlaysIncluded = false;
	result.errorCategory = "backend_readback_unavailable";
	if (!impl || impl->device == VK_NULL_HANDLE || impl->allocator == VK_NULL_HANDLE)
	{
		result.message = "Vulkan capture readback cannot run before the backend is initialized.";
		return result;
	}
	if (impl->deviceLost)
	{
		result.errorCategory = "device_lost";
		result.message = "Vulkan capture readback cannot run while renderer recovery is pending.";
		return result;
	}
	if (impl->inFlightReadbacks.size() >= kMaxVulkanCaptureReadbacks)
	{
		result.errorCategory = "queue_full";
		result.message = "Vulkan capture readback has reached its bounded in-flight limit.";
		return result;
	}

	const bool captureSceneView = request.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
		&& request.viewType == RenderViewType::Scene;
	const bool captureDedicatedView = request.sourceKind != TerraEngine::Renderer::RenderCaptureSourceKind::GameView
		|| request.dedicatedRenderTarget;
	if (!impl->editorMode && !captureDedicatedView)
	{
		result.message =
			"Packaged-runtime Vulkan readback requires a dedicated offscreen capture target.";
		return result;
	}
	const Impl::OffscreenResources& target = captureDedicatedView
		? impl->captureView
		: (captureSceneView ? impl->sceneView : impl->gameView);
	if (target.image == VK_NULL_HANDLE || target.framebuffer == VK_NULL_HANDLE)
	{
		result.message = captureDedicatedView
			? "Dedicated capture render target is not ready for Vulkan readback."
			: (captureSceneView
			? "Scene View render target is not ready for Vulkan readback."
			: "Game View render target is not ready for Vulkan readback.");
		return result;
	}

	const std::uint32_t sourceWidth = target.extent.width;
	const std::uint32_t sourceHeight = target.extent.height;
	if (sourceWidth == 0u || sourceHeight == 0u)
	{
		result.message = "Vulkan readback skipped because the source render target has zero size.";
		return result;
	}

	const std::uint32_t bytesPerPixel = TerraEngine::Renderer::VulkanReadback::BytesPerPixel(impl->offscreenColorFormat);
	if (bytesPerPixel == 0u)
	{
		result.errorCategory = "unsupported_readback";
		result.message = "Vulkan readback cannot encode the active render target format.";
		return result;
	}

	const std::uint64_t pixelCount64 = static_cast<std::uint64_t>(sourceWidth) * static_cast<std::uint64_t>(sourceHeight);
	if (pixelCount64 > std::numeric_limits<std::uint64_t>::max() / bytesPerPixel)
	{
		result.message = "Vulkan readback skipped because the source render target byte count overflowed.";
		return result;
	}
	const std::uint64_t copyBytes64 = pixelCount64 * bytesPerPixel;
	if (copyBytes64 > std::numeric_limits<VkDeviceSize>::max() || copyBytes64 > std::numeric_limits<std::size_t>::max())
	{
		result.message = "Vulkan readback skipped because the source render target is too large to stage.";
		return result;
	}

	// Report the observed native source before any resampling changes the PNG
	// extent. Editor resize evidence must distinguish a rebuilt target from an
	// unchanged source whose pixels were merely scaled to the requested size.
	result.backendTarget = TerraEngine::Renderer::RenderCaptureBackendTargetObservation{
		impl->renderSessionGeneration, target.generation, sourceWidth, sourceHeight, captureDedicatedView };
	VulkanBuffer stagingBuffer{};
	try
	{
		stagingBuffer = impl->CreateBuffer(
			static_cast<VkDeviceSize>(copyBytes64),
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_GPU_TO_CPU,
			"Capture Readback Staging Buffer");
	}
	catch (const std::exception& exception)
	{
		result.message = std::string("Vulkan capture readback could not allocate staging memory: ") + exception.what();
		return result;
	}

	auto destroyStagingBuffer = [&]() {
		if (stagingBuffer.buffer != VK_NULL_HANDLE && stagingBuffer.allocation != VK_NULL_HANDLE)
		{
			impl->DestroyBufferImmediate(stagingBuffer);
		}
	};

	VkCommandBuffer commandBuffer = impl->BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE)
	{
		destroyStagingBuffer();
		result.message = "Vulkan capture readback could not allocate an immediate command buffer.";
		return result;
	}
	const VkCommandPool commandPool = impl->immediateCommandPool != VK_NULL_HANDLE
		? impl->immediateCommandPool
		: impl->commandPools[impl->currentFrame];
	auto freeCommandBuffer = [&]() {
		if (commandBuffer != VK_NULL_HANDLE)
		{
			std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
			vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
			commandBuffer = VK_NULL_HANDLE;
		}
	};
	RecordCaptureImageBarrier(commandBuffer, target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	VkBufferImageCopy copyRegion{};
	copyRegion.bufferOffset = 0u;
	copyRegion.bufferRowLength = 0u;
	copyRegion.bufferImageHeight = 0u;
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.mipLevel = 0u;
	copyRegion.imageSubresource.baseArrayLayer = 0u;
	copyRegion.imageSubresource.layerCount = 1u;
	copyRegion.imageOffset = { 0, 0, 0 };
	copyRegion.imageExtent = { sourceWidth, sourceHeight, 1u };
	vkCmdCopyImageToBuffer(
		commandBuffer,
		target.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		stagingBuffer.buffer,
		1u,
		&copyRegion);
	RecordCaptureImageBarrier(commandBuffer, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	const VkResult endResult = vkEndCommandBuffer(commandBuffer);
	if (endResult != VK_SUCCESS)
	{
		freeCommandBuffer();
		destroyStagingBuffer();
		result.message = "Vulkan capture readback could not finish recording its copy command.";
		return result;
	}

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence readbackFence = VK_NULL_HANDLE;
	const VkResult fenceResult = vkCreateFence(impl->device, &fenceInfo, nullptr, &readbackFence);
	if (fenceResult != VK_SUCCESS)
	{
		destroyStagingBuffer();
		freeCommandBuffer();
		result.message = "Vulkan capture readback could not create a fence.";
		return result;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffer;
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	const std::uint64_t gateWaitValue = 1;
	const VkPipelineStageFlags gateWaitStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkTimelineSemaphoreSubmitInfo timelineWait{};
	timelineWait.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	if (impl->readbackValidationGate != VK_NULL_HANDLE
		&& impl->readbackValidationSnapshot.operationId == request.requestId) {
		// Unlike a host-set VkEvent, a timeline semaphore legally permits this
		// queued transfer to wait for a later host signal. The real fence stays
		// incomplete until value 1 is signaled by ReleaseReadbackLifetimeGate.
		timelineWait.waitSemaphoreValueCount = 1;
		timelineWait.pWaitSemaphoreValues = &gateWaitValue;
		submitInfo.pNext = &timelineWait;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &impl->readbackValidationGate;
		submitInfo.pWaitDstStageMask = &gateWaitStages;
		impl->readbackValidationSnapshot.gateWaitValue = gateWaitValue;
		impl->readbackValidationSnapshot.gateWaitStageMask = gateWaitStages;
		impl->readbackValidationSnapshot.gateRecorded = true;
	}
#endif

	Impl::VulkanReadbackRequest* prepared = nullptr;
	TerraEngine::Renderer::Vulkan::ScopedReadbackAdmission admission([&]() noexcept {
		// Local owners exist before metadata construction. No exception or failed
		// submit can skip this shared RejectSubmission/native rollback path.
		vkDestroyFence(impl->device, readbackFence, nullptr);
		freeCommandBuffer();
		destroyStagingBuffer();
		if (prepared) impl->inFlightReadbacks.erase(prepared);
	});
	try {
		prepared = &impl->inFlightReadbacks.emplace_back();
		Impl::VulkanReadbackRequest& readbackRequest = *prepared;
		readbackRequest.admissionId = request.requestId;
		readbackRequest.completion = result;
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		readbackRequest.validationTracked = impl->readbackValidationGate != VK_NULL_HANDLE
			&& impl->readbackValidationSnapshot.operationId == request.requestId;
		if (readbackRequest.validationTracked) {
			impl->readbackValidationSourceImage = target.image;
			impl->readbackValidationCommandPool = commandPool;
		}
#endif
		readbackRequest.completion.size = glm::uvec2(sourceWidth, sourceHeight);
		readbackRequest.completion.mimeType = "image/png";
		readbackRequest.completion.backendReadback = true;
		readbackRequest.completion.deterministicFallback = false;
		readbackRequest.completion.pendingAsync = false;
		readbackRequest.completion.overlaysIncluded = false;
		readbackRequest.viewType = request.viewType;
		readbackRequest.sourceImage = target.image;
		readbackRequest.stagingBuffer = stagingBuffer.buffer;
		readbackRequest.stagingAllocation = stagingBuffer.allocation;
		readbackRequest.fence = readbackFence;
		readbackRequest.commandBuffer = commandBuffer;
		readbackRequest.commandPool = commandPool;
		readbackRequest.width = sourceWidth;
		readbackRequest.height = sourceHeight;
		readbackRequest.requestedWidth = request.targetSize.x > 0u ? request.targetSize.x : sourceWidth;
		readbackRequest.requestedHeight = request.targetSize.y > 0u ? request.targetSize.y : sourceHeight;
		readbackRequest.sourceFormat = impl->offscreenColorFormat;
		readbackRequest.submittedAt = std::chrono::steady_clock::now();
		readbackRequest.timeout = std::chrono::milliseconds(request.timeoutMs > 0u ? request.timeoutMs : 2000u);
		readbackRequest.dedicatedRenderTarget = captureDedicatedView;
		if (impl->offscreenColorFormat == VK_FORMAT_R16G16B16A16_SFLOAT)
		{
			readbackRequest.completion.warnings.push_back("HDR render target pixels were tone-clamped and gamma encoded into an 8-bit PNG.");
		}
		if (request.includeOverlays)
		{
			readbackRequest.completion.warnings.push_back("Vulkan render-target readback captures the renderer color target before editor overlay composition.");
		}

		// Known terminal messages fit without allocation after native acceptance.
		readbackRequest.completion.errorCategory.reserve(64);
		readbackRequest.completion.message.reserve(256);

		result.ok = true;
		result.pendingAsync = true;
		result.errorCategory.clear();
		result.mimeType = "image/png";
		result.backendReadback = true;
		result.deterministicFallback = false;
		result.message = captureDedicatedView
			? "Queued live Vulkan dedicated capture render target for async readback."
			: (captureSceneView
			? "Queued live Vulkan Scene View render target for async readback."
			: "Queued live Vulkan Game View render target for async readback.");
	} catch (...) {
		result.ok = false;
		result.pendingAsync = false;
		result.errorCategory = "backend_readback_unavailable";
		result.message = "Vulkan capture could not reserve bounded ownership before submission.";
		return result;
	}

	VkResult submitResult = VK_SUCCESS;
	{
		std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
		submitResult = impl->SubmitGraphicsQueue(1u, &submitInfo, readbackFence);
	}
	if (submitResult != VK_SUCCESS)
	{
		const bool lost = impl->MarkDeviceLost(submitResult, "asynchronous capture readback submit");
		result.ok = false;
		result.pendingAsync = false;
		result.errorCategory = lost ? "device_lost" : "backend_readback_unavailable";
		result.message = lost
			? "Vulkan device was lost while submitting capture readback."
			: "Vulkan capture readback could not submit its copy command.";
		return result;
	}


	admission.Accept(prepared->lifecycle); // noexcept: stable ownership transfer, no allocation.
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if (prepared->validationTracked) {
		auto& receipt = impl->readbackValidationSnapshot;
		receipt.submitAccepted = true;
		receipt.nativeState = TerraEngine::Renderer::Vulkan::ReadbackNativeState::Submitted;
		receipt.sourceImageBlocked = true;
		receipt.ownerCreated.fill(1u);
		impl->RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::SubmitAccepted, submitResult);
	}
#endif
	return result;
}

void VulkanContext::Impl::DestroyReadbackRequestResources(VulkanReadbackRequest& request) noexcept
{
	// This helper is called only after fence completion, failed submission, or
	// device teardown. Command-pool operations need the same external lock used
	// by immediate allocation and submission.
	if (request.fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
		vkDestroyFence(device, request.fence, nullptr);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked) ++readbackValidationSnapshot.ownerDestroyed[3];
#endif
		request.fence = VK_NULL_HANDLE;
	}
	if (request.commandBuffer != VK_NULL_HANDLE && request.commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked) {
			readbackValidationSnapshot.commandPoolAliveAtRetirement = request.commandPool == immediateCommandPool
				|| std::find(commandPools.begin(), commandPools.end(), request.commandPool) != commandPools.end();
		}
#endif
		std::lock_guard<std::mutex> lock(immediateCommandMutex);
		vkFreeCommandBuffers(device, request.commandPool, 1u, &request.commandBuffer);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked) {
			++readbackValidationSnapshot.ownerDestroyed[2];
			++readbackValidationSnapshot.commandPoolDependencyReleases;
		}
#endif
		request.commandBuffer = VK_NULL_HANDLE;
	}
	if (request.stagingBuffer != VK_NULL_HANDLE && request.stagingAllocation != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE)
	{
		VulkanBuffer stagingBuffer{ request.stagingBuffer, request.stagingAllocation, 0u };
		DestroyBufferImmediate(stagingBuffer);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked) {
			++readbackValidationSnapshot.ownerDestroyed[0];
			++readbackValidationSnapshot.ownerDestroyed[1];
		}
#endif
		request.stagingBuffer = VK_NULL_HANDLE;
		request.stagingAllocation = VK_NULL_HANDLE;
	}
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
	if (request.validationTracked && readbackValidationSnapshot.sourceImageBlocked) {
		readbackValidationSnapshot.sourceImageBlocked = false;
		++readbackValidationSnapshot.sourceBlockerReleases;
		readbackValidationSnapshot.nativeState = TerraEngine::Renderer::Vulkan::ReadbackNativeState::Retired;
		RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::NativeRetired);
		DestroyReadbackValidationGate();
	}
#endif
}

bool VulkanContext::Impl::HasInFlightReadbackForImage(VkImage image) const
{
	if (image == VK_NULL_HANDLE)
	{
		return false;
	}
	return std::any_of(inFlightReadbacks.begin(), inFlightReadbacks.end(), [image](const VulkanReadbackRequest& request) {
		return request.sourceImage == image && request.lifecycle.BlocksSourceImage();
	}) || std::any_of(inFlightConventionalVisibilityIdentityReadbacks.begin(),
		inFlightConventionalVisibilityIdentityReadbacks.end(), [image](const VulkanConventionalVisibilityIdentityReadbackRequest& request) {
			return request.sourceImage == image && request.lifecycle.BlocksSourceImage();
		});
}

void VulkanContext::Impl::CancelReadbackRequests(std::string_view errorCategory, std::string_view message, bool publishResults, TerraEngine::Renderer::Vulkan::ReadbackRetirementProof proof)
{
	using namespace TerraEngine::Renderer::Vulkan;
	struct Adapter final : ReadbackLifecycleAdapter {
		Impl& owner;
		VulkanReadbackRequest& request;
		std::string_view errorCategory;
		std::string_view message;
		Adapter(Impl& ownerValue, VulkanReadbackRequest& requestValue, std::string_view category, std::string_view text)
			: owner(ownerValue), request(requestValue), errorCategory(category), message(text) {}
		ReadbackFenceObservation QueryFence() override { return ReadbackFenceObservation::NotReady; }
		void PublishTerminal(ReadbackTerminalReason) override {
			request.completion.ok = false;
			request.completion.pendingAsync = false;
			request.completion.base64Data.clear();
			request.completion.errorCategory.assign(errorCategory);
			request.completion.message.assign(message);
			request.deliveryReady = true;
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
			if (request.validationTracked) {
				++owner.readbackValidationSnapshot.terminalDeliveries;
				owner.RecordReadbackValidationEvent(ReadbackValidationEventKind::ClientTerminal);
			}
#endif
		}
		void RetireNative() noexcept override { owner.DestroyReadbackRequestResources(request); }
	};
	for (VulkanReadbackRequest& request : inFlightReadbacks) {
		Adapter adapter(*this, request, errorCategory, message);
		request.lifecycle.Cancel(adapter, proof, publishResults);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked) {
			readbackValidationSnapshot.clientState = request.lifecycle.ClientState();
			readbackValidationSnapshot.nativeState = request.lifecycle.NativeState();
		}
#endif
	}
	// Keep terminal delivery records until the caller drains them.
}

void VulkanContext::Impl::PublishCaptureReadbackTerminal(
	VulkanReadbackRequest& request, TerraEngine::Renderer::Vulkan::ReadbackTerminalReason reason)
{
	using Reason = TerraEngine::Renderer::Vulkan::ReadbackTerminalReason;
	// Payload conversion runs only after the shared controller observes a real
	// completed fence. Every other reason publishes client state without mapping.
	if (reason != Reason::Completed) {
		request.completion.ok = false;
		request.completion.pendingAsync = false;
		request.completion.base64Data.clear();
		if (reason == Reason::Timeout) {
			request.completion.errorCategory = "timeout";
			request.completion.message = "Vulkan async capture readback timed out waiting for the GPU.";
		} else if (reason == Reason::DeviceLost) {
			request.completion.errorCategory = "device_lost";
			request.completion.message = "Vulkan device was lost during async capture readback.";
		} else if (reason == Reason::Canceled) {
			request.completion.errorCategory = "canceled";
			request.completion.message = "Vulkan image readback was canceled during renderer shutdown.";
		} else {
			request.completion.errorCategory = "backend_readback_unavailable";
			request.completion.message = "Vulkan async capture readback fence polling failed.";
		}
		request.deliveryReady = true;
		return;
	}
	try {
		const std::uint32_t bytesPerPixel = TerraEngine::Renderer::VulkanReadback::BytesPerPixel(request.sourceFormat);
		const std::uint64_t copyBytes64 = static_cast<std::uint64_t>(request.width) * static_cast<std::uint64_t>(request.height) * bytesPerPixel;

		const bool captureSceneView = request.completion.sourceKind == TerraEngine::Renderer::RenderCaptureSourceKind::GameView
			&& request.viewType == RenderViewType::Scene;
		const bool captureDedicatedView = request.dedicatedRenderTarget;
		const OffscreenResources& target = captureDedicatedView
			? captureView
			: (captureSceneView ? sceneView : gameView);

		if (request.sourceImage != target.image
			|| request.width != target.extent.width
			|| request.height != target.extent.height)
		{
			request.completion.ok = false;
			request.completion.errorCategory = "resize_aborted";
			request.completion.message = "Vulkan capture readback aborted because its source viewport was recreated before completion.";
			request.deliveryReady = true;
			return;
		}

	#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (request.validationTracked && readbackValidationSnapshot.payloadAllocationFaultArmed) {
			readbackValidationSnapshot.payloadAllocationFaultArmed = false;
			readbackValidationSnapshot.payloadAllocationFaultTriggered = true;
			RecordReadbackValidationEvent(TerraEngine::Renderer::Vulkan::ReadbackValidationEventKind::PayloadAllocationFault);
			throw std::bad_alloc();
		}
	#endif
		std::vector<std::uint8_t> readbackBytes(static_cast<std::size_t>(copyBytes64));
		void* mapped = nullptr;
		const VkResult mapResult = vmaMapMemory(allocator, request.stagingAllocation, &mapped);
		if (mapResult == VK_ERROR_DEVICE_LOST)
		{
			MarkDeviceLost(mapResult, "async readback memory mapping");
			request.completion.ok = false;
			request.completion.errorCategory = "device_lost";
			request.completion.message = "Vulkan device was lost while mapping capture staging memory.";
			request.deliveryReady = true;
		}
		else if (mapResult != VK_SUCCESS || mapped == nullptr)
		{
			request.completion.ok = false;
			request.completion.errorCategory = "backend_readback_unavailable";
			request.completion.message = "Vulkan capture readback could not map staging memory.";
			request.deliveryReady = true;
		}
		else
		{
			const VkResult invalidateResult = vmaInvalidateAllocation(allocator, request.stagingAllocation, 0u, static_cast<VkDeviceSize>(copyBytes64));
			if (invalidateResult == VK_SUCCESS)
			{
				std::memcpy(readbackBytes.data(), mapped, readbackBytes.size());
			}
			vmaUnmapMemory(allocator, request.stagingAllocation);

			if (invalidateResult != VK_SUCCESS)
			{
				if (invalidateResult == VK_ERROR_DEVICE_LOST)
				{
					MarkDeviceLost(invalidateResult, "async readback memory invalidation");
				}
				request.completion.ok = false;
				request.completion.errorCategory = invalidateResult == VK_ERROR_DEVICE_LOST ? "device_lost" : "backend_readback_unavailable";
				request.completion.message = "Vulkan capture readback could not invalidate staging memory for host access.";
				request.deliveryReady = true;
			}
			else
			{
				std::vector<std::uint8_t> rgbaPixels = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(
					readbackBytes,
					request.width,
					request.height,
					request.sourceFormat);
				const std::uint32_t outputWidth = request.requestedWidth > 0u ? request.requestedWidth : request.width;
				const std::uint32_t outputHeight = request.requestedHeight > 0u ? request.requestedHeight : request.height;
				if (outputWidth != request.width || outputHeight != request.height)
				{
					rgbaPixels = TerraEngine::Renderer::ResizeRgba8(
						rgbaPixels,
						request.width,
						request.height,
						outputWidth,
						outputHeight);
					request.completion.warnings.push_back("Capture pixels were resized from the live Vulkan target to the requested output dimensions.");
				}
				const std::vector<std::uint8_t> pngBytes = TerraEngine::Renderer::EncodeRgba8ToPng(rgbaPixels, outputWidth, outputHeight);
				request.completion.base64Data = TerraEngine::Renderer::EncodeBase64(pngBytes);
				request.completion.size = glm::uvec2(outputWidth, outputHeight);
				request.completion.ok = true;
				request.completion.errorCategory.clear();
				request.completion.message = captureDedicatedView
					? "Captured live Vulkan dedicated capture render target."
					: (captureSceneView
					? "Captured live Vulkan Scene View render target."
					: "Captured live Vulkan Game View render target.");
				request.deliveryReady = true;
			}
		}
	} catch (...) {
		// These constant messages fit capacity reserved before queue acceptance.
		// Even bad_alloc can therefore commit one explicit failure and let the
		// completed native submission retire, without allocating error details.
		request.completion.ok = false;
		request.completion.pendingAsync = false;
		request.completion.base64Data.clear();
		request.completion.errorCategory.assign("backend_readback_unavailable");
		request.completion.message.assign("Vulkan capture could not prepare the completed image payload.");
		request.deliveryReady = true;
	}
}

void VulkanContext::Impl::ProcessReadbackRequests()
{
	using namespace TerraEngine::Renderer::Vulkan;
	// Native APIs remain here; the exact same controller drives counted tests.
	struct Adapter final : ReadbackLifecycleAdapter {
		Impl& owner;
		VulkanReadbackRequest& request;
		Adapter(Impl& ownerValue, VulkanReadbackRequest& requestValue) : owner(ownerValue), request(requestValue) {}
		ReadbackFenceObservation QueryFence() override {
			const VkResult status = vkGetFenceStatus(owner.device, request.fence);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
			if (request.validationTracked) {
				owner.readbackValidationSnapshot.lastFenceResult = status;
				owner.RecordReadbackValidationEvent(ReadbackValidationEventKind::FenceObserved, status);
			}
#endif
			if (status == VK_ERROR_DEVICE_LOST) owner.MarkDeviceLost(status, "async readback fence polling");
			return ObserveReadbackFence(status);
		}
		void PublishTerminal(ReadbackTerminalReason reason) override {
			owner.PublishCaptureReadbackTerminal(request, reason);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
			if (request.validationTracked) {
				++owner.readbackValidationSnapshot.terminalDeliveries;
				owner.RecordReadbackValidationEvent(ReadbackValidationEventKind::ClientTerminal);
			}
#endif
		}
		void RetireNative() noexcept override { owner.DestroyReadbackRequestResources(request); }
	};
	const auto now = std::chrono::steady_clock::now();
	for (auto iterator = inFlightReadbacks.begin(); iterator != inFlightReadbacks.end();) {
		Adapter adapter(*this, *iterator);
		iterator->lifecycle.Poll(adapter, now - iterator->submittedAt >= iterator->timeout);
#if defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION)
		if (iterator->validationTracked) {
			readbackValidationSnapshot.clientState = iterator->lifecycle.ClientState();
			readbackValidationSnapshot.nativeState = iterator->lifecycle.NativeState();
		}
#endif
		if (iterator->lifecycle.NativeState() == ReadbackNativeState::Retired && !iterator->deliveryReady) iterator = inFlightReadbacks.erase(iterator);
		else ++iterator;
	}
}

std::vector<TerraEngine::Renderer::RenderCaptureResult> VulkanContext::PollCaptureReadbacks()
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	std::vector<TerraEngine::Renderer::RenderCaptureResult> results;
	if (!impl) return results;
	// Reserve the outward vector before moving any committed delivery. Physical
	// retirement and client delivery are independent, including after shutdown.
	results.reserve(impl->inFlightReadbacks.size());
	if (impl->initialized && impl->device != VK_NULL_HANDLE) impl->ProcessReadbackRequests();
	for (auto iterator = impl->inFlightReadbacks.begin(); iterator != impl->inFlightReadbacks.end();) {
		if (iterator->deliveryReady) {
			results.push_back(std::move(iterator->completion));
			iterator->deliveryReady = false;
		}
		if (iterator->lifecycle.NativeState() == TerraEngine::Renderer::Vulkan::ReadbackNativeState::Retired)
			iterator = impl->inFlightReadbacks.erase(iterator);
		else ++iterator;
	}
	return results;
}

bool VulkanContext::QueueConventionalVisibilityIdentityReadback(
	const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest& request)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (!impl || impl->device == VK_NULL_HANDLE || impl->allocator == VK_NULL_HANDLE
		|| impl->deviceLost || request.frameId == 0u
		|| impl->inFlightConventionalVisibilityIdentityReadbacks.size() >= kMaxVulkanCaptureReadbacks) {
		return false;
	}
	// Dedicated capture proof is selected only by an explicit typed request, so it
	// cannot replace normal Scene/Game viewport identity data.
	const Impl::OffscreenResources& target = request.dedicatedCaptureTarget
		? impl->captureView
		: (request.viewType == RenderViewType::Scene ? impl->sceneView : impl->gameView);
	if (target.visibilityIdentity.image == VK_NULL_HANDLE
		|| target.visibilityIdentity.view == VK_NULL_HANDLE
		|| request.pixel.x >= target.extent.width || request.pixel.y >= target.extent.height) {
		TERRAENGINE_LOG(LogLevel::Warning,
			"Rejecting Vulkan conventional identity readback because the requested target or pixel is unavailable.");
		return false;
	}
	if (request.targetGeneration != 0u && request.targetGeneration != target.generation) {
		TERRAENGINE_LOG(LogLevel::Warning,
			"Rejecting Vulkan conventional identity readback because its target generation is stale.");
		return false;
	}
	if (target.identityFrameId != request.frameId
		|| (request.rendererGeneration != 0u
			&& target.identityRendererGeneration != request.rendererGeneration)
		|| target.identityOriginGeneration != request.originGeneration
		|| target.identitySpatialGeneration != request.spatialGeneration
		|| target.identityResidencyGeneration != request.residencyGeneration) {
		TERRAENGINE_LOG(LogLevel::Warning,
			"Rejecting Vulkan conventional identity readback because rendered target metadata does not match the request. "
				<< "requestFrame=" << request.frameId << " targetFrame=" << target.identityFrameId
				<< " requestGeneration=" << request.rendererGeneration
				<< " targetGeneration=" << target.identityRendererGeneration
				<< " dedicatedCapture=" << request.dedicatedCaptureTarget);
		return false;
	}

	VulkanBuffer staging = impl->CreateBuffer(sizeof(std::uint32_t) * 4u,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU,
		"Conventional Visibility Identity Readback");
	if (staging.buffer == VK_NULL_HANDLE || staging.allocation == VK_NULL_HANDLE) return false;
	VkCommandBuffer commandBuffer = impl->BeginSingleTimeCommands();
	if (commandBuffer == VK_NULL_HANDLE) {
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	const VkCommandPool commandPool = impl->immediateCommandPool != VK_NULL_HANDLE
		? impl->immediateCommandPool : impl->commandPools[impl->currentFrame];
	auto freeIdentityCommandBuffer = [&]() {
		if (commandBuffer == VK_NULL_HANDLE) return;
		// The pool is shared with worker immediate allocations. Freeing a
		// rejected command obeys the same external synchronization contract.
		std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
		vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
		commandBuffer = VK_NULL_HANDLE;
	};
	VkImageMemoryBarrier toTransfer{};
	toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransfer.image = target.visibilityIdentity.image;
	toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransfer.subresourceRange.levelCount = 1u;
	toTransfer.subresourceRange.layerCount = 1u;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);
	VkBufferImageCopy copy{};
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1u;
	copy.imageOffset = { static_cast<std::int32_t>(request.pixel.x), static_cast<std::int32_t>(request.pixel.y), 0 };
	copy.imageExtent = { 1u, 1u, 1u };
	vkCmdCopyImageToBuffer(commandBuffer, target.visibilityIdentity.image,
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1u, &copy);
	std::swap(toTransfer.oldLayout, toTransfer.newLayout);
	toTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toTransfer.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u, 0u, nullptr, 0u, nullptr, 1u, &toTransfer);
	if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
		freeIdentityCommandBuffer();
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(impl->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
		freeIdentityCommandBuffer();
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1u;
	submit.pCommandBuffers = &commandBuffer;
	Impl::VulkanConventionalVisibilityIdentityReadbackRequest* prepared = nullptr;
	TerraEngine::Renderer::Vulkan::ScopedReadbackAdmission admission([&]() noexcept {
		vkDestroyFence(impl->device, fence, nullptr);
		freeIdentityCommandBuffer();
		impl->DestroyBufferImmediate(staging);
		if (prepared) impl->inFlightConventionalVisibilityIdentityReadbacks.erase(prepared);
	});
	try {
		prepared = &impl->inFlightConventionalVisibilityIdentityReadbacks.emplace_back();
		auto& pending = *prepared;
		pending.completion.viewType = request.viewType;
		pending.completion.pixel = request.pixel;
		pending.completion.frameId = request.frameId;
		pending.completion.rendererGeneration = request.rendererGeneration != 0u
			? request.rendererGeneration : impl->renderSessionGeneration;
		pending.completion.targetGeneration = target.generation;
		pending.completion.originGeneration = target.identityOriginGeneration;
		pending.completion.spatialGeneration = target.identitySpatialGeneration;
		pending.completion.residencyGeneration = target.identityResidencyGeneration;
		pending.completion.dedicatedCaptureTarget = request.dedicatedCaptureTarget;
		pending.sourceImage = target.visibilityIdentity.image;
		pending.stagingBuffer = staging.buffer;
		pending.stagingAllocation = staging.allocation;
		pending.fence = fence;
		pending.commandBuffer = commandBuffer;
		pending.commandPool = commandPool;
		pending.completion.rejectionReason.reserve(256);
	} catch (...) {
		return false;
	}
	if (impl->SubmitGraphicsQueue(1u, &submit, fence) != VK_SUCCESS) {
		return false;
	}

	admission.Accept(prepared->lifecycle);
	return true;
}

std::optional<TerraEngine::Renderer::ConventionalVisibilityIdentityTargetMetadata>
VulkanContext::GetConventionalVisibilityIdentityTargetMetadata(
	RenderViewType viewType, bool dedicatedCaptureTarget) const
{
	if(!impl) return std::nullopt;
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	const Impl::OffscreenResources& target = dedicatedCaptureTarget
		? impl->captureView
		: (viewType == RenderViewType::Scene ? impl->sceneView : impl->gameView);
	TerraEngine::Renderer::ConventionalVisibilityIdentityTargetMetadata metadata{};
	metadata.viewType = viewType;
	metadata.frameId = target.identityFrameId;
	metadata.rendererGeneration = target.identityRendererGeneration;
	metadata.targetGeneration = target.generation;
	metadata.originGeneration = target.identityOriginGeneration;
	metadata.spatialGeneration = target.identitySpatialGeneration;
	metadata.residencyGeneration = target.identityResidencyGeneration;
	metadata.dedicatedCaptureTarget = dedicatedCaptureTarget;
	return metadata.IsComplete()
		? std::optional<TerraEngine::Renderer::ConventionalVisibilityIdentityTargetMetadata>(
			metadata)
		: std::nullopt;
}

void VulkanContext::Impl::DestroyConventionalVisibilityIdentityReadbackRequestResources(
	VulkanConventionalVisibilityIdentityReadbackRequest& request) noexcept
{
	if (request.fence != VK_NULL_HANDLE) vkDestroyFence(device, request.fence, nullptr);
	if (request.commandBuffer != VK_NULL_HANDLE) {
		std::lock_guard<std::mutex> lock(immediateCommandMutex);
		vkFreeCommandBuffers(device, request.commandPool, 1u, &request.commandBuffer);
	}
	if (request.stagingBuffer != VK_NULL_HANDLE && request.stagingAllocation != VK_NULL_HANDLE) {
		VulkanBuffer staging{ request.stagingBuffer, request.stagingAllocation, 0u };
		DestroyBufferImmediate(staging);
	}
	request.fence = VK_NULL_HANDLE;
	request.commandBuffer = VK_NULL_HANDLE;
	request.stagingBuffer = VK_NULL_HANDLE;
	request.stagingAllocation = VK_NULL_HANDLE;
}

void VulkanContext::Impl::PublishIdentityReadbackTerminal(
	VulkanConventionalVisibilityIdentityReadbackRequest& request, TerraEngine::Renderer::Vulkan::ReadbackTerminalReason reason)
{
	request.completion.completed = reason == TerraEngine::Renderer::Vulkan::ReadbackTerminalReason::Completed;
	if (!request.completion.completed) {
		request.completion.rejectionReason = "Vulkan identity readback fence did not complete successfully.";
	} else {
		const OffscreenResources& currentTarget = request.completion.dedicatedCaptureTarget
			? captureView
			: (request.completion.viewType == RenderViewType::Scene ? sceneView : gameView);
		if (request.sourceImage != currentTarget.visibilityIdentity.image
			|| request.completion.targetGeneration != currentTarget.generation
			|| request.completion.frameId != currentTarget.identityFrameId
			|| request.completion.rendererGeneration != currentTarget.identityRendererGeneration
			|| request.completion.originGeneration != currentTarget.identityOriginGeneration
			|| request.completion.spatialGeneration != currentTarget.identitySpatialGeneration
			|| request.completion.residencyGeneration != currentTarget.identityResidencyGeneration) {
			request.completion.rejectionReason =
				"Vulkan identity readback target was recreated before its fence completed.";
			request.completion.authoritative = false;
			request.deliveryReady = true;
			return;
		}
		void* mapped = nullptr;
		const VkResult mapResult = vmaMapMemory(allocator, request.stagingAllocation, &mapped);
		const VkResult invalidateResult = (mapResult == VK_SUCCESS && mapped != nullptr)
			? vmaInvalidateAllocation(allocator, request.stagingAllocation, 0u, sizeof(std::uint32_t) * 4u)
			: VK_ERROR_MEMORY_MAP_FAILED;
		if (mapResult != VK_SUCCESS || mapped == nullptr || invalidateResult != VK_SUCCESS) {
			if (mapResult == VK_SUCCESS && mapped != nullptr) vmaUnmapMemory(allocator, request.stagingAllocation);
			request.completion.rejectionReason = "Vulkan identity readback staging memory was unavailable.";
		} else {
			std::uint32_t words[4]{};
			std::memcpy(words, mapped, sizeof(words));
			vmaUnmapMemory(allocator, request.stagingAllocation);
			request.completion.identity = glm::uvec4(words[0], words[1], words[2], words[3]);
			request.completion.authoritative = request.completion.identity != glm::uvec4(0u);
			if (!request.completion.authoritative) request.completion.rejectionReason = "Identity pixel was zero and cannot prove a visible producer.";
		}
	}
	request.deliveryReady = true;
}

void VulkanContext::Impl::ProcessConventionalVisibilityIdentityReadbacks()
{
	using namespace TerraEngine::Renderer::Vulkan;
	struct Adapter final : ReadbackLifecycleAdapter {
		Impl& owner;
		VulkanConventionalVisibilityIdentityReadbackRequest& request;
		Adapter(Impl& ownerValue, VulkanConventionalVisibilityIdentityReadbackRequest& requestValue) : owner(ownerValue), request(requestValue) {}
		ReadbackFenceObservation QueryFence() override { return ObserveReadbackFence(vkGetFenceStatus(owner.device, request.fence)); }
		void PublishTerminal(ReadbackTerminalReason reason) override { owner.PublishIdentityReadbackTerminal(request, reason); }
		void RetireNative() noexcept override { owner.DestroyConventionalVisibilityIdentityReadbackRequestResources(request); }
	};
	for (auto iterator = inFlightConventionalVisibilityIdentityReadbacks.begin(); iterator != inFlightConventionalVisibilityIdentityReadbacks.end();) {
		Adapter adapter(*this, *iterator);
		iterator->lifecycle.Poll(adapter, false);
		if (iterator->lifecycle.NativeState() == ReadbackNativeState::Retired && !iterator->deliveryReady) iterator = inFlightConventionalVisibilityIdentityReadbacks.erase(iterator);
		else ++iterator;
	}
}

std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult>
VulkanContext::PollConventionalVisibilityIdentityReadbacks()
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult> results;
	if (!impl) return results;
	results.reserve(impl->inFlightConventionalVisibilityIdentityReadbacks.size());
	if (impl->device != VK_NULL_HANDLE) impl->ProcessConventionalVisibilityIdentityReadbacks();
	for (auto iterator = impl->inFlightConventionalVisibilityIdentityReadbacks.begin(); iterator != impl->inFlightConventionalVisibilityIdentityReadbacks.end();) {
		if (iterator->deliveryReady) {
			results.push_back(std::move(iterator->completion));
			iterator->deliveryReady = false;
		}
		if (iterator->lifecycle.NativeState() == TerraEngine::Renderer::Vulkan::ReadbackNativeState::Retired)
			iterator = impl->inFlightConventionalVisibilityIdentityReadbacks.erase(iterator);
		else ++iterator;
	}
	return results;
}

std::vector<TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt>
VulkanContext::PollVirtualGeometryExecutionReceipts()
{
	if(!impl) {
		return {};
	}
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// Receipts are inserted only after a fence-owned command has completed. The
	// current CPU-prepared virtual-geometry route inserts none, which prevents a
	// packet upload or CPU-expanded draw from being misreported as execution.
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt> receipts;
	receipts.swap(impl->completedVirtualGeometryExecutionReceipts);
	return receipts;
}

std::vector<TerraEngine::Renderer::VirtualGeometryBackendMeasurement>
VulkanContext::PollVirtualGeometryMeasurements()
{
	if(!impl) {
		return {};
	}
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	// This protected-boundary drain is non-blocking. Unavailable rows deliberately
	// carry no duration, and future live markers arrive only after their delayed
	// Vulkan query readback completed for the frame slot.
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendMeasurement> measurements;
	measurements.swap(impl->completedVirtualGeometryMeasurements);
	return measurements;
}

