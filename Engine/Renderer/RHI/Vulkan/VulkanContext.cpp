#include "VulkanContext.h"

#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"
#include "ThreadPool/ThreadPool.h"

VulkanContext::VulkanContext() : impl(std::make_unique<Impl>()) {}
VulkanContext::~VulkanContext() = default;

void VulkanContext::Initialize(WindowSystem& windowSystem) {
	// Build the Vulkan instance/device chain and prime per-frame resources.
	impl->InitVulkan(windowSystem);
}

void VulkanContext::BeginFrame() {
	// Wait until the GPU is done with the resources for this frame before reusing them.
	vkWaitForFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);
}

void VulkanContext::RenderFrame(const FrameRenderData& frameData) {
	// Skip rendering if initialization failed so we do not spam validation errors.
	if (!impl->initialized) return;

	// Handle Editor Mode resizing (lazy)
	if (impl->editorMode) {
		if (impl->offscreenImage == VK_NULL_HANDLE) {
			impl->InitEditorMode();
		}
		if (impl->offscreenFramebuffer == VK_NULL_HANDLE) {
			impl->CreateOffscreenResources();
		}
	}

	uint32_t imageIndex = 0;
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;

	if (!impl->editorMode) {
		// Normal Mode: Acquire image
		VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);
		
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			impl->RecreateSwapChain();
			return;
		} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image! Result: " << result);
			return;
		}
		
		renderPassInfo.renderPass = impl->renderPass;
		renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	} else {
		// Editor Mode: Render to offscreen
		renderPassInfo.renderPass = impl->offscreenRenderPass;
		renderPassInfo.framebuffer = impl->offscreenFramebuffer;
	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	VkClearValue clearValues[2];
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; 
	clearValues[1].depthStencil = {1.0f, 0};

	renderPassInfo.clearValueCount = 2;
	renderPassInfo.pClearValues = clearValues;

	// Prepare Draw Data
	struct PreparedDraw {
		VulkanMesh* mesh = nullptr;
		VulkanPipeline* pipeline = nullptr;
		std::vector<VkDescriptorSet> descriptorSets;
		PushConstants push{};
	};
	std::vector<PreparedDraw> prepared;
	prepared.reserve(frameData.commands.size());

	for (const auto& command : frameData.commands) {
		impl->EnsureMesh(command.meshId);
		if (!impl->meshCache.contains(command.meshId)) continue;

		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;

		TerraEngine::UUID shaderId = material->shaderCollectionId;
		impl->EnsurePipeline(shaderId, renderPassInfo.renderPass);
		if (!impl->pipelineCache.contains(renderPassInfo.renderPass) || !impl->pipelineCache[renderPassInfo.renderPass].contains(shaderId)) continue;

		std::vector<VkDescriptorSet> materialSets = impl->EnsureMaterialDescriptorSets(shaderId, command.materialId);

		VulkanPipeline& pipeline = impl->pipelineCache[renderPassInfo.renderPass][shaderId];
		VulkanMesh& mesh = impl->meshCache[command.meshId];

		PushConstants push{};
		push.modelViewProjection = frameData.projectionMatrix * frameData.viewMatrix * command.modelMatrix;

		if (Mesh* cpuMesh = ResourceManager::Instance().GetMesh(command.meshId)) {
			push.positionOffset = glm::vec4(cpuMesh->header.quantization.positionOffset[0], cpuMesh->header.quantization.positionOffset[1], cpuMesh->header.quantization.positionOffset[2], 0.0f);
			push.positionScale = glm::vec4(cpuMesh->header.quantization.positionScale[0], cpuMesh->header.quantization.positionScale[1], cpuMesh->header.quantization.positionScale[2], 0.0f);
			push.uvOffsetScale = glm::vec4(cpuMesh->header.quantization.uvOffset[0], cpuMesh->header.quantization.uvOffset[1], cpuMesh->header.quantization.uvScale[0], cpuMesh->header.quantization.uvScale[1]);
		} else {
			push.positionOffset = glm::vec4(0.0f);
			push.positionScale = glm::vec4(1.0f);
			push.uvOffsetScale = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		}

		PreparedDraw draw{};
		draw.mesh = &mesh;
		draw.pipeline = &pipeline;
		draw.descriptorSets = std::move(materialSets);
		draw.push = push;
		prepared.push_back(std::move(draw));
	}

	vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
	vkResetCommandBuffer(impl->commandBuffers[impl->currentFrame], 0);
	VkCommandBuffer cmd = impl->commandBuffers[impl->currentFrame];

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &beginInfo);

	std::vector<VkCommandBuffer> secondaryBuffers;
	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);
		auto& framePools = impl->workerCommandPools[impl->currentFrame];
		for (size_t i = 0; i < workerCount; ++i) vkResetCommandPool(impl->device, framePools[i], 0);

		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);

		for (size_t taskIndex = 0; taskIndex < workerCount; ++taskIndex) {
			const size_t start = taskIndex * chunkSize;
			if (start >= prepared.size()) break;
			const size_t end = std::min(prepared.size(), start + chunkSize);
			VkCommandPool pool = framePools[taskIndex];
			VkFramebuffer framebuffer = renderPassInfo.framebuffer;
			VkRenderPass renderPass = renderPassInfo.renderPass;

			auto task = ThreadPool::instance().createTask([pool, frameIndex = impl->currentFrame, workerIndex = taskIndex, renderPass, framebuffer, start, end, &prepared, this]() -> VkCommandBuffer {
				VkCommandBuffer secondary = impl->AcquireWorkerCommandBuffer(frameIndex, workerIndex);
				if (secondary == VK_NULL_HANDLE) return VK_NULL_HANDLE;

				VkCommandBufferInheritanceInfo inherit{};
				inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
				inherit.renderPass = renderPass;
				inherit.subpass = 0;
				inherit.framebuffer = framebuffer;

				VkCommandBufferBeginInfo begin{};
				begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
				begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
				begin.pInheritanceInfo = &inherit;

				vkBeginCommandBuffer(secondary, &begin);
				for (size_t i = start; i < end; ++i) {
					const PreparedDraw& draw = prepared[i];
					vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
					if (!draw.descriptorSets.empty()) {
						vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);
					}
					VkBuffer vertexBuffers[] = {draw.mesh->vertexBuffer.buffer};
					VkDeviceSize offsets[] = {0};
					vkCmdBindVertexBuffers(secondary, 0, 1, vertexBuffers, offsets);
					vkCmdBindIndexBuffer(secondary, draw.mesh->indexBuffer.buffer, 0, draw.mesh->indexType);
					if (draw.pipeline->supportsDefaultPushConstants) {
						vkCmdPushConstants(secondary, draw.pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &draw.push);
					}
					vkCmdDrawIndexed(secondary, draw.mesh->indexCount, 1, 0, 0, 0);
				}
				vkEndCommandBuffer(secondary);
				return secondary;
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}
		for (auto& f : futures) {
			VkCommandBuffer secondary = f.get();
			if (secondary != VK_NULL_HANDLE) secondaryBuffers.push_back(secondary);
		}
	}

	VkSubpassContents contents = secondaryBuffers.empty() ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
	vkCmdBeginRenderPass(cmd, &renderPassInfo, contents);
	if (!secondaryBuffers.empty()) {
		vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
	}
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	// In Normal Mode, we wait for ImageAvailable. In Editor Mode, we don't.
	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	
	if (!impl->editorMode) {
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
	}

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;

	VkSemaphore signalSemaphores[] = {impl->renderFinishedSemaphores[impl->currentFrame]};
	
	// In Editor Mode, Scene -> UI sync is via Fence, so we don't signal semaphore
	// This prevents validation errors if UI submission fails or skips wait.
	if (!impl->editorMode) {
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;
	} else {
		submitInfo.signalSemaphoreCount = 0;
	}

	VkResult submitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
	if (submitResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffer! Result: " << submitResult);
		std::cerr << "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.\n";
		impl->RecreateSwapChain();
		return;
	}

	if (!impl->editorMode) {
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = {impl->swapchain};
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &imageIndex;

		VkResult result = vkQueuePresentKHR(impl->presentQueue, &presentInfo);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
			impl->framebufferResized = false;
			impl->RecreateSwapChain();
		} else if (result != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to present swap chain image! Result: " << result);
		}

		impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
	}
}

