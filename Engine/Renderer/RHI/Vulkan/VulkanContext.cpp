#include "VulkanContext.h"

#define VOLK_IMPLEMENTATION
#define VMA_IMPLEMENTATION

#include <cstdio>
#include "Core/Logger/Logger.h"

#define VMA_LEAK_LOG_FORMAT(format, ...) do { \
        char _vma_buf[1024]; \
        std::snprintf(_vma_buf, sizeof(_vma_buf), (format), __VA_ARGS__); \
        TERRAENGINE_LOG(LogLevel::Error, "[VMA] " << _vma_buf); \
    } while(false)

#include "VulkanCommonIncludes.h"
#include "VulkanContextImpl.h"
#include "ThreadPool/ThreadPool.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <thread>
#include <glm/gtc/matrix_transform.hpp>

namespace
{
	glm::vec4 GetMatrixRow(const glm::mat4& matrix, int rowIndex)
	{
		return glm::vec4(matrix[0][rowIndex], matrix[1][rowIndex], matrix[2][rowIndex], matrix[3][rowIndex]);
	}

	glm::vec4 NormalizePlane(const glm::vec4& plane)
	{
		const float length = glm::length(glm::vec3(plane));
		if (length <= 1e-6f)
		{
			return glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		return plane / length;
	}

	std::array<glm::vec4, 6> BuildFrustumPlanes(const glm::mat4& viewProjection)
	{
		const glm::vec4 row0 = GetMatrixRow(viewProjection, 0);
		const glm::vec4 row1 = GetMatrixRow(viewProjection, 1);
		const glm::vec4 row2 = GetMatrixRow(viewProjection, 2);
		const glm::vec4 row3 = GetMatrixRow(viewProjection, 3);

		std::array<glm::vec4, 6> planes{};
		planes[0] = NormalizePlane(row3 + row0); // Left
		planes[1] = NormalizePlane(row3 - row0); // Right
		planes[2] = NormalizePlane(row3 + row1); // Bottom
		planes[3] = NormalizePlane(row3 - row1); // Top
		planes[4] = NormalizePlane(row3 + row2); // Near
		planes[5] = NormalizePlane(row3 - row2); // Far
		return planes;
	}

	float SafeFrameTimeMs(float deltaTimeSeconds)
	{
		if (deltaTimeSeconds <= 0.0f)
		{
			return 0.0f;
		}
		return deltaTimeSeconds * 1000.0f;
	}

	glm::vec3 SafeNormalize(const glm::vec3& value, const glm::vec3& fallback)
	{
		const float len = glm::length(value);
		if (!std::isfinite(len) || len <= 1e-5f)
		{
			return fallback;
		}
		return value / len;
	}

	bool MatricesApproximatelyEqual(const glm::mat4& lhs, const glm::mat4& rhs, float epsilon)
	{
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				if (std::abs(lhs[column][row] - rhs[column][row]) > epsilon)
				{
					return false;
				}
			}
		}
		return true;
	}

	glm::mat4 BuildDirectionalShadowViewProjection(const GPULight& light, const glm::vec3& cameraWorldPosition)
	{
		const glm::vec3 lightDirection = SafeNormalize(-glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const glm::vec3 lightPosition = cameraWorldPosition - lightDirection * 60.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, cameraWorldPosition, worldUp);
		glm::mat4 projection = glm::orthoRH_ZO(-40.0f, 40.0f, -40.0f, 40.0f, 0.1f, 180.0f);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	glm::mat4 BuildSpotShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalize(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float outerConeCos = std::clamp(light.coneAnglesAndArea.y, -0.999f, 0.999f);
		const float outerAngleRadians = std::max(0.1f, std::acos(outerConeCos) * 2.0f);
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(outerAngleRadians, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	glm::mat4 BuildAreaShadowViewProjection(const GPULight& light)
	{
		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const glm::vec3 lightDirection = SafeNormalize(glm::vec3(light.directionAndRange), glm::vec3(0.0f, -1.0f, 0.0f));
		const glm::vec3 worldUp = (std::abs(glm::dot(lightDirection, glm::vec3(0.0f, 1.0f, 0.0f))) > 0.95f)
			? glm::vec3(1.0f, 0.0f, 0.0f)
			: glm::vec3(0.0f, 1.0f, 0.0f);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const float width = std::max(light.coneAnglesAndArea.z, 0.5f);
		const float height = std::max(light.coneAnglesAndArea.w, 0.5f);
		const float fov = std::atan2(std::max(width, height), range) * 2.0f;
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + lightDirection, worldUp);
		glm::mat4 projection = glm::perspectiveRH_ZO(fov, 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}

	glm::mat4 BuildPointShadowViewProjection(const GPULight& light, uint32_t faceIndex)
	{
		static constexpr std::array<glm::vec3, 6> kFaceDirections = {
			glm::vec3(1.0f, 0.0f, 0.0f),
			glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, -1.0f)
		};
		static constexpr std::array<glm::vec3, 6> kFaceUps = {
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f),
			glm::vec3(0.0f, 0.0f, -1.0f),
			glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, -1.0f, 0.0f)
		};

		const glm::vec3 lightPosition = glm::vec3(light.positionAndType);
		const float range = std::max(light.directionAndRange.w, 0.5f);
		const uint32_t safeFace = std::min(faceIndex, static_cast<uint32_t>(kFaceDirections.size() - 1u));
		const glm::vec3 targetDirection = kFaceDirections[safeFace];
		const glm::vec3 up = kFaceUps[safeFace];
		glm::mat4 view = glm::lookAtRH(lightPosition, lightPosition + targetDirection, up);
		glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.1f, range);
		projection[1][1] *= -1.0f;
		return projection * view;
	}
}

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
	// The waited frame slot is now complete, so snapshot its GPU counters for UI stats.
	impl->PublishCompletedFrameStats(impl->currentFrame);

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

