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

	struct PreparedDraw {
		VulkanMesh* mesh = nullptr;
		VulkanPipeline* pipeline = nullptr;
		std::vector<VkDescriptorSet> descriptorSets;
		PushConstants push{};
	};

	uint32_t imageIndex;
	VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR) {
		impl->RecreateSwapChain();
		return;
	} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image!");
		return;
	}

	std::vector<PreparedDraw> prepared;
	prepared.reserve(frameData.commands.size());

	// Prime GPU resources serially to avoid cache races, then record in parallel.
	for (const auto& command : frameData.commands) {
		impl->EnsureMesh(command.meshId);
		if (!impl->meshCache.contains(command.meshId)) continue;

		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;

		TerraEngine::UUID shaderId = material->shaderCollectionId;
		impl->EnsurePipeline(shaderId);
		if (!impl->pipelineCache.contains(shaderId)) continue;

		std::vector<VkDescriptorSet> materialSets = impl->EnsureMaterialDescriptorSets(shaderId, command.materialId);

		VulkanPipeline& pipeline = impl->pipelineCache[shaderId];
		VulkanMesh& mesh = impl->meshCache[command.meshId];

		PushConstants push{};
		push.modelViewProjection = frameData.projectionMatrix * frameData.viewMatrix * command.modelMatrix;

		if (Mesh* cpuMesh = ResourceManager::Instance().GetMesh(command.meshId)) {
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

	std::vector<VkCommandBuffer> secondaryBuffers;

	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);

		auto& framePools = impl->workerCommandPools[impl->currentFrame];

		for (size_t i = 0; i < workerCount; ++i) {
			vkResetCommandPool(impl->device, framePools[i], 0);
		}

		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);

		for (size_t taskIndex = 0; taskIndex < workerCount; ++taskIndex) {
			const size_t start = taskIndex * chunkSize;
			if (start >= prepared.size()) break;
			const size_t end = std::min(prepared.size(), start + chunkSize);
			VkCommandPool pool = framePools[taskIndex];
			VkFramebuffer framebuffer = impl->framebuffers[imageIndex];
			VkRenderPass renderPass = impl->renderPass;

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
					if (!draw.mesh || !draw.pipeline) continue;

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

		for (size_t idx = 0; idx < futures.size(); ++idx) {
			VkCommandBuffer secondary = futures[idx].get();
			if (secondary != VK_NULL_HANDLE) {
				secondaryBuffers.push_back(secondary);
			}
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