void VulkanContext::EndFrame() {}

void VulkanContext::Shutdown() {
	impl->Cleanup();
}

void VulkanContext::GetEditorHandles(EditorHandles& h) {
	h.instance = impl->instance;
	h.physicalDevice = impl->physicalDevice;
	h.device = impl->device;
	h.queue = impl->graphicsQueue;
	h.queueFamily = impl->FindQueueFamilies(impl->physicalDevice).graphicsFamily.value();
	h.descriptorPool = impl->descriptorPool;
	h.renderPass = impl->renderPass;
	h.minImageCount = impl->MAX_FRAMES_IN_FLIGHT;
	h.imageCount = static_cast<uint32_t>(impl->swapchainImages.size());
	h.sceneImageView = (void*)impl->offscreenImageView;
	h.sceneSampler = (void*)impl->offscreenSampler;
	h.vkGetInstanceProcAddr = (void*)vkGetInstanceProcAddr;
}

void VulkanContext::EnableEditorMode() {
	impl->InitEditorMode();
}

void VulkanContext::PresentEditor(std::function<void(void*)> renderUI) {
	if (!impl->editorMode || !impl->initialized) return;

	// Wait for Scene Render to finish (CPU Wait on Fence)
	// This serializes Scene -> UI rendering, ensuring the offscreen texture is ready.
	vkWaitForFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		impl->RecreateSwapChain();
		impl->RecreateOffscreenResources();
		return;
	} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image for Editor!");
		return;
	}

	VkCommandBuffer uiCmd;
	VkCommandBufferAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocInfo.commandPool = impl->commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	vkAllocateCommandBuffers(impl->device, &allocInfo, &uiCmd);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(uiCmd, &beginInfo);

	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	renderPassInfo.renderPass = impl->renderPass;
	renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	VkClearValue clearValues[2];
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clearValues[1].depthStencil = {1.0f, 0};

	renderPassInfo.clearValueCount = 2;
	renderPassInfo.pClearValues = clearValues;

	vkCmdBeginRenderPass(uiCmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
	if (renderUI) renderUI(uiCmd);
	vkCmdEndRenderPass(uiCmd);
	vkEndCommandBuffer(uiCmd);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	
	// Wait ONLY for Image Available. Scene dependency handled by Fence.
	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &uiCmd;

	VkSemaphore uiSemaphore = impl->uiSemaphores[impl->currentFrame];
	
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &uiSemaphore;

	VkFence uiFence;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	vkCreateFence(impl->device, &fenceInfo, nullptr, &uiFence);

	VkResult uiSubmitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, uiFence);
	if (uiSubmitResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "PresentEditor QueueSubmit failed: " << uiSubmitResult);
		vkDestroyFence(impl->device, uiFence, nullptr);
		vkFreeCommandBuffers(impl->device, impl->commandPool, 1, &uiCmd);
		impl->RecreateSwapChain();
		return;
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &uiSemaphore;
	
	VkSwapchainKHR swapChains[] = {impl->swapchain};
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;

	vkQueuePresentKHR(impl->presentQueue, &presentInfo);

	vkWaitForFences(impl->device, 1, &uiFence, VK_TRUE, UINT64_MAX);
	vkDestroyFence(impl->device, uiFence, nullptr);
	vkFreeCommandBuffers(impl->device, impl->commandPool, 1, &uiCmd);

	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
}