void VulkanContext::RenderFrame(const FrameRenderData& frameData, ViewType view) {
	// Skip rendering if initialization failed so we do not spam validation errors.
	if (!impl->initialized || impl->skipFrame) return;

	const uint32_t currentFrame = impl->currentFrame;

	if (!impl->editorMode && !impl->swapchainImageAcquired) {
		return;
	}

	// Handle Editor Mode resizing (lazy).
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

	// 1. Prepare All Resources (Pipelines, Meshes, Materials) BEFORE starting command recording.
	// This ensures descriptor set updates don't invalidate the recording command buffer.
	const TerraEngine::UUID defaultMeshCollectionId = ResourceManager::Instance().GetDefaultShaderCollectionID();
	const TerraEngine::UUID defaultTaskCollectionId = ResourceManager::Instance().GetDefaultTaskShaderCollectionID();
	const bool defaultTaskPipelineAvailable = impl->taskShaderSupported
		&& (ResourceManager::Instance().GetShaderCollection(defaultTaskCollectionId) != nullptr);

	VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;

	const auto PrepareDrawResources = [&](const DrawCommand& command) {
		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) return;

		impl->EnsureMesh(command.meshId);
		
		TerraEngine::UUID collectionId = material->header.shaderCollectionId;
		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}
		if (collectionId == defaultMeshCollectionId && defaultTaskPipelineAvailable) {
			collectionId = defaultTaskCollectionId;
		}

		impl->EnsurePipeline(collectionId, currentPass, material->header.flags);
		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);
		
		// Fallback for failed task pipeline
		auto passIt = impl->pipelineCache.find(currentPass);
		if (passIt != impl->pipelineCache.end()) {
			PipelineKey pipeKey = {collectionId, material->header.flags};
			if (passIt->second.find(pipeKey) == passIt->second.end() || passIt->second[pipeKey].pipeline == VK_NULL_HANDLE) {
				if (collectionId == defaultTaskCollectionId) {
					impl->EnsurePipeline(defaultMeshCollectionId, currentPass, material->header.flags);
				}
			}
		}
	};

	for (const auto& command : frameData.commands) {
		PrepareDrawResources(command);
	}

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	const bool useMainCommandBuffer = impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()
		|| impl->pendingPrimaryCommandBuffers[impl->currentFrame].empty();

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
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	if (vkBeginCommandBuffer(cmd, &beginInfo) != VK_SUCCESS) {
		return;
	}

	uint32_t imageIndex = impl->acquiredImageIndex;
	VkRenderPassBeginInfo renderPassInfo{};
	renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;

	if (!impl->editorMode) {
		renderPassInfo.renderPass = impl->renderPass;
		renderPassInfo.framebuffer = impl->framebuffers[imageIndex];
	} else {
		// Editor mode renders into per-view offscreen targets.
		renderPassInfo.renderPass = impl->offscreenRenderPass;
		renderPassInfo.framebuffer = (view == ViewType::Scene) ? impl->sceneView.framebuffer : impl->gameView.framebuffer;
	}

	renderPassInfo.renderArea.offset = {0, 0};
	renderPassInfo.renderArea.extent = impl->swapchainExtent;

	std::array<VkClearValue, 3> clearValues{};
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clearValues[1].depthStencil = {1.0f, 0};
	clearValues[2].color = {{0, 0, 0, 0}};
	renderPassInfo.clearValueCount = impl->vrsFeatures.attachmentFragmentShadingRate ? 3 : 2;
	renderPassInfo.pClearValues = clearValues.data();

	// Create Grid Pipeline if needed (lazy init).
	if (view == ViewType::Scene) {
		impl->CreateSceneViewGridPipeline(renderPassInfo.renderPass);
	}

	const auto EnsureShadowRenderResources = [&]() -> bool {
		if (impl->directionalShadowDepth.view == VK_NULL_HANDLE
			|| impl->directionalShadowColor.view == VK_NULL_HANDLE
			|| impl->spotShadowDepth.view == VK_NULL_HANDLE
			|| impl->spotShadowColor.view == VK_NULL_HANDLE
			|| impl->areaShadowDepth.view == VK_NULL_HANDLE
			|| impl->areaShadowColor.view == VK_NULL_HANDLE) {
			if (!impl->EnsureShadowRenderResources()) {
				return false;
			}
		}

		if (impl->shadowRenderPass == VK_NULL_HANDLE) {
			VkAttachmentDescription colorAttachment{};
			colorAttachment.format = impl->swapchainImageFormat;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = impl->depthFormat;
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

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

			VkSubpassDependency dependency{};
			dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
			dependency.dstSubpass = 0;
			dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
			dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
			dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
			VkRenderPassCreateInfo renderPassInfo{};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
			renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			renderPassInfo.pAttachments = attachments.data();
			renderPassInfo.subpassCount = 1;
			renderPassInfo.pSubpasses = &subpass;
			renderPassInfo.dependencyCount = 1;
			renderPassInfo.pDependencies = &dependency;
			if (vkCreateRenderPass(impl->device, &renderPassInfo, nullptr, &impl->shadowRenderPass) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Warning, "failed to create shadow render pass");
				impl->shadowRenderPass = VK_NULL_HANDLE;
				return false;
			}
		}

		const uint32_t shadowWidth = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		const uint32_t shadowHeight = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
		const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;

		auto EnsureFramebuffer = [&](VkFramebuffer& framebuffer, VkImageView colorView, VkImageView depthView, uint32_t width, uint32_t height) -> bool {
			if (framebuffer != VK_NULL_HANDLE) {
				return true;
			}
			std::array<VkImageView, 2> attachments = { colorView, depthView };
			VkFramebufferCreateInfo framebufferInfo{};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = impl->shadowRenderPass;
			framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
			framebufferInfo.pAttachments = attachments.data();
			framebufferInfo.width = width;
			framebufferInfo.height = height;
			framebufferInfo.layers = 1;
			return vkCreateFramebuffer(impl->device, &framebufferInfo, nullptr, &framebuffer) == VK_SUCCESS;
		};

		if (!EnsureFramebuffer(impl->directionalShadowFramebuffer, impl->directionalShadowColor.view, impl->directionalShadowDepth.view, shadowWidth, shadowHeight)) {
			return false;
		}
		if (!EnsureFramebuffer(impl->spotShadowFramebuffer, impl->spotShadowColor.view, impl->spotShadowDepth.view, shadowWidth, shadowHeight)) {
			return false;
		}
		if (!EnsureFramebuffer(impl->areaShadowFramebuffer, impl->areaShadowColor.view, impl->areaShadowDepth.view, shadowWidth, shadowHeight)) {
			return false;
		}
		for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
			if (impl->pointShadowColorFaceViews[faceIndex] == VK_NULL_HANDLE || impl->pointShadowDepthFaceViews[faceIndex] == VK_NULL_HANDLE) {
				return false;
			}
			if (!EnsureFramebuffer(impl->pointShadowFaceFramebuffers[faceIndex], impl->pointShadowColorFaceViews[faceIndex], impl->pointShadowDepthFaceViews[faceIndex], pointShadowSize, pointShadowSize)) {
				return false;
			}
		}

		return true;
	};

	const bool shadowResourcesReady = EnsureShadowRenderResources();

	const uint32_t statsSlotIndex = impl->GetStatsSlotIndex(impl->currentFrame, view);
	std::vector<GPULight> lightingPayload = frameData.lights;
	for (GPULight& light : lightingPayload) {
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		const bool castsShadows = light.shadowAndPadding.x > 0.5f;
		light.shadowAndPadding.y = -1.0f;
		light.shadowAndPadding.z = static_cast<float>(impl->ltcAmplitudeTextureIndex);
		light.shadowAndPadding.w = static_cast<float>(impl->ltcInverseTextureIndex);
		if (!castsShadows) {
			continue;
		}
		if (lightType == 0u) {
			light.shadowAndPadding.y = static_cast<float>(impl->directionalShadowTextureIndex);
		}
		else if (lightType == 2u) {
			light.shadowAndPadding.y = static_cast<float>(impl->spotShadowTextureIndex);
		}
		else if (lightType == 3u) {
			light.shadowAndPadding.y = static_cast<float>(impl->areaShadowTextureIndex);
		}
	}
	impl->UploadLightingData(lightingPayload);
	FrameCullingDataGPU cullingData{};
	cullingData.frustumPlanes = BuildFrustumPlanes(frameData.projectionMatrix * frameData.viewMatrix);
	cullingData.cameraPositionAndEpsilon = glm::vec4(frameData.cameraWorldPosition, std::max(frameData.cullingEpsilon, 0.0001f));
	cullingData.clusterGridAndLightCount = glm::uvec4(
		kLightingClusterDimX,
		kLightingClusterDimY,
		kLightingClusterDimZ,
		static_cast<uint32_t>(std::min<size_t>(lightingPayload.size(), VulkanContext::Impl::MAX_GLOBAL_LIGHTS)));
	cullingData.clusterDepthAndViewport = glm::vec4(
		0.1f,
		100.0f,
		static_cast<float>(std::max<uint32_t>(impl->swapchainExtent.width, 1u)),
		static_cast<float>(std::max<uint32_t>(impl->swapchainExtent.height, 1u)));
	cullingData.viewMatrix = frameData.viewMatrix;
	cullingData.invProjection = glm::inverse(frameData.projectionMatrix);
	cullingData.renderSettings0 = glm::uvec4(
		frameData.settings.shadowResolution,
		frameData.settings.shadowFilteringSize,
		frameData.settings.maxDynamicLights,
		frameData.settings.enableLTC ? 1u : 0u);
	cullingData.renderSettings1 = glm::uvec4(
		frameData.settings.enableIBL ? 1u : 0u,
		frameData.settings.anisotropicFiltering,
		0u, 0u);
	cullingData.globalTaskCullingEnabled = (frameData.globalTaskShaderCullingEnabled && impl->globalTaskShaderCullingEnabled) ? 1u : 0u;
	cullingData.taskShaderSupported = impl->taskShaderSupported ? 1u : 0u;
	impl->UpdateFrameCullingData(statsSlotIndex, cullingData);
	// Compute lighting shaders index slot 0 explicitly, so mirror the latest frame
	// culling payload there in addition to the per-view stats slot.
	impl->UpdateFrameCullingData(0u, cullingData);
	impl->ResetMeshletCounters(statsSlotIndex);

	// Prepare Indirect Buffer
	std::vector<VkDrawMeshTasksIndirectCommandEXT> indirectCommands;
	indirectCommands.reserve(frameData.commands.size());

	const auto EnsureLightingComputePipelines = [&]() {
		if (impl->lightingComputePipelineLayout == VK_NULL_HANDLE) {
			VkPushConstantRange pushRange{};
			pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			pushRange.offset = 0;
			pushRange.size = 64; // Plenty of room for various compute push constants

			VkPipelineLayoutCreateInfo layoutInfo{};
			layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			layoutInfo.setLayoutCount = 1;
			layoutInfo.pSetLayouts = &impl->globalDescriptorSetLayout;
			layoutInfo.pushConstantRangeCount = 1;
			layoutInfo.pPushConstantRanges = &pushRange;
			if (vkCreatePipelineLayout(impl->device, &layoutInfo, nullptr, &impl->lightingComputePipelineLayout) != VK_SUCCESS) {
				impl->lightingComputePipelineLayout = VK_NULL_HANDLE;
				return;
			}
		}

		auto LoadShaderModule = [&](const char* path) -> VkShaderModule {
			std::ifstream file(path, std::ios::ate | std::ios::binary);
			if (!file.is_open()) {
				return VK_NULL_HANDLE;
			}
			const size_t fileSize = static_cast<size_t>(file.tellg());
			if (fileSize == 0 || (fileSize % sizeof(uint32_t)) != 0) {
				return VK_NULL_HANDLE;
			}
			std::vector<uint32_t> code(fileSize / sizeof(uint32_t));
			file.seekg(0);
			file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(fileSize));
			file.close();

			VkShaderModuleCreateInfo moduleInfo{};
			moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			moduleInfo.codeSize = fileSize;
			moduleInfo.pCode = code.data();
			VkShaderModule module = VK_NULL_HANDLE;
			if (vkCreateShaderModule(impl->device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
				return VK_NULL_HANDLE;
			}
			return module;
		};

		auto CreateComputePipeline = [&](VkPipeline& pipeline, const char* shaderPath) {
			if (pipeline != VK_NULL_HANDLE || impl->lightingComputePipelineLayout == VK_NULL_HANDLE) {
				return;
			}
			VkShaderModule module = LoadShaderModule(shaderPath);
			if (module == VK_NULL_HANDLE) {
				return;
			}

			VkPipelineShaderStageCreateInfo stageInfo{};
			stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
			stageInfo.module = module;
			stageInfo.pName = "main";

			VkComputePipelineCreateInfo pipelineInfo{};
			pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
			pipelineInfo.stage = stageInfo;
			pipelineInfo.layout = impl->lightingComputePipelineLayout;
			if (vkCreateComputePipelines(impl->device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
				pipeline = VK_NULL_HANDLE;
			}
			vkDestroyShaderModule(impl->device, module, nullptr);
		};

		CreateComputePipeline(impl->clusterGridComputePipeline, "Shaders/Lighting/ClusterGrid.comp.spv");
		CreateComputePipeline(impl->clusterDepthMaskComputePipeline, "Shaders/Lighting/ClusterDepthMask.comp.spv");
		CreateComputePipeline(impl->lightCullComputePipeline, "Shaders/Lighting/LightCull.comp.spv");
		CreateComputePipeline(impl->hiZComputePipeline, "Editor/Shaders/PostProcess/HiZGenerate.comp.spv");
		CreateComputePipeline(impl->vrsComputePipeline, "Editor/Shaders/PostProcess/VRSGenerate.comp.spv");
	};

	EnsureLightingComputePipelines();

	// Hi-Z Generation
	impl->GenerateHiZPyramid(cmd, impl->depthImageView);

	// Light Culling
	if (impl->clusterGridComputePipeline != VK_NULL_HANDLE && impl->lightCullComputePipeline != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		
		uint32_t clusterGroupCountX = (kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ + 63) / 64;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterGridComputePipeline);
		vkCmdDispatch(cmd, clusterGroupCountX, 1, 1);

		VkMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);

		if (impl->clusterDepthMaskComputePipeline != VK_NULL_HANDLE) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterDepthMaskComputePipeline);
			vkCmdDispatch(cmd, (kLightingClusterDimX + 7) / 8, (kLightingClusterDimY + 7) / 8, 1);
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightCullComputePipeline);
		// Note: LightCull needs access to depth buffer for masking
		vkCmdDispatch(cmd, clusterGroupCountX, 1, 1);

		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
	}

	// Prepare draw data.
	struct PreparedDraw {
		VulkanMesh* mesh = nullptr;
		VulkanPipeline* pipeline = nullptr;
		std::vector<VkDescriptorSet> descriptorSets;
		PushConstants push{};
		uint32_t indirectBufferIndex = 0;
	};
	std::vector<PreparedDraw> prepared;
	prepared.reserve(frameData.commands.size());
	std::vector<PreparedDraw> preparedShadow;
	preparedShadow.reserve(frameData.commands.size());

	RenderStatsSnapshot pendingStats{};
	pendingStats.viewType = (view == ViewType::Scene) ? RenderViewType::Scene : RenderViewType::Game;
	pendingStats.frameTimeMs = SafeFrameTimeMs(frameData.deltaTimeSeconds);
	pendingStats.fps = pendingStats.frameTimeMs > 0.0f ? (1000.0f / pendingStats.frameTimeMs) : 0.0f;
	pendingStats.valid = true;

	const auto ResolvePipeline = [&](const TerraEngine::UUID& shaderCollectionId, VkRenderPass currentPass, uint32_t materialFlags) -> VulkanPipeline* {
		impl->EnsurePipeline(shaderCollectionId, currentPass, materialFlags);
		auto passIt = impl->pipelineCache.find(currentPass);
		if (passIt == impl->pipelineCache.end()) {
			return nullptr;
		}

		PipelineKey pipeKey = {shaderCollectionId, materialFlags};
		auto pipelineIt = passIt->second.find(pipeKey);
		if (pipelineIt == passIt->second.end()
			|| pipelineIt->second.pipeline == VK_NULL_HANDLE
			|| pipelineIt->second.layout == VK_NULL_HANDLE) {
			return nullptr;
		}

		return &pipelineIt->second;
	};

	for (const auto& command : frameData.commands) {
		Material* material = ResourceManager::Instance().GetMaterial(command.materialId);
		if (!material) continue;

		impl->EnsureMesh(command.meshId);
		auto meshIt = impl->meshCache.find(command.meshId);
		if (meshIt == impl->meshCache.end()) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because mesh GPU buffers are unavailable.");
			continue;
		}

		VkRenderPass currentPass = impl->editorMode ? impl->offscreenRenderPass : impl->renderPass;
		uint32_t materialFlags = material->header.flags;
		VulkanMesh& mesh = meshIt->second;

		if (mesh.meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
			|| mesh.indexBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletBuffer.buffer == VK_NULL_HANDLE
			|| mesh.meshletCount == 0) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because meshlet payload buffers are unavailable.");
			continue;
		}

		TerraEngine::UUID collectionId = material->header.shaderCollectionId;
		if (collectionId == TerraEngine::EMPTY_UUID) {
			collectionId = defaultMeshCollectionId;
		}

		// Route default materials through the task pipeline whenever supported.
		if (collectionId == defaultMeshCollectionId && defaultTaskPipelineAvailable) {
			collectionId = defaultTaskCollectionId;
		}

		VulkanPipeline* pipeline = ResolvePipeline(collectionId, currentPass, materialFlags);
		if (pipeline == nullptr && collectionId == defaultTaskCollectionId) {
			collectionId = defaultMeshCollectionId;
			pipeline = ResolvePipeline(collectionId, currentPass, materialFlags);
		}
		if (pipeline == nullptr) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because graphics pipeline creation failed.");
			continue;
		}

		impl->EnsureMaterialDescriptorSets(collectionId, command.materialId);
		VkDescriptorSet meshDescriptorSet = impl->EnsureMeshDescriptorSet(command.meshId, *pipeline);
		if (meshDescriptorSet == VK_NULL_HANDLE) {
			TERRAENGINE_LOG(LogLevel::Warning, "Skipping draw because mesh shader descriptor bindings were not created.");
			continue;
		}

		const bool activeTaskCulling = pipeline->hasTaskStage
			&& frameData.globalTaskShaderCullingEnabled
			&& impl->globalTaskShaderCullingEnabled
			&& command.taskShaderCullingEnabled
			&& mesh.hasMeshletBounds;

		PushConstants push{};
		push.viewProj = frameData.projectionMatrix * frameData.viewMatrix;
		push.model = command.modelMatrix;
		push.textureIndex = 0;
		if (material->header.textureCount > 0 && !material->textures.empty()) {
			TerraEngine::UUID texID = material->textures[0].textureUuid;
			if (impl->textureIndexMap.contains(texID)) {
				push.textureIndex = impl->textureIndexMap[texID];
			}
		}
		push.materialIndex = 0;
		if (impl->materialIndexMap.contains(command.materialId)) {
			push.materialIndex = impl->materialIndexMap[command.materialId];
		}
		push.meshDataIndex = mesh.bindlessIndex;
		push.textureIndex = 0;
		if (material->header.textureCount > 0 && !material->textures.empty()) {
			TerraEngine::UUID texID = material->textures[0].textureUuid;
			if (impl->textureIndexMap.contains(texID)) {
				push.textureIndex = impl->textureIndexMap[texID];
			}
		}
		push.vrsActive = 0u; 
		push.taskCullingEnabled = activeTaskCulling ? 1u : 0u;
		push.statsSlotIndex = statsSlotIndex;
		push.cullingEpsilon = std::max(frameData.cullingEpsilon, 0.0001f);

		PreparedDraw draw{};
		draw.mesh = &mesh;
		draw.pipeline = pipeline;
		// Set 0 is the global bindless heap; set 1 is per-mesh payload for mesh shader pipelines.
		draw.descriptorSets = {impl->globalDescriptorSets[currentFrame]};
		draw.descriptorSets.push_back(meshDescriptorSet);
		draw.push = push;
		
		const uint32_t kTaskBatchSize = 32u;
		const uint32_t taskGroupCount = pipeline->hasTaskStage
			? ((mesh.meshletCount + (kTaskBatchSize - 1u)) / kTaskBatchSize)
			: mesh.meshletCount;

		VkDrawMeshTasksIndirectCommandEXT indirectCmd{};
		indirectCmd.groupCountX = taskGroupCount;
		indirectCmd.groupCountY = 1;
		indirectCmd.groupCountZ = 1;
		draw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
		indirectCommands.push_back(indirectCmd);

		prepared.push_back(std::move(draw));

		if (shadowResourcesReady) {
			TerraEngine::UUID shadowCollectionId = collectionId;
			VulkanPipeline* shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			if (shadowPipeline == nullptr && shadowCollectionId == defaultTaskCollectionId) {
				shadowCollectionId = defaultMeshCollectionId;
				shadowPipeline = ResolvePipeline(shadowCollectionId, impl->shadowRenderPass, materialFlags);
			}

			if (shadowPipeline != nullptr) {
				VkDescriptorSet shadowMeshDescriptorSet = impl->EnsureMeshDescriptorSet(command.meshId, *shadowPipeline);
				if (shadowMeshDescriptorSet != VK_NULL_HANDLE) {
					PreparedDraw shadowDraw{};
					shadowDraw.mesh = &mesh;
					shadowDraw.pipeline = shadowPipeline;
					shadowDraw.descriptorSets = {impl->globalDescriptorSets[currentFrame], shadowMeshDescriptorSet};
					shadowDraw.push = push;
					// Shadow-map passes never use the cluster heatmap debug branch.
					shadowDraw.push.taskCullingEnabled = 0u;
					
					VkDrawMeshTasksIndirectCommandEXT shadowIndirectCmd{};
					shadowIndirectCmd.groupCountX = taskGroupCount;
					shadowIndirectCmd.groupCountY = 1;
					shadowIndirectCmd.groupCountZ = 1;
					shadowDraw.indirectBufferIndex = static_cast<uint32_t>(indirectCommands.size());
					indirectCommands.push_back(shadowIndirectCmd);

					preparedShadow.push_back(std::move(shadowDraw));
				}
			}
		}

		pendingStats.drawCalls += 1u;
		pendingStats.triangleCount += (mesh.indexCount / 3u);
		pendingStats.meshletsTotal += mesh.meshletCount;
		pendingStats.taskCullingActive = pendingStats.taskCullingActive || activeTaskCulling;

		// Mesh-only and pass-through fallback paths keep conservative CPU estimates.
		if (!pipeline->hasTaskStage || !activeTaskCulling) {
			pendingStats.meshletsDispatched += mesh.meshletCount;
		}
	}

	impl->RecordPendingStats(statsSlotIndex, pendingStats);

	// If this is a subsequent pass targeting the swapchain (multi-camera),
	// inject a barrier to wait for the previous pass to finish its writes.
	if (!useMainCommandBuffer && !impl->editorMode) {
		VkImageMemoryBarrier barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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

	// Build/refresh clustered light lists with compute before recording the
	// graphics render pass so fragment shading reads the latest culling output.
	if (impl->lightingComputePipelineLayout != VK_NULL_HANDLE
		&& impl->clusterGridComputePipeline != VK_NULL_HANDLE
		&& impl->lightCullComputePipeline != VK_NULL_HANDLE) {
		const glm::mat4 currentViewProjection = frameData.projectionMatrix * frameData.viewMatrix;
		const bool cameraChanged = !impl->previousClusterViewProjectionValid
			|| !MatricesApproximatelyEqual(currentViewProjection, impl->previousClusterViewProjection, 1e-4f);

		if (cameraChanged) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->clusterGridComputePipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
			const uint32_t dispatchX = (kLightingClusterDimX + 7u) / 8u;
			const uint32_t dispatchY = (kLightingClusterDimY + 7u) / 8u;
			const uint32_t dispatchZ = std::max<uint32_t>(kLightingClusterDimZ, 1u);
			vkCmdDispatch(cmd, dispatchX, dispatchY, dispatchZ);

			VkMemoryBarrier gridBarrier{};
			gridBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
			gridBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
			gridBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
			vkCmdPipelineBarrier(
				cmd,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				0,
				1, &gridBarrier,
				0, nullptr,
				0, nullptr
			);

			impl->previousClusterViewProjection = currentViewProjection;
			impl->previousClusterViewProjectionValid = true;
		}

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightCullComputePipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, impl->lightingComputePipelineLayout, 0, 1, &impl->globalDescriptorSets[currentFrame], 0, nullptr);
		const uint32_t clusterCount = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
		vkCmdDispatch(cmd, std::max<uint32_t>(clusterCount, 1u), 1u, 1u);

		VkMemoryBarrier lightCullBarrier{};
		lightCullBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		lightCullBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		lightCullBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT,
			0,
			1, &lightCullBarrier,
			0, nullptr,
			0, nullptr
		);
	}

	const GPULight* directionalShadowLight = nullptr;
	const GPULight* pointShadowLight = nullptr;
	const GPULight* spotShadowLight = nullptr;
	const GPULight* areaShadowLight = nullptr;
	for (const GPULight& light : lightingPayload) {
		if (light.shadowAndPadding.x <= 0.5f) {
			continue;
		}
		const uint32_t lightType = static_cast<uint32_t>(std::max(0.0f, light.positionAndType.w));
		if (lightType == 0u && directionalShadowLight == nullptr) {
			directionalShadowLight = &light;
		}
		else if (lightType == 1u && pointShadowLight == nullptr) {
			pointShadowLight = &light;
		}
		else if (lightType == 2u && spotShadowLight == nullptr) {
			spotShadowLight = &light;
		}
		else if (lightType == 3u && areaShadowLight == nullptr) {
			areaShadowLight = &light;
		}
	}

	auto RecordShadowDraws = [&](const glm::mat4& lightViewProjection) {
		for (const PreparedDraw& draw : preparedShadow) {
			if (draw.pipeline == nullptr
				|| draw.pipeline->pipeline == VK_NULL_HANDLE
				|| draw.pipeline->layout == VK_NULL_HANDLE
				|| draw.mesh == nullptr
				|| draw.mesh->meshletCount == 0) {
				continue;
			}

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);

			PushConstants shadowPush = draw.push;
			shadowPush.viewProj = lightViewProjection;
			if (draw.pipeline->supportsDefaultPushConstants) {
				vkCmdPushConstants(cmd, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants), &shadowPush);
			}

			VkDeviceSize offset = draw.indirectBufferIndex * sizeof(VkDrawMeshTasksIndirectCommandEXT);
			vkCmdDrawMeshTasksIndirectEXT(cmd, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
		}
	};

	auto RecordShadowPass = [&](VkFramebuffer framebuffer, uint32_t width, uint32_t height, const glm::mat4& lightViewProjection) -> bool {
		if (framebuffer == VK_NULL_HANDLE || impl->shadowRenderPass == VK_NULL_HANDLE) {
			return false;
		}

		VkRenderPassBeginInfo shadowPassInfo{};
		shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		shadowPassInfo.renderPass = impl->shadowRenderPass;
		shadowPassInfo.framebuffer = framebuffer;
		shadowPassInfo.renderArea.offset = { 0, 0 };
		shadowPassInfo.renderArea.extent = { width, height };

		VkClearValue clearValues[2];
		clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
		clearValues[1].depthStencil = {1.0f, 0};
		shadowPassInfo.clearValueCount = 2;
		shadowPassInfo.pClearValues = clearValues;

		vkCmdBeginRenderPass(cmd, &shadowPassInfo, VK_SUBPASS_CONTENTS_INLINE);
		RecordShadowDraws(lightViewProjection);
		vkCmdEndRenderPass(cmd);
		return true;
	};

	bool recordedAnyShadowPass = false;
	const uint32_t shadowWidth = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
	const uint32_t shadowHeight = VulkanContext::Impl::SHADOW_MAP_RESOLUTION;
	const uint32_t pointShadowSize = VulkanContext::Impl::POINT_SHADOW_MAP_RESOLUTION;
	if (shadowResourcesReady && impl->shadowRenderPass != VK_NULL_HANDLE && !preparedShadow.empty()) {
		if (directionalShadowLight != nullptr) {
			const glm::mat4 lightViewProjection = BuildDirectionalShadowViewProjection(*directionalShadowLight, frameData.cameraWorldPosition);
			recordedAnyShadowPass = RecordShadowPass(impl->directionalShadowFramebuffer, shadowWidth, shadowHeight, lightViewProjection) || recordedAnyShadowPass;
		}
		if (pointShadowLight != nullptr) {
			for (uint32_t faceIndex = 0u; faceIndex < 6u; ++faceIndex) {
				const glm::mat4 lightViewProjection = BuildPointShadowViewProjection(*pointShadowLight, faceIndex);
				recordedAnyShadowPass = RecordShadowPass(impl->pointShadowFaceFramebuffers[faceIndex], pointShadowSize, pointShadowSize, lightViewProjection) || recordedAnyShadowPass;
			}
		}
		if (spotShadowLight != nullptr) {
			const glm::mat4 lightViewProjection = BuildSpotShadowViewProjection(*spotShadowLight);
			recordedAnyShadowPass = RecordShadowPass(impl->spotShadowFramebuffer, shadowWidth, shadowHeight, lightViewProjection) || recordedAnyShadowPass;
		}
		if (areaShadowLight != nullptr) {
			const glm::mat4 lightViewProjection = BuildAreaShadowViewProjection(*areaShadowLight);
			recordedAnyShadowPass = RecordShadowPass(impl->areaShadowFramebuffer, shadowWidth, shadowHeight, lightViewProjection) || recordedAnyShadowPass;
		}
	}

	if (recordedAnyShadowPass) {
		VkMemoryBarrier shadowBarrier{};
		shadowBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		shadowBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		shadowBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(
			cmd,
			VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0,
			1, &shadowBarrier,
			0, nullptr,
			0, nullptr
		);
	}

	if (!indirectCommands.empty()) {
		void* data = nullptr;
		vmaMapMemory(impl->allocator, impl->indirectDrawBuffers[impl->currentFrame].allocation, &data);
		std::memcpy(data, indirectCommands.data(), indirectCommands.size() * sizeof(VkDrawMeshTasksIndirectCommandEXT));
		vmaUnmapMemory(impl->allocator, impl->indirectDrawBuffers[impl->currentFrame].allocation);
	}

	std::vector<VkCommandBuffer> secondaryBuffers;
	if (!prepared.empty()) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
		const size_t chunkSize = (prepared.size() + workerCount - 1) / workerCount;

		impl->EnsureWorkerPools(impl->currentFrame, workerCount);
		std::vector<std::future<VkCommandBuffer>> futures;
		futures.reserve(workerCount);

		for (size_t taskIndex = 0; taskIndex < workerCount; ++taskIndex) {
			const size_t start = taskIndex * chunkSize;
			if (start >= prepared.size()) break;
			const size_t end = std::min(prepared.size(), start + chunkSize);
			VkFramebuffer framebuffer = renderPassInfo.framebuffer;
			VkRenderPass renderPass = renderPassInfo.renderPass;
			const uint32_t frameIndex = currentFrame;
			VkCommandBuffer secondary = impl->AcquireWorkerCommandBuffer(frameIndex, taskIndex);

			auto task = ThreadPool::instance().createTask([secondary, renderPass, framebuffer, start, end, &prepared, this]() -> VkCommandBuffer {
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
					if (draw.pipeline == nullptr
						|| draw.pipeline->pipeline == VK_NULL_HANDLE
						|| draw.pipeline->layout == VK_NULL_HANDLE
						|| draw.mesh == nullptr) {
						continue;
					}
					if (draw.mesh->meshShaderVertexBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletVertexRemapBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshDecodeInfoBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->indexBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletBuffer.buffer == VK_NULL_HANDLE
						|| draw.mesh->meshletCount == 0) {
						continue;
					}

					vkCmdBindPipeline(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->pipeline);
					vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout, 0, static_cast<uint32_t>(draw.descriptorSets.size()), draw.descriptorSets.data(), 0, nullptr);
					if (draw.pipeline->supportsDefaultPushConstants) {
						vkCmdPushConstants(secondary, draw.pipeline->layout, draw.pipeline->defaultPushConstantStages, 0, sizeof(PushConstants), &draw.push);
					}

					VkDeviceSize offset = draw.indirectBufferIndex * sizeof(VkDrawMeshTasksIndirectCommandEXT);
					vkCmdDrawMeshTasksIndirectEXT(secondary, impl->indirectDrawBuffers[impl->currentFrame].buffer, offset, 1, sizeof(VkDrawMeshTasksIndirectCommandEXT));
				}
				vkEndCommandBuffer(secondary);
				return secondary;
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		for (auto& future : futures) {
			VkCommandBuffer secondary = future.get();
			if (secondary != VK_NULL_HANDLE) {
				secondaryBuffers.push_back(secondary);
			}
		}
	}

	// Render grid (Scene View only).
	if (view == ViewType::Scene) {
		const size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
		const size_t workerCount = std::min(prepared.size(), hw);
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
			
				// VRS Generation for next frame
				impl->GenerateVRSImage(cmd, (view == ViewType::Scene) ? impl->sceneView.view : impl->gameView.view);
			
				vkEndCommandBuffer(cmd);
				// Record the command buffer to be submitted at EndFrame so multiple cameras
	// render sequentially before a single present.
	if (impl->currentFrame >= impl->pendingPrimaryCommandBuffers.size()) {
		impl->pendingPrimaryCommandBuffers.resize(impl->MAX_FRAMES_IN_FLIGHT);
	}
	impl->pendingPrimaryCommandBuffers[impl->currentFrame].push_back(cmd);
}

