#include "VulkanContext.h"

#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"
#include "ThreadPool/ThreadPool.h"
#include <fstream>

VulkanContext::VulkanContext() : impl(std::make_unique<Impl>()) {}
VulkanContext::~VulkanContext() = default;

void VulkanContext::Initialize(WindowSystem& windowSystem) {
	// Build the Vulkan instance/device chain and prime per-frame resources.
	impl->InitVulkan(windowSystem);
}

void VulkanContext::Resize(int width, int height, ViewType view) {
	if (impl) {
		impl->framebufferResized = true;
	}
}

void VulkanContext::BeginFrame() {
	// Wait until the GPU is done with the resources for this frame before reusing them.
	if (!impl->initialized) return;

	vkWaitForFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame], VK_TRUE, UINT64_MAX);

	// Release any extra primary command buffers used for prior camera passes
	// now that the fence guarantees they are idle.
	if (impl->currentFrame < impl->extraPrimaryCommandBuffers.size()) {
		auto& extras = impl->extraPrimaryCommandBuffers[impl->currentFrame];
		if (!extras.empty()) {
			vkFreeCommandBuffers(impl->device, impl->commandPools[impl->currentFrame], static_cast<uint32_t>(extras.size()), extras.data());
			extras.clear();
		}
	}
	if (impl->currentFrame < impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers[impl->currentFrame].clear();
	}

	// Reset command pool so the main command buffer and any newly allocated
	// per-camera buffers start from a clean state each frame.
	vkResetCommandPool(impl->device, impl->commandPools[impl->currentFrame], 0);
	// Reset worker pools for this frame once so secondary buffers can be rerecorded safely.
	if (impl->currentFrame < impl->workerCommandPools.size()) {
		auto& framePools = impl->workerCommandPools[impl->currentFrame];
		for (VkCommandPool pool : framePools) {
			if (pool != VK_NULL_HANDLE) {
				vkResetCommandPool(impl->device, pool, 0);
			}
		}
	}
	// Reset usage counts for this frame
	if (impl->currentFrame < impl->workerBufferUsage.size()) {
		std::fill(impl->workerBufferUsage[impl->currentFrame].begin(), impl->workerBufferUsage[impl->currentFrame].end(), 0);
	} else if (impl->workerBufferUsage.size() <= impl->currentFrame) {
		impl->workerBufferUsage.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}

	impl->swapchainImageAcquired = false;
	impl->skipFrame = false;

	// Acquire the swapchain image once per frame so multiple camera passes can
	// render to the same back buffer and present only on the final camera.
	if (!impl->editorMode) {
		VkResult result = vkAcquireNextImageKHR(impl->device, impl->swapchain, UINT64_MAX, impl->imageAvailableSemaphores[impl->currentFrame], VK_NULL_HANDLE, &impl->acquiredImageIndex);
		if (result == VK_ERROR_OUT_OF_DATE_KHR) {
			impl->RecreateSwapChain();
			impl->skipFrame = true;
			return;
		} else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to acquire swap chain image! Result: " << result);
			impl->skipFrame = true;
			return;
		}
		impl->swapchainImageAcquired = true;
	}
}

