#include "VulkanContext.h"

#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"

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

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		impl->RecreateSwapChain();
		return;
	} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image!");
		return;
	}

	vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);

	vkResetCommandBuffer(impl->commandBuffers[impl->currentFrame], 0);
	VkCommandBuffer cmd = impl->commandBuffers[impl->currentFrame];

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &beginInfo);

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

	vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

	// Submit each draw command using cached GPU resources.
	for (const auto& command : frameData.commands) {
		impl->EnsureMesh(command.meshId);
		if (!impl->meshCache.contains(command.meshId)) continue;
		
		VulkanMesh& mesh = impl->meshCache[command.meshId];

		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;
		
		TerraEngine::UUID shaderId = material->shaderCollectionId;
		impl->EnsurePipeline(shaderId);
		if (!impl->pipelineCache.contains(shaderId)) continue;
		
		VulkanPipeline& pipeline = impl->pipelineCache[shaderId];

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.pipeline);
		
		VkDescriptorSet materialSet = impl->EnsureMaterialDescriptorSet(command.materialId);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout, 0, 1, &materialSet, 0, nullptr);

		VkBuffer vertexBuffers[] = {mesh.vertexBuffer.buffer};
		VkDeviceSize offsets[] = {0};
		vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
		vkCmdBindIndexBuffer(cmd, mesh.indexBuffer.buffer, 0, mesh.indexType);

		PushConstants push{};
		push.modelViewProjection = frameData.projectionMatrix * frameData.viewMatrix * command.modelMatrix;
		
		Mesh* cpuMesh = ResourceManager::Instance().GetMesh(command.meshId);
		if (cpuMesh) {
			push.positionOffset = glm::vec4(
				cpuMesh->header.quantization.positionOffset[0],
				cpuMesh->header.quantization.positionOffset[1],
				cpuMesh->header.quantization.positionOffset[2],
				0.0f
			);
			push.positionScale = glm::vec4(
				cpuMesh->header.quantization.positionScale[0],
				cpuMesh->header.quantization.positionScale[1],
				cpuMesh->header.quantization.positionScale[2],
				0.0f
			);
			push.uvOffsetScale = glm::vec4(
				cpuMesh->header.quantization.uvOffset[0],
				cpuMesh->header.quantization.uvOffset[1],
				cpuMesh->header.quantization.uvScale[0],
				cpuMesh->header.quantization.uvScale[1]
			);
		} else {
			push.positionOffset = glm::vec4(0.0f);
			push.positionScale = glm::vec4(1.0f);
			push.uvOffsetScale = glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
		}

		vkCmdPushConstants(cmd, pipeline.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(PushConstants), &push);

		vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = waitSemaphores;
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;

	VkSemaphore signalSemaphores[] = {impl->renderFinishedSemaphores[impl->currentFrame]};
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	if (vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffer!");
		return;
	}

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = {impl->swapchain};
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;

	result = vkQueuePresentKHR(impl->presentQueue, &presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
		impl->framebufferResized = false;
		impl->RecreateSwapChain();
	} else if (result != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to present swap chain image!");
	}

	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
}

void VulkanContext::EndFrame() {
	// Placeholder hook for future end-of-frame work like frame pacing.
}

void VulkanContext::Shutdown() {
	// Release every Vulkan object before exiting.
	impl->Cleanup();
}