void VulkanContext::Impl::GenerateHiZPyramid(VkCommandBuffer cmd, VkImageView depthView) {
	if (hiZ.image == VK_NULL_HANDLE || hiZComputePipeline == VK_NULL_HANDLE) return;

	// 1. Transition main depth buffer to DEPTH_STENCIL_READ_ONLY_OPTIMAL so sampled depth reads are valid.
	VkImageMemoryBarrier depthBarrier{};
	depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.image = depthImage;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.subresourceRange.baseMipLevel = 0;
	depthBarrier.subresourceRange.levelCount = 1;
	depthBarrier.subresourceRange.baseArrayLayer = 0;
	depthBarrier.subresourceRange.layerCount = 1;
	depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hiZComputePipeline);

	// 2. Generate Level 0 (Convert D32 to R32F)
	{
		VkImageMemoryBarrier hizBarrier{};
		hizBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.image = hiZ.image;
		hizBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		hizBarrier.subresourceRange.baseMipLevel = 0;
		hizBarrier.subresourceRange.levelCount = 1;
		hizBarrier.srcAccessMask = 0;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);

		VkDescriptorImageInfo depthInfo{};
		depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
		depthInfo.imageView = depthImageView;
		depthInfo.sampler = VK_NULL_HANDLE;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = hiZ.mipViews[0];

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[currentFrame];
		writes[0].dstBinding = 12;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1;
		writes[0].pImageInfo = &depthInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[currentFrame];
		writes[1].dstBinding = 16;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1;
		writes[1].pImageInfo = &outputInfo;

		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

		struct { uint32_t w, h, isL0; } push;
		push.w = hiZ.width; push.h = hiZ.height; push.isL0 = 1;
		vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, (hiZ.width + 7) / 8, (hiZ.height + 7) / 8, 1);

		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		hizBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);
	}

	// 3. Generate Level 1..N
	for (uint32_t i = 1; i < hiZ.mipLevels; ++i) {
		uint32_t mipW = std::max(1u, hiZ.width >> i);
		uint32_t mipH = std::max(1u, hiZ.height >> i);

		VkImageMemoryBarrier hizBarrier{};
		hizBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.image = hiZ.image;
		hizBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		hizBarrier.subresourceRange.baseMipLevel = i;
		hizBarrier.subresourceRange.levelCount = 1;
		hizBarrier.srcAccessMask = 0;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);

		VkDescriptorImageInfo inputInfo{};
		inputInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		inputInfo.imageView = hiZ.mipViews[i - 1];
		inputInfo.sampler = hiZ.sampler;

		VkDescriptorImageInfo outputInfo{};
		outputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		outputInfo.imageView = hiZ.mipViews[i];

		std::array<VkWriteDescriptorSet, 2> writes{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = globalDescriptorSets[currentFrame];
		writes[0].dstBinding = 12;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].descriptorCount = 1;
		writes[0].pImageInfo = &inputInfo;

		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = globalDescriptorSets[currentFrame];
		writes[1].dstBinding = 16;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[1].descriptorCount = 1;
		writes[1].pImageInfo = &outputInfo;

		vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);

		struct { uint32_t w, h, isL0; } push;
		push.w = mipW; push.h = mipH; push.isL0 = 0;
		vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
		vkCmdDispatch(cmd, (mipW + 7) / 8, (mipH + 7) / 8, 1);

		hizBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		hizBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		hizBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
		hizBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &hizBarrier);
	}

	// 4. Keep depth in read-only layout so the subsequent light-culling compute pass can sample it.
	depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	depthBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	depthBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

	VkDescriptorImageInfo restoreDepthInfo{};
	// Keep the descriptor in read-only layout because binding 12 is sampled by compute passes.
	restoreDepthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
	restoreDepthInfo.imageView = depthImageView;
	restoreDepthInfo.sampler = hiZ.sampler;

	VkWriteDescriptorSet restoreWrite{};
	restoreWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	restoreWrite.dstSet = globalDescriptorSets[currentFrame];
	restoreWrite.dstBinding = 12;
	restoreWrite.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	restoreWrite.descriptorCount = 1;
	restoreWrite.pImageInfo = &restoreDepthInfo;
	vkUpdateDescriptorSets(device, 1, &restoreWrite, 0, nullptr);
}