// Impl Extensions

void VulkanContext::Impl::InitEditorMode() {
	editorMode = true;
	CreateOffscreenResources();
}

void VulkanContext::Impl::CreateOffscreenResources() {
	if (offscreenImage != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, offscreenImage, offscreenAllocation);
		vkDestroyImageView(device, offscreenImageView, nullptr);
		vkDestroyFramebuffer(device, offscreenFramebuffer, nullptr);
		vkDestroySampler(device, offscreenSampler, nullptr);
		// Don't destroy RenderPass if compatible?
	}
	
	if (offscreenRenderPass == VK_NULL_HANDLE) {
		// Create Render Pass
		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM; // Force RGBA8 for offscreen
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkAttachmentDescription depthAttachment{};
		depthAttachment.format = FindDepthFormat();
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthAttachmentRef{};
		depthAttachmentRef.attachment = 1;
		depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = &depthAttachmentRef;

		std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;

		// Dependencies
		std::array<VkSubpassDependency, 2> dependencies;
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassInfo.pDependencies = dependencies.data();

		vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenRenderPass);
	}

	// Create Image
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = swapchainExtent.width;
	imageInfo.extent.height = swapchainExtent.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	vmaCreateImage(allocator, &imageInfo, &allocInfo, &offscreenImage, &offscreenAllocation, nullptr);

	// Create View
	offscreenImageView = CreateImageView(offscreenImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

	// Create Framebuffer
	std::array<VkImageView, 2> attachments = {
		offscreenImageView,
		depthImageView // Reuse swapchain depth? Size must match.
	};
	// Wait, swapchain depth matches swapchain extent. Offscreen matches swapchain extent. Safe.
	
	VkFramebufferCreateInfo framebufferInfo{};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = offscreenRenderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
	framebufferInfo.pAttachments = attachments.data();
	framebufferInfo.width = swapchainExtent.width;
	framebufferInfo.height = swapchainExtent.height;
	framebufferInfo.layers = 1;

	vkCreateFramebuffer(device, &framebufferInfo, nullptr, &offscreenFramebuffer);

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

	vkCreateSampler(device, &samplerInfo, nullptr, &offscreenSampler);
}

void VulkanContext::Impl::RecreateOffscreenResources() {
	vkDeviceWaitIdle(device);
	CreateOffscreenResources();
}