// Create Grid Pipeline for Scene View
void VulkanContext::Impl::CreateSceneViewGridPipeline(VkRenderPass renderPass) {
    if (sceneViewGridPipeline != VK_NULL_HANDLE) return;

    // Load SPIR-V
    auto loadShader = [&](const std::string& path) -> VkShaderModule {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open()) {
            TERRAENGINE_LOG(LogLevel::Error, "Failed to open shader file: " << path);
            return VK_NULL_HANDLE;
        }
        size_t fileSize = (size_t)file.tellg();
        std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
        file.seekg(0);
        file.read((char*)buffer.data(), fileSize);
        file.close();

        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = buffer.size() * sizeof(uint32_t);
        createInfo.pCode = buffer.data();

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << path);
            return VK_NULL_HANDLE;
        }
        return shaderModule;
    };

    // We expect these files to be generated.
    VkShaderModule vertShader = loadShader("Shaders/Editor/SceneViewGrid.vert.spv");
    VkShaderModule fragShader = loadShader("Shaders/Editor/SceneViewGrid.frag.spv");

    if (!vertShader || !fragShader) {
        TERRAENGINE_LOG(LogLevel::Warning, "Grid shaders not found. Grid will not be rendered.");
        if (vertShader) vkDestroyShaderModule(device, vertShader, nullptr);
        if (fragShader) vkDestroyShaderModule(device, fragShader, nullptr);
        return;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShader;
    vertShaderStageInfo.pName = "VSMain";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShader;
    fragShaderStageInfo.pName = "PSMainWithDepth";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{}; // Dynamic
    VkRect2D scissor{}; // Dynamic

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE; 
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_TRUE;
    colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(GridPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &sceneViewGridPipelineLayout) != VK_SUCCESS) {
        TERRAENGINE_LOG(LogLevel::Error, "failed to create grid pipeline layout!");
        return;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = sceneViewGridPipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &sceneViewGridPipeline) != VK_SUCCESS) {
        TERRAENGINE_LOG(LogLevel::Error, "failed to create grid pipeline!");
    }

    vkDestroyShaderModule(device, vertShader, nullptr);
    vkDestroyShaderModule(device, fragShader, nullptr);
}

void VulkanContext::Impl::RenderSceneViewGrid(VkCommandBuffer cmd, const FrameRenderData& frameData) {
    if (sceneViewGridPipeline == VK_NULL_HANDLE) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneViewGridPipeline);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapchainExtent.width;
    viewport.height = (float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    GridPushConstants push{};
    push.view = frameData.viewMatrix;
    push.proj = frameData.projectionMatrix;
    push.inverseView = glm::inverse(frameData.viewMatrix);
    push.inverseProj = glm::inverse(frameData.projectionMatrix);

    vkCmdPushConstants(cmd, sceneViewGridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPushConstants), &push);

    vkCmdDraw(cmd, 6, 1, 0, 0); // 6 vertices for fullscreen quad
}

// Modify RenderFrame to initialize/render grid
// I will inject this into RenderFrame method.