void VulkanContext::Impl::GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView) {
	if (vrsComputePipeline == VK_NULL_HANDLE || vrs.image == VK_NULL_HANDLE) return;

	// Transition VRS to GENERAL
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.oldLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.image = vrs.image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.baseArrayLayer = 0;
	barrier.subresourceRange.layerCount = 1;
	barrier.srcAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;

	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	// Update descriptor set with current color view and VRS image
	VkDescriptorImageInfo colorInfo{};
	colorInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	colorInfo.imageView = colorView;
	colorInfo.sampler = hiZ.sampler;

	VkDescriptorImageInfo vrsOutputInfo{};
	vrsOutputInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	vrsOutputInfo.imageView = vrs.view;

	std::array<VkWriteDescriptorSet, 2> vrsWrites{};
	vrsWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[0].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[0].dstBinding = 14;
	vrsWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	vrsWrites[0].descriptorCount = 1;
	vrsWrites[0].pImageInfo = &vrsOutputInfo;

	vrsWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	vrsWrites[1].dstSet = globalDescriptorSets[currentFrame];
	vrsWrites[1].dstBinding = 15;
	vrsWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	vrsWrites[1].descriptorCount = 1;
	vrsWrites[1].pImageInfo = &colorInfo;

	vkUpdateDescriptorSets(device, static_cast<uint32_t>(vrsWrites.size()), vrsWrites.data(), 0, nullptr);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vrsComputePipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lightingComputePipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

	struct {
		uint32_t width;
		uint32_t height;
		uint32_t texelSize;
	} push;
	push.width = swapchainExtent.width;
	push.height = swapchainExtent.height;
	push.texelSize = vrs.texelSize.width;
	vkCmdPushConstants(cmd, lightingComputePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);

	vkCmdDispatch(cmd, (vrs.width + 7) / 8, (vrs.height + 7) / 8, 1);

	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0, 0, nullptr, 0, nullptr, 1, &barrier);
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
			TERRAENGINE_LOG(LogLevel::Error, "[Vulkan] QueueSubmit failed! Result: " << submitResult << ". Recreating swapchain to recover state.");
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

