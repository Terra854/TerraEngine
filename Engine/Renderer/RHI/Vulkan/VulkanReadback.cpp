// VulkanReadback.cpp — Asynchronous capture and visibility readback lifecycle
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

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

	VkResult submitResult = VK_SUCCESS;
	{
		std::lock_guard<std::mutex> lock(impl->immediateCommandMutex);
		submitResult = impl->SubmitGraphicsQueue(1u, &submitInfo, readbackFence);
	}
	if (submitResult != VK_SUCCESS)
	{
		const bool lost = impl->MarkDeviceLost(submitResult, "asynchronous capture readback submit");
		vkDestroyFence(impl->device, readbackFence, nullptr);
		freeCommandBuffer();
		destroyStagingBuffer();
		result.errorCategory = lost ? "device_lost" : "backend_readback_unavailable";
		result.message = lost
			? "Vulkan device was lost while submitting capture readback."
			: "Vulkan capture readback could not submit its copy command.";
		return result;
	}

	Impl::VulkanReadbackRequest readbackRequest{};
	readbackRequest.completion = result;
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

	impl->inFlightReadbacks.push_back(std::move(readbackRequest));

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

	return result;
}

void VulkanContext::Impl::DestroyReadbackRequestResources(VulkanReadbackRequest& request)
{
	// This helper is called only after fence completion, failed submission, or
	// device teardown. Command-pool operations need the same external lock used
	// by immediate allocation and submission.
	if (request.fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
		vkDestroyFence(device, request.fence, nullptr);
		request.fence = VK_NULL_HANDLE;
	}
	if (request.commandBuffer != VK_NULL_HANDLE && request.commandPool != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
	{
		std::lock_guard<std::mutex> lock(immediateCommandMutex);
		vkFreeCommandBuffers(device, request.commandPool, 1u, &request.commandBuffer);
		request.commandBuffer = VK_NULL_HANDLE;
	}
	if (request.stagingBuffer != VK_NULL_HANDLE && request.stagingAllocation != VK_NULL_HANDLE && allocator != VK_NULL_HANDLE)
	{
		VulkanBuffer stagingBuffer{ request.stagingBuffer, request.stagingAllocation, 0u };
		DestroyBufferImmediate(stagingBuffer);
		request.stagingBuffer = VK_NULL_HANDLE;
		request.stagingAllocation = VK_NULL_HANDLE;
	}
}

bool VulkanContext::Impl::HasInFlightReadbackForImage(VkImage image) const
{
	if (image == VK_NULL_HANDLE)
	{
		return false;
	}
	return std::any_of(inFlightReadbacks.begin(), inFlightReadbacks.end(), [image](const VulkanReadbackRequest& request) {
		return request.sourceImage == image;
	}) || std::any_of(inFlightConventionalVisibilityIdentityReadbacks.begin(),
		inFlightConventionalVisibilityIdentityReadbacks.end(), [image](const VulkanConventionalVisibilityIdentityReadbackRequest& request) {
			return request.sourceImage == image;
		});
}

void VulkanContext::Impl::CancelReadbackRequests(std::string_view errorCategory, std::string_view message, bool publishResults)
{
	for (VulkanReadbackRequest& request : inFlightReadbacks)
	{
		if (publishResults && !request.completionPublished)
		{
			request.completion.ok = false;
			request.completion.pendingAsync = false;
			request.completion.base64Data.clear();
			request.completion.errorCategory = std::string(errorCategory);
			request.completion.message = std::string(message);
			completedReadbacks.push_back(std::move(request.completion));
			request.completionPublished = true;
		}
		DestroyReadbackRequestResources(request);
	}
	inFlightReadbacks.clear();
}

void VulkanContext::Impl::ProcessReadbackRequests()
{
	const auto now = std::chrono::steady_clock::now();
	for (auto iterator = inFlightReadbacks.begin(); iterator != inFlightReadbacks.end();)
	{
		VulkanReadbackRequest& request = *iterator;
		VkResult status = vkGetFenceStatus(device, request.fence);
		if (status == VK_NOT_READY)
		{
			if (!request.completionPublished && now - request.submittedAt >= request.timeout)
			{
				request.completion.ok = false;
				request.completion.pendingAsync = false;
				request.completion.base64Data.clear();
				request.completion.errorCategory = "timeout";
				request.completion.message = "Vulkan async capture readback timed out waiting for the GPU.";
				completedReadbacks.push_back(request.completion);
				request.completionPublished = true;
			}
			// A timeout is a client-visible cancellation, not proof that Vulkan
			// released the submission. Keep all GPU objects until a later poll sees
			// the fence signal or device loss.
			++iterator;
			continue;
		}
		if (status == VK_ERROR_DEVICE_LOST)
		{
			MarkDeviceLost(status, "async readback fence polling");
			if (!request.completionPublished)
			{
				request.completion.ok = false;
				request.completion.errorCategory = "device_lost";
				request.completion.message = "Vulkan device was lost during async capture readback.";
				completedReadbacks.push_back(std::move(request.completion));
			}
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}
		if (status != VK_SUCCESS)
		{
			if (!request.completionPublished)
			{
				request.completion.ok = false;
				request.completion.errorCategory = "backend_readback_unavailable";
				request.completion.message = "Vulkan async capture readback fence polling failed.";
				completedReadbacks.push_back(request.completion);
				request.completionPublished = true;
			}
			// Query errors such as host/device memory exhaustion do not prove the
			// submitted command stopped using its resources. Retain ownership just
			// like the timeout path and retry on a later protected poll.
			++iterator;
			continue;
		}

		// A previously published timeout only needs deferred resource cleanup once
		// the fence signals; it must never publish a second terminal result.
		if (request.completionPublished)
		{
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}

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
			completedReadbacks.push_back(std::move(request.completion));
			DestroyReadbackRequestResources(request);
			iterator = inFlightReadbacks.erase(iterator);
			continue;
		}

		std::vector<std::uint8_t> readbackBytes(static_cast<std::size_t>(copyBytes64));
		void* mapped = nullptr;
		const VkResult mapResult = vmaMapMemory(allocator, request.stagingAllocation, &mapped);
		if (mapResult == VK_ERROR_DEVICE_LOST)
		{
			MarkDeviceLost(mapResult, "async readback memory mapping");
			request.completion.ok = false;
			request.completion.errorCategory = "device_lost";
			request.completion.message = "Vulkan device was lost while mapping capture staging memory.";
			completedReadbacks.push_back(std::move(request.completion));
		}
		else if (mapResult != VK_SUCCESS || mapped == nullptr)
		{
			request.completion.ok = false;
			request.completion.errorCategory = "backend_readback_unavailable";
			request.completion.message = "Vulkan capture readback could not map staging memory.";
			completedReadbacks.push_back(std::move(request.completion));
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
				completedReadbacks.push_back(std::move(request.completion));
			}
			else
			{
				try
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
				}
				catch (const std::exception& exception)
				{
					request.completion.ok = false;
					request.completion.errorCategory = "backend_readback_unavailable";
					request.completion.base64Data.clear();
					request.completion.message = std::string("Vulkan capture readback failed while encoding PNG data: ") + exception.what();
				}
				completedReadbacks.push_back(std::move(request.completion));
			}
		}

		DestroyReadbackRequestResources(request);
		iterator = inFlightReadbacks.erase(iterator);
	}
}