void VulkanContext::RenderFrame(const FrameRenderData& frameData, ViewType view) {

	// Skip rendering if initialization failed so we do not spam validation errors.

	if (!impl->initialized || impl->skipFrame) return;



	// Handle Editor Mode resizing (lazy)

	if (impl->editorMode) {

		if (impl->sceneView.image == VK_NULL_HANDLE) {

			impl->InitEditorMode();

		}

		if (impl->sceneView.framebuffer == VK_NULL_HANDLE) {

			impl->CreateOffscreenResources(impl->sceneView);

		}

        if (impl->gameView.framebuffer == VK_NULL_HANDLE) {

            impl->CreateOffscreenResources(impl->gameView);

        }

	}



	uint32_t imageIndex = impl->acquiredImageIndex;

	VkRenderPassBeginInfo renderPassInfo{};

	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;



	if (!impl->editorMode) {

		if (!impl->swapchainImageAcquired) {

			return;

		}

		renderPassInfo.renderPass = impl->renderPass;

		renderPassInfo.framebuffer = impl->framebuffers[imageIndex];

	} else {

		// Editor Mode: Render to offscreen

		renderPassInfo.renderPass = impl->offscreenRenderPass;

		renderPassInfo.framebuffer = (view == ViewType::Scene) ? impl->sceneView.framebuffer : impl->gameView.framebuffer;

	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	VkClearValue clearValues[2];
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; 
	clearValues[1].depthStencil = {1.0f, 0};

	renderPassInfo.clearValueCount = 2;
	renderPassInfo.pClearValues = clearValues;

    // Create Grid Pipeline if needed (lazy init)
    if (view == ViewType::Scene) {
        impl->CreateSceneViewGridPipeline(renderPassInfo.renderPass);
    }

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
		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;

		TerraEngine::UUID collectionId = material->header.shaderCollectionId;
		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
		}

		VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;
		uint32_t materialFlags = material->header.flags;

		impl->EnsureMesh(command.meshId);
		impl->EnsurePipeline(collectionId, currentPass, materialFlags);
		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);

		PipelineKey pipeKey = { collectionId, materialFlags };
		VulkanPipeline& pipeline = impl->pipelineCache[currentPass][pipeKey];
		VulkanMesh& mesh = impl->meshCache[command.meshId];

		PushConstants push{};
		push.viewProj = frameData.projectionMatrix * frameData.viewMatrix;
        push.model = command.modelMatrix;
		
		// Set texture index for bindless access
		push.textureIndex = 0; // Default
		if (material->header.textureCount > 0 && !material->textures.empty()) {
			TerraEngine::UUID texID = material->textures[0].textureUuid;
			if (impl->textureIndexMap.contains(texID)) {
				push.textureIndex = impl->textureIndexMap[texID];
			}
		}

		push.materialIndex = 0; // Default
		if (impl->materialIndexMap.contains(command.materialId)) {
			push.materialIndex = impl->materialIndexMap[command.materialId];
		}

		PreparedDraw draw{};
		draw.mesh = &mesh;
		draw.pipeline = &pipeline;
		draw.descriptorSets = { impl->globalDescriptorSet }; // Always bind the global set
		draw.push = push;
		prepared.push_back(std::move(draw));
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const bool useMainCommandBuffer = impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size() || impl->pendingPrimaryCommandBuffers[impl->currentFrame].empty();
	if (useMainCommandBuffer) {
		cmd = impl->commandBuffers[impl->currentFrame];
		vkResetCommandBuffer(cmd, 0);
	} else {
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = impl->commandPools[impl->currentFrame];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(impl->device, &allocInfo, &cmd) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffer for additional camera pass");
			return;
		}
		if (impl->currentFrame >= impl->extraPrimaryCommandBuffers.size()) {
			impl->extraPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
		}
		impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
	}

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	vkBeginCommandBuffer(cmd, &beginInfo);

	// If this is a subsequent pass targeting the swapchain (multi-camera),
	// inject a barrier to wait for the previous pass to finish its writes.
	if (!useMainCommandBuffer && !impl->editorMode) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		// Wait for previous color output
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		// We will write color output
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Previous pass ended here
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Keep it here, RenderPass will handle transition from Initial=Undefined
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = impl->swapchainImages[imageIndex];
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &barrier
		);
	}

	std::vector<VkCommandBuffer> secondaryBuffers;
	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

			impl->EnsureWorkerPools(impl->currentFrame, workerCount);
			auto& framePools = impl->workerCommandPools[impl->currentFrame];

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
					vkResetCommandBuffer(secondary, 0);

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
						vkCmdPushConstants(secondary, draw.pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &draw.push);
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

    // Render Grid (Scene View Only)
    if (view == ViewType::Scene) {
        // Allocate a secondary buffer for the grid on the main thread
        // We use 'workerCount' as the index to avoid collision with threaded workers
        // We need to calculate workerCount again or store it.
        const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
        
        // Ensure pool exists for this "extra" worker index
        impl->EnsureWorkerPools(impl->currentFrame, workerCount + 1);
        
        VkCommandBuffer gridCmd = impl->AcquireWorkerCommandBuffer(impl->currentFrame, workerCount);
        if (gridCmd != VK_NULL_HANDLE) {
            vkResetCommandBuffer(gridCmd, 0);
            
            VkCommandBufferInheritanceInfo inherit{};
            inherit.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
            inherit.renderPass = renderPassInfo.renderPass;
            inherit.subpass = 0;
            inherit.framebuffer = renderPassInfo.framebuffer;

            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
            begin.pInheritanceInfo = &inherit;

            vkBeginCommandBuffer(gridCmd, &begin);
            impl->RenderSceneViewGrid(gridCmd, frameData);
            vkEndCommandBuffer(gridCmd);
            
            secondaryBuffers.push_back(gridCmd);
        }
    }

	VkSubpassContents contents = secondaryBuffers.empty() ? VK_SUBPASS_CONTENTS_INLINE : VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS;
	vkCmdBeginRenderPass(cmd, &renderPassInfo, contents);
	if (!secondaryBuffers.empty()) {
		vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());
	}
	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);

	// Record the command buffer to be submitted at EndFrame so multiple cameras
	// render sequentially before a single present.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	impl->pendingPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
}