void VulkanContext::SetGlobalTaskShaderCullingEnabled(bool enabled) {
	if (impl) {
		impl->globalTaskShaderCullingEnabled = enabled;
	}
}

bool VulkanContext::IsTaskShaderSupported() const {
	return impl != nullptr && impl->taskShaderSupported;
}

RenderStatsSnapshot VulkanContext::GetLatestRenderStats(RenderViewType viewType) const {
	if (!impl) {
		RenderStatsSnapshot snapshot{};
		snapshot.viewType = viewType;
		return snapshot;
	}
	return impl->GetLatestStats(viewType);
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

void VulkanContext::InvalidateResource(const TerraEngine::UUID& id) {
	if (impl) {
		impl->InvalidateResource(id);
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

	std::array<VkClearValue, 3> clearValues{};
	clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
	clearValues[1].depthStencil = {1.0f, 0};
	clearValues[2].color = {{0, 0, 0, 0}};

	renderPassInfo.clearValueCount = impl->vrsFeatures.attachmentFragmentShadingRate ? 3 : 2;
	renderPassInfo.pClearValues = clearValues.data();

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
	if (editorMode) return;
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
		if (vrsFeatures.attachmentFragmentShadingRate) {
			VkAttachmentDescription2 colorAttachment{};
			colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference2 colorAttachmentRef{};
			colorAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription2 depthAttachment{};
			depthAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference2 depthAttachmentRef{};
			depthAttachmentRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
			depthAttachmentRef.attachment = 1;
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
			vrsAttachmentRef.attachment = 2;
			vrsAttachmentRef.layout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;

			VkFragmentShadingRateAttachmentInfoKHR vrsInfo{};
			vrsInfo.sType = VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
			vrsInfo.pFragmentShadingRateAttachment = &vrsAttachmentRef;
			vrsInfo.shadingRateAttachmentTexelSize = vrs.texelSize;

			VkSubpassDescription2 subpass{};
			subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;
			subpass.pNext = &vrsInfo;

			std::array<VkSubpassDependency2, 1> dependencies{};
			dependencies[0].sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
			dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
			dependencies[0].dstSubpass = 0;
			dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].srcAccessMask = 0;
			dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
			dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

			std::array<VkAttachmentDescription2, 3> attachments = { colorAttachment, depthAttachment, vrsAttachment };
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
			colorAttachment.format = VK_FORMAT_R8G8B8A8_UNORM;
			colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkAttachmentReference colorAttachmentRef{};
			colorAttachmentRef.attachment = 0;
			colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			VkAttachmentDescription depthAttachment{};
			depthAttachment.format = FindDepthFormat();
			depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkAttachmentReference depthAttachmentRef{};
			depthAttachmentRef.attachment = 1;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

			VkSubpassDescription subpass{};
			subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1;
			subpass.pColorAttachments = &colorAttachmentRef;
			subpass.pDepthStencilAttachment = &depthAttachmentRef;

			std::array<VkSubpassDependency, 2> dependencies{};
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

			std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };
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
	vmaSetAllocationName(allocator, res.allocation, "Offscreen Render Target Image");

	// Create View
	res.view = CreateImageView(res.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

	// Create Framebuffer
	std::vector<VkImageView> fbAttachments = {
		res.view,
		depthImageView
	};
	if (vrsFeatures.attachmentFragmentShadingRate && vrs.view != VK_NULL_HANDLE) {
		fbAttachments.push_back(vrs.view);
	}
	
	VkFramebufferCreateInfo framebufferInfo{};
	framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebufferInfo.renderPass = offscreenRenderPass;
	framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
	framebufferInfo.pAttachments = fbAttachments.data();
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
		vmaDestroyBuffer(allocator, mesh.meshDecodeInfoBuffer.buffer, mesh.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBuffer.buffer, mesh.meshletBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

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

	DestroyFramebuffer(directionalShadowFramebuffer);
	DestroyFramebuffer(spotShadowFramebuffer);
	DestroyFramebuffer(areaShadowFramebuffer);
	for (VkFramebuffer& framebuffer : pointShadowFaceFramebuffers) {
		DestroyFramebuffer(framebuffer);
	}

	for (VkImageView& faceView : pointShadowDepthFaceViews) {
		if (faceView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, faceView, nullptr);
			faceView = VK_NULL_HANDLE;
		}
	}
	for (VkImageView& faceView : pointShadowColorFaceViews) {
		if (faceView != VK_NULL_HANDLE) {
			vkDestroyImageView(device, faceView, nullptr);
			faceView = VK_NULL_HANDLE;
		}
	}

	DestroyVulkanImage(directionalShadowDepth);
	DestroyVulkanImage(directionalShadowColor);
	DestroyVulkanImage(spotShadowDepth);
	DestroyVulkanImage(spotShadowColor);
	DestroyVulkanImage(areaShadowDepth);
	DestroyVulkanImage(areaShadowColor);
	DestroyVulkanImage(pointShadowCubeDepth);
	DestroyVulkanImage(pointShadowCubeColor);
	DestroyVulkanImage(ltcAmplitudeTexture);
	DestroyVulkanImage(ltcInverseTexture);

	textureIndexMap.clear();
	freeTextureIndices.clear();
	nextTextureIndex = 2; // Index 0 is Default White, Index 1 is Default Normal

    // Reset shadow texture indices so they get reallocated on reopen
    directionalShadowTextureIndex = 0;
    spotShadowTextureIndex = 0;
    areaShadowTextureIndex = 0;
    ltcAmplitudeTextureIndex = 0;
    ltcInverseTextureIndex = 0;

	// Reset default texture descriptors to prevent stale views from being sampled if they were overwritten
	if (defaultTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(0, defaultTexture);
	}
	if (defaultNormalTexture.view != VK_NULL_HANDLE) {
		UpdateGlobalTexture(1, defaultNormalTexture);
	}

	materialIndexMap.clear();
	freeMaterialIndices.clear();
	nextMaterialIndex = 0;
}