std::vector<TerraEngine::Renderer::RenderCaptureResult> VulkanContext::PollCaptureReadbacks()
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	std::vector<TerraEngine::Renderer::RenderCaptureResult> results;
	if (!impl || !impl->initialized || impl->device == VK_NULL_HANDLE)
	{
		return results;
	}
	impl->ProcessReadbackRequests();
	results.reserve(impl->completedReadbacks.size());
	while (!impl->completedReadbacks.empty())
	{
		results.push_back(std::move(impl->completedReadbacks.front()));
		impl->completedReadbacks.pop_front();
	}
	return results;
}

bool VulkanContext::QueueConventionalVisibilityIdentityReadback(
	const TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackRequest& request)
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	if (!impl || impl->device == VK_NULL_HANDLE || impl->allocator == VK_NULL_HANDLE
		|| impl->deviceLost || request.frameId == 0u) {
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
		vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	VkFence fence = VK_NULL_HANDLE;
	if (vkCreateFence(impl->device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
		vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1u;
	submit.pCommandBuffers = &commandBuffer;
	if (impl->SubmitGraphicsQueue(1u, &submit, fence) != VK_SUCCESS) {
		vkDestroyFence(impl->device, fence, nullptr);
		vkFreeCommandBuffers(impl->device, commandPool, 1u, &commandBuffer);
		impl->DestroyBufferImmediate(staging);
		return false;
	}
	Impl::VulkanConventionalVisibilityIdentityReadbackRequest pending{};
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
	impl->inFlightConventionalVisibilityIdentityReadbacks.push_back(std::move(pending));
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
	VulkanConventionalVisibilityIdentityReadbackRequest& request)
{
	if (request.fence != VK_NULL_HANDLE) vkDestroyFence(device, request.fence, nullptr);
	if (request.commandBuffer != VK_NULL_HANDLE) vkFreeCommandBuffers(device, request.commandPool, 1u, &request.commandBuffer);
	if (request.stagingBuffer != VK_NULL_HANDLE && request.stagingAllocation != VK_NULL_HANDLE) {
		VulkanBuffer staging{ request.stagingBuffer, request.stagingAllocation, 0u };
		DestroyBufferImmediate(staging);
	}
	request = {};
}

void VulkanContext::Impl::ProcessConventionalVisibilityIdentityReadbacks()
{
	for (auto it = inFlightConventionalVisibilityIdentityReadbacks.begin();
		it != inFlightConventionalVisibilityIdentityReadbacks.end();) {
		VkResult status = vkGetFenceStatus(device, it->fence);
		if (status == VK_NOT_READY) { ++it; continue; }
		it->completion.completed = status == VK_SUCCESS;
		if (status != VK_SUCCESS) {
			it->completion.rejectionReason = "Vulkan identity readback fence did not complete successfully.";
		} else {
			const OffscreenResources& currentTarget = it->completion.dedicatedCaptureTarget
				? captureView
				: (it->completion.viewType == RenderViewType::Scene ? sceneView : gameView);
			if (it->sourceImage != currentTarget.visibilityIdentity.image
				|| it->completion.targetGeneration != currentTarget.generation
				|| it->completion.frameId != currentTarget.identityFrameId
				|| it->completion.rendererGeneration != currentTarget.identityRendererGeneration
				|| it->completion.originGeneration != currentTarget.identityOriginGeneration
				|| it->completion.spatialGeneration != currentTarget.identitySpatialGeneration
				|| it->completion.residencyGeneration != currentTarget.identityResidencyGeneration) {
				it->completion.rejectionReason =
					"Vulkan identity readback target was recreated before its fence completed.";
				it->completion.authoritative = false;
				completedConventionalVisibilityIdentityReadbacks.push_back(std::move(it->completion));
				DestroyConventionalVisibilityIdentityReadbackRequestResources(*it);
				it = inFlightConventionalVisibilityIdentityReadbacks.erase(it);
				continue;
			}
			void* mapped = nullptr;
			const VkResult mapResult = vmaMapMemory(allocator, it->stagingAllocation, &mapped);
			const VkResult invalidateResult = (mapResult == VK_SUCCESS && mapped != nullptr)
				? vmaInvalidateAllocation(allocator, it->stagingAllocation, 0u, sizeof(std::uint32_t) * 4u)
				: VK_ERROR_MEMORY_MAP_FAILED;
			if (mapResult != VK_SUCCESS || mapped == nullptr || invalidateResult != VK_SUCCESS) {
				if (mapResult == VK_SUCCESS && mapped != nullptr) vmaUnmapMemory(allocator, it->stagingAllocation);
				it->completion.rejectionReason = "Vulkan identity readback staging memory was unavailable.";
			} else {
				std::uint32_t words[4]{};
				std::memcpy(words, mapped, sizeof(words));
				vmaUnmapMemory(allocator, it->stagingAllocation);
				it->completion.identity = glm::uvec4(words[0], words[1], words[2], words[3]);
				it->completion.authoritative = it->completion.identity != glm::uvec4(0u);
				if (!it->completion.authoritative) it->completion.rejectionReason = "Identity pixel was zero and cannot prove a visible producer.";
			}
		}
		completedConventionalVisibilityIdentityReadbacks.push_back(std::move(it->completion));
		DestroyConventionalVisibilityIdentityReadbackRequestResources(*it);
		it = inFlightConventionalVisibilityIdentityReadbacks.erase(it);
	}
}

std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult>
VulkanContext::PollConventionalVisibilityIdentityReadbacks()
{
	std::lock_guard<std::recursive_mutex> stateLock(impl->deviceStateMutex);
	std::vector<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult> results;
	if (!impl || impl->device == VK_NULL_HANDLE) return results;
	impl->ProcessConventionalVisibilityIdentityReadbacks();
	while (!impl->completedConventionalVisibilityIdentityReadbacks.empty()) {
		results.push_back(std::move(impl->completedConventionalVisibilityIdentityReadbacks.front()));
		impl->completedConventionalVisibilityIdentityReadbacks.pop_front();
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