void VulkanContext::EndFrame() {
	if (!impl->initialized || impl->skipFrame) return;

	// Submit all recorded camera command buffers together so the swapchain is
	// presented once using the final camera's output.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		return;
	}

	auto& cmds = impl->pendingPrimaryCommandBuffers[impl->currentFrame];
	if (cmds.empty()) {
		return;
	}

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	VkSemaphore waitSemaphores[] = {impl->imageAvailableSemaphores[impl->currentFrame]};
	VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
	if (!impl->editorMode && impl->swapchainImageAcquired) {
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;
	}

	submitInfo.commandBufferCount = static_cast<uint32_t>(cmds.size());
	submitInfo.pCommandBuffers = cmds.data();

	// If not in editor mode, we must use the semaphore corresponding to the swapchain image index
	// to avoid validation errors about reusing semaphores still in use by the presentation engine.
	VkSemaphore signalSemaphore = impl->editorMode 
		? impl->renderFinishedSemaphores[impl->currentFrame] 
		: impl->renderFinishedSemaphores[impl->acquiredImageIndex];

	VkSemaphore signalSemaphores[] = {signalSemaphore};
	// Always signal render finished, so Editor pass can wait on it
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	if (impl->editorMode) {
		// In Editor Mode, we defer the frame fence signal to the UI pass (PresentEditor)
		// so the frame is considered "in flight" until the UI is also done.
		VkResult submitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffers! Result: " << submitResult);
			impl->RecreateSwapChain();
			return;
		}
	} else {
		vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);
		VkResult submitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
		if (submitResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to submit draw command buffers! Result: " << submitResult);
			std::cerr << "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.\n";
			impl->RecreateSwapChain();
			return;
		}
	}

	if (!impl->editorMode) {
		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		VkSwapchainKHR swapChains[] = {impl->swapchain};
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = swapChains;
		presentInfo.pImageIndices = &impl->acquiredImageIndex;

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

void VulkanContext::InvalidateMaterial(const TerraEngine::UUID& materialId) {
	if (impl) {
		impl->InvalidateMaterial(materialId);
	}
}

void VulkanContext::InvalidateMesh(const TerraEngine::UUID& meshId) {
	if (impl) {
		impl->InvalidateMesh(meshId);
	}
}

void VulkanContext::InvalidateTexture(const TerraEngine::UUID& textureId) {
	if (impl) {
		impl->InvalidateTexture(textureId);
	}
}

void VulkanContext::WaitIdle() {
	if (impl && impl->device) {
		vkDeviceWaitIdle(impl->device);
	}
}

void VulkanContext::Shutdown() {
	impl->Cleanup();
}

void VulkanContext::ClearResources() {
    impl->ClearResources();
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
	h.sceneImageView = (void*)impl->sceneView.view;
	h.sceneSampler = (void*)impl->sceneView.sampler;
    h.gameImageView = (void*)impl->gameView.view;
    h.gameSampler = (void*)impl->gameView.sampler;
	h.vkGetInstanceProcAddr = (void*)vkGetInstanceProcAddr;
}

void VulkanContext::EnableEditorMode() {
	impl->InitEditorMode();
}

void VulkanContext::PresentEditor(std::function<void(void*)> renderUI) {
	if (!impl->editorMode || !impl->initialized) return;

	// Removed CPU Fence Wait. We now rely on Semaphores to synchronize Scene -> UI on the GPU.
	
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
	allocInfo.commandPool = impl->commandPools[impl->currentFrame];
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	vkAllocateCommandBuffers(impl->device, &allocInfo, &uiCmd);

	if (impl->currentFrame >= impl->extraPrimaryCommandBuffers.size()) {
		impl->extraPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	impl->extraPrimaryCommandBuffers[impl->currentFrame].push_back(uiCmd);

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
	
	// Wait for BOTH Swapchain Image (Available) AND Scene Render (RenderFinished)
	std::array<VkSemaphore, 2> waitSemaphores = {
		impl->imageAvailableSemaphores[impl->currentFrame],
		impl->renderFinishedSemaphores[impl->currentFrame]
	};
	std::array<VkPipelineStageFlags, 2> waitStages = {
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, // Swapchain available
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT // Scene done (safe to sample in UI fragment shader)
	};
	
	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
	submitInfo.pWaitSemaphores = waitSemaphores.data();
	submitInfo.pWaitDstStageMask = waitStages.data();

	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &uiCmd;

	VkSemaphore uiSemaphore = impl->uiSemaphores[imageIndex];
	
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &uiSemaphore;

	// Reset and use the main frame fence here, so BeginFrame waits for UI completion too.
	vkResetFences(impl->device, 1, &impl->inFlightFences[impl->currentFrame]);

	VkResult uiSubmitResult = vkQueueSubmit(impl->graphicsQueue, 1, &submitInfo, impl->inFlightFences[impl->currentFrame]);
	if (uiSubmitResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "PresentEditor QueueSubmit failed: " << uiSubmitResult);
		vkFreeCommandBuffers(impl->device, impl->commandPools[impl->currentFrame], 1, &uiCmd);
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

	VkResult presentResult = vkQueuePresentKHR(impl->presentQueue, &presentInfo);
	if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || impl->framebufferResized) {
		impl->framebufferResized = false;
		impl->RecreateSwapChain();
		impl->RecreateOffscreenResources();
	}

	// Removed End-of-Frame Block. We proceed optimistically.
	
	impl->currentFrame = (impl->currentFrame + 1) % impl->MAX_FRAMES_IN_FLIGHT;
}

// Impl Extensions

void VulkanContext::Impl::InitEditorMode() {
	editorMode = true;
	CreateOffscreenResources(sceneView);
    CreateOffscreenResources(gameView);
}

void VulkanContext::Impl::CreateOffscreenResources(OffscreenResources& res) {
	if (res.image != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, res.image, res.allocation);
		vkDestroyImageView(device, res.view, nullptr);
		vkDestroyFramebuffer(device, res.framebuffer, nullptr);
		vkDestroySampler(device, res.sampler, nullptr);
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

	vmaCreateImage(allocator, &imageInfo, &allocInfo, &res.image, &res.allocation, nullptr);

	// Create View
	res.view = CreateImageView(res.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

	// Create Framebuffer
	std::array<VkImageView, 2> attachments = {
		res.view,
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

	vkCreateFramebuffer(device, &framebufferInfo, nullptr, &res.framebuffer);

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

	vkCreateSampler(device, &samplerInfo, nullptr, &res.sampler);
}

void VulkanContext::Impl::RecreateOffscreenResources() {
	vkDeviceWaitIdle(device);
	CreateOffscreenResources(sceneView);
    CreateOffscreenResources(gameView);
}

void VulkanContext::Impl::ClearResources() {
    if (!device) return;
    vkDeviceWaitIdle(device);

    for (auto& [id, mesh] : meshCache) {
        vmaDestroyBuffer(allocator, mesh.indexBuffer.buffer, mesh.indexBuffer.allocation);
        vmaDestroyBuffer(allocator, mesh.vertexBuffer.buffer, mesh.vertexBuffer.allocation);
    }
    meshCache.clear();

    for (auto& [id, tex] : textureCache) {
        vkDestroySampler(device, tex.sampler, nullptr);
        vkDestroyImageView(device, tex.view, nullptr);
        vmaDestroyImage(allocator, tex.image, tex.allocation);
    }
    textureCache.clear();
    
    textureIndexMap.clear();
    freeTextureIndices.clear();
    nextTextureIndex = 1; // Index 0 is reserved for Default Texture

    materialIndexMap.clear();
    freeMaterialIndices.clear();
    nextMaterialIndex = 0;
}
