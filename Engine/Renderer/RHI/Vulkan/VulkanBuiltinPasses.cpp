// VulkanBuiltinPasses.cpp — Built-in grid, sky, and UI rendering passes
//
// This file owns one focused VulkanContext responsibility. State remains in VulkanContext::Impl,
// so the split changes compilation boundaries without changing resource lifetime or synchronization.

#include "VulkanContextSource.h"

// Create Grid Pipeline for Scene View

void VulkanContext::Impl::CreateSceneViewGridPipeline(VkRenderPass renderPass) {
	if (sceneViewGridPipeline != VK_NULL_HANDLE) return;

	// Load SPIR-V
	auto loadShader = [&](const std::string& path) -> VkShaderModule {
		std::filesystem::path shaderPath(path);
		if (shaderPath.is_relative())
		{
			// Editor debug shaders are installed beside the executable, not the
			// current working directory. Resolve through ResourceManager so
			// --project launches from arbitrary folders still find them.
			shaderPath = ResourceManager::Instance().GetEngineRoot() / shaderPath;
		}

		std::ifstream file(shaderPath, std::ios::ate | std::ios::binary);
		if (!file.is_open()) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to open shader file: " << shaderPath);
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
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << shaderPath);
			return VK_NULL_HANDLE;
		}
		return shaderModule;
	};

	// These editor/debug shaders stay as raw SPIR-V payloads.
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
	// Grid shaders intentionally write target0 only. Device selection requires
	// independentBlend for this two-attachment pass, so its identity slot may
	// safely disable both blending and writes without affecting target0.
	VkPipelineColorBlendAttachmentState identityBlendAttachment{};
	identityBlendAttachment.colorWriteMask = 0u;
	identityBlendAttachment.blendEnable = VK_FALSE;
	const std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments = {
		colorBlendAttachment, identityBlendAttachment };

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = UsesConventionalVisibilityAttachments(renderPass) ? 2u : 1u;
	colorBlending.pAttachments = UsesConventionalVisibilityAttachments(renderPass)
		? blendAttachments.data() : &colorBlendAttachment;

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
		vkDestroyShaderModule(device, vertShader, nullptr);
		vkDestroyShaderModule(device, fragShader, nullptr);
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


void VulkanContext::Impl::RenderSceneViewGrid(VkCommandBuffer cmd,
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	VkExtent2D renderExtent) {
	if (sceneViewGridPipeline == VK_NULL_HANDLE) return;

	// Scene geometry is rendered in a camera-relative float frame, while the
	// construction grid is anchored to canonical world Y=0. Express canonical
	// origin in this view's immutable reference frame, then make the shader's
	// local y=0 plane represent that canonical plane. Without this translation,
	// perspective rays intersected a plane through the camera and were discarded.
	const TerraEngine::Renderer::RenderLocalizationResult gridOrigin =
		TerraEngine::Renderer::LocalizeRenderPosition(
			TerraEngine::Scene::WorldPosition{}, presentation.referenceFrame);
	if (!gridOrigin.Succeeded()) return;

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sceneViewGridPipeline);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(renderExtent.width);
	viewport.height = static_cast<float>(renderExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = renderExtent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	GridPushConstants push{};
	const glm::mat4 gridCoordinatesToViewFrame = glm::translate(
		glm::mat4(1.0F), gridOrigin.offsetMetres);
	push.view = presentation.viewMatrix * gridCoordinatesToViewFrame;
	push.proj = presentation.projectionMatrix;
	push.inverseView = glm::inverse(push.view);
	push.inverseProj = glm::inverse(presentation.projectionMatrix);

	vkCmdPushConstants(cmd, sceneViewGridPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPushConstants), &push);

	vkCmdDraw(cmd, 6, 1, 0, 0); // 6 vertices for fullscreen quad
}


struct SkyPushConstants
{
	glm::vec4 cameraOrientation = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	glm::vec4 horizonTintIntensity = glm::vec4(1.0f);
	glm::vec4 zenithTintExposure = glm::vec4(1.0f);
	glm::vec4 groundTintPadding = glm::vec4(1.0f);
	glm::vec4 sunDirectionTextureIndex = glm::vec4(0.0f, 0.35f, -1.0f, 0.0f);
	glm::vec4 skyParams = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);
	glm::vec4 editorTint = glm::vec4(0.0f);
};


void VulkanContext::Impl::CreateSkyBackgroundPipeline(VkRenderPass renderPass)
{
	if (skyBackgroundPipeline != VK_NULL_HANDLE)
	{
		return;
	}


	struct LoadedSkyShader
	{
		VkShaderModule module = VK_NULL_HANDLE;
		const char* entryPoint = nullptr;
	};

	auto loadShader = [&](const std::string& relativePath) -> LoadedSkyShader {
		LoadedSkyShader loadedShader{};
		const std::filesystem::path resolvedPath = ResourceManager::Instance().ResolveEngineAssetPath(relativePath);

		// Sky shaders now use the same packaged TSPF path as the default mesh
		// pipeline so task/mesh stages inherit the Vulkan 1.3 target flags and
		// task-payload patching performed by TerraEngineShaderCompiler.
		const TerraEngine::UUID shaderId = ResourceManager::Instance().LoadShader(resolvedPath);
		Shader* shader = ResourceManager::Instance().GetShader(shaderId);
		if (shader == nullptr || shader->bytecode.empty())
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to load packaged sky shader: " << resolvedPath);
			return loadedShader;
		}

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = shader->bytecode.size();
		createInfo.pCode = reinterpret_cast<const std::uint32_t*>(shader->bytecode.data());


		if (vkCreateShaderModule(device, &createInfo, nullptr, &loadedShader.module) != VK_SUCCESS)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << resolvedPath);
			loadedShader.module = VK_NULL_HANDLE;
			return loadedShader;
		}

		loadedShader.entryPoint = shader->stringTable.data() + shader->binary.entryPointOffset;
		return loadedShader;
	};


	if (!taskShaderSupported)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot create sky background pipeline without task shader support.");
		return;
	}

	const LoadedSkyShader taskShader = loadShader("Shaders/Sky/SkyBackground.task.tspf");
	const LoadedSkyShader meshShader = loadShader("Shaders/Sky/SkyBackground.mesh.tspf");
	const LoadedSkyShader fragShader = loadShader("Shaders/Sky/SkyBackground.frag.tspf");

	if (taskShader.module == VK_NULL_HANDLE || meshShader.module == VK_NULL_HANDLE || fragShader.module == VK_NULL_HANDLE)
	{
		if (taskShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, taskShader.module, nullptr);
		}

		if (meshShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, meshShader.module, nullptr);
		}

		if (fragShader.module != VK_NULL_HANDLE)
		{
			vkDestroyShaderModule(device, fragShader.module, nullptr);
		}
		return;
	}

	VkPipelineShaderStageCreateInfo shaderStages[3]{};
	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_TASK_BIT_EXT;
	shaderStages[0].module = taskShader.module;
	shaderStages[0].pName = taskShader.entryPoint;
	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
	shaderStages[1].module = meshShader.module;
	shaderStages[1].pName = meshShader.entryPoint;
	shaderStages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[2].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[2].module = fragShader.module;
	shaderStages[2].pName = fragShader.entryPoint;

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
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_FALSE;
	// The sky background remains target0-only even when the scene pass includes
	// an identity attachment. Required independentBlend lets this second slot
	// disable both blending and writes while preserving the render-pass ABI.
	VkPipelineColorBlendAttachmentState identityBlendAttachment{};
	identityBlendAttachment.colorWriteMask = 0u;
	identityBlendAttachment.blendEnable = VK_FALSE;
	const std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments = {
		colorBlendAttachment, identityBlendAttachment };

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = UsesConventionalVisibilityAttachments(renderPass) ? 2u : 1u;
	colorBlending.pAttachments = UsesConventionalVisibilityAttachments(renderPass)
		? blendAttachments.data() : &colorBlendAttachment;

	std::array<VkDynamicState, 2> dynamicStates = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(SkyPushConstants);

	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pSetLayouts = &globalDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;


	if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &skyBackgroundPipelineLayout) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create sky background pipeline layout!");
		vkDestroyShaderModule(device, taskShader.module, nullptr);
		vkDestroyShaderModule(device, meshShader.module, nullptr);
		vkDestroyShaderModule(device, fragShader.module, nullptr);
		return;
	}

	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount = 3;
	pipelineInfo.pStages = shaderStages;
	pipelineInfo.pVertexInputState = nullptr;
	pipelineInfo.pInputAssemblyState = nullptr;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterizer;
	pipelineInfo.pMultisampleState = &multisampling;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &colorBlending;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = skyBackgroundPipelineLayout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0;


	if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skyBackgroundPipeline) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Error, "failed to create sky background pipeline!");
	}

	vkDestroyShaderModule(device, taskShader.module, nullptr);
	vkDestroyShaderModule(device, meshShader.module, nullptr);
	vkDestroyShaderModule(device, fragShader.module, nullptr);
}


void VulkanContext::Impl::RenderSkyBackground(VkCommandBuffer cmd,
	const TerraEngine::Renderer::RenderViewPresentationInput& presentation,
	VkExtent2D renderExtent)
{
	if (!presentation.sky.renderBackground || skyBackgroundPipeline == VK_NULL_HANDLE || skyBackgroundPipelineLayout == VK_NULL_HANDLE)
	{
		return;
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBackgroundPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBackgroundPipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

	VkViewport viewport{};
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = static_cast<float>(renderExtent.width);
	viewport.height = static_cast<float>(renderExtent.height);
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &viewport);

	VkRect2D scissor{};
	scissor.offset = {0, 0};
	scissor.extent = renderExtent;
	vkCmdSetScissor(cmd, 0, 1, &scissor);

	const glm::mat4 inverseViewMatrix = glm::inverse(presentation.viewMatrix);
	const glm::quat cameraOrientation = glm::normalize(glm::quat_cast(glm::mat3(inverseViewMatrix)));
	const bool orthographicProjection = std::abs(presentation.projectionMatrix[3][3]) > 0.5f;
	// Orthographic projection scale is a zoom value, not a perspective FOV.
	// Keep the procedural sky from stretching as the Scene View ortho size changes.
	const float tanHalfFovX = orthographicProjection ? 1.0f : 1.0f / std::max(std::abs(presentation.projectionMatrix[0][0]), 0.0001f);
	const float tanHalfFovY = orthographicProjection ? 1.0f : 1.0f / std::max(std::abs(presentation.projectionMatrix[1][1]), 0.0001f);

	SkyPushConstants push{};
	push.cameraOrientation = glm::vec4(cameraOrientation.x, cameraOrientation.y, cameraOrientation.z, cameraOrientation.w);
	push.horizonTintIntensity = glm::vec4(presentation.sky.horizonTint, presentation.sky.intensity);
	push.zenithTintExposure = glm::vec4(presentation.sky.zenithTint, presentation.sky.exposure);
	push.groundTintPadding = glm::vec4(presentation.sky.groundTint, 0.0f);
	push.sunDirectionTextureIndex = glm::vec4(presentation.sky.sunDirection, 0.0f);
	std::uint32_t skyFlags = 0u;

	if (presentation.sky.hasTexture)
	{
		skyFlags |= 1u;
	}

	if (presentation.sky.forceBlack)
	{
		skyFlags |= 2u;
	}
	if (presentation.sceneViewOrthographic)
	{
		// Scene View orthographic mode still uses the sky pipeline, but the
		// shader switches to a screen-space editor gradient instead of world sky.
		skyFlags |= 4u;
	}
	push.skyParams = glm::vec4(tanHalfFovX, tanHalfFovY, glm::radians(presentation.sky.rotationOffsetDegrees), static_cast<float>(skyFlags));
	push.editorTint = presentation.editorContextTint;


	if (presentation.sky.hasTexture)
	{
		EnsureTexture(presentation.sky.textureId);

		if (const auto textureIndexIt = textureIndexMap.find(presentation.sky.textureId); textureIndexIt != textureIndexMap.end())
		{
			push.sunDirectionTextureIndex.w = static_cast<float>(textureIndexIt->second);
		}
	}

	vkCmdPushConstants(cmd, skyBackgroundPipelineLayout, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SkyPushConstants), &push);
	vkCmdDrawMeshTasksEXT(cmd, 1u, 1u, 1u);
}

// UI Rendering Implementation

void VulkanContext::Impl::CreateUIPipelines(VkRenderPass renderPass) {
	auto loadShader = [&](const std::string& path) -> VkShaderModule {
		const std::filesystem::path shaderPath = ResourceManager::Instance().ResolveEngineAssetPath(path);
		const std::vector<std::uint8_t> shaderBytes = ResourceManager::Instance().ReadFileBytes(shaderPath);
		// SPIR-V shader modules must be non-empty and aligned to 32-bit words.
		if (shaderBytes.empty() || (shaderBytes.size() % sizeof(std::uint32_t)) != 0) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to load valid UI shader bytes: " << shaderPath);
			return VK_NULL_HANDLE;
		}

		std::vector<std::uint32_t> buffer(shaderBytes.size() / sizeof(std::uint32_t));
		std::memcpy(buffer.data(), shaderBytes.data(), shaderBytes.size());

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = buffer.size() * sizeof(std::uint32_t);
		createInfo.pCode = buffer.data();

		VkShaderModule shaderModule = VK_NULL_HANDLE;

		if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create shader module for: " << shaderPath);
			return VK_NULL_HANDLE;
		}

		return shaderModule;
	};

	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = sizeof(float) * 8;
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	VkVertexInputAttributeDescription attributeDescriptions[3]{};
	attributeDescriptions[0].binding = 0;
	attributeDescriptions[0].location = 0;
	attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[0].offset = 0;
	attributeDescriptions[1].binding = 0;
	attributeDescriptions[1].location = 1;
	attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributeDescriptions[1].offset = sizeof(float) * 2;
	attributeDescriptions[2].binding = 0;
	attributeDescriptions[2].location = 2;
	attributeDescriptions[2].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributeDescriptions[2].offset = sizeof(float) * 4;

	VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
	vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertexInputInfo.vertexBindingDescriptionCount = 1;
	vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
	vertexInputInfo.vertexAttributeDescriptionCount = 3;
	vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

	VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
	inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	inputAssembly.primitiveRestartEnable = VK_FALSE;

	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1;
	viewportState.scissorCount = 1;
	// One UI pipeline serves the runtime swapchain and differently sized editor
	// Game targets. Record their viewport/scissor in each command buffer instead
	// of baking the window size into the pipeline and moving visible hit targets.
	const VkDynamicState uiDynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount = 2;
	dynamicState.pDynamicStates = uiDynamicStates;

	VkPipelineRasterizationStateCreateInfo rasterizer{};
	rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterizer.depthClampEnable = VK_FALSE;
	rasterizer.rasterizerDiscardEnable = VK_FALSE;
	rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
	rasterizer.lineWidth = 1.0f;
	rasterizer.cullMode = VK_CULL_MODE_NONE;
	rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterizer.depthBiasEnable = VK_FALSE;

	VkPipelineMultisampleStateCreateInfo multisampling{};
	multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisampling.sampleShadingEnable = VK_FALSE;
	multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_FALSE;
	depthStencil.depthWriteEnable = VK_FALSE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.stencilTestEnable = VK_FALSE;

	VkPipelineColorBlendAttachmentState colorBlendAttachment{};
	colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	colorBlendAttachment.blendEnable = VK_TRUE;
	colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
	colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
	colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
	// UI is target0-only content. Required independentBlend lets the typed
	// identity attachment disable both blending and writes independently.
	VkPipelineColorBlendAttachmentState identityBlendAttachment{};
	identityBlendAttachment.colorWriteMask = 0u;
	identityBlendAttachment.blendEnable = VK_FALSE;
	const std::array<VkPipelineColorBlendAttachmentState, 2> blendAttachments = {
		colorBlendAttachment, identityBlendAttachment };

	VkPipelineColorBlendStateCreateInfo colorBlending{};
	colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	colorBlending.logicOpEnable = VK_FALSE;
	colorBlending.attachmentCount = UsesConventionalVisibilityAttachments(renderPass) ? 2u : 1u;
	colorBlending.pAttachments = UsesConventionalVisibilityAttachments(renderPass)
		? blendAttachments.data() : &colorBlendAttachment;

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(glm::vec2) + (sizeof(uint32_t) * 2);


	if (staticUiPipelineLayout == VK_NULL_HANDLE) {
		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 1;
		pipelineLayoutInfo.pSetLayouts = &globalDescriptorSetLayout;
		pipelineLayoutInfo.pushConstantRangeCount = 1;
		pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;


		if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &staticUiPipelineLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create static UI pipeline layout!");
		}
	}


	if (staticUiPipeline == VK_NULL_HANDLE && staticUiPipelineLayout != VK_NULL_HANDLE) {
		VkShaderModule staticVert = loadShader("Shaders/UI/UI_Static.vert.spv");
		VkShaderModule staticFrag = loadShader("Shaders/UI/UI_Static.frag.spv");

		if (!staticVert || !staticFrag) {
			TERRAENGINE_LOG(LogLevel::Warning, "UI Static shaders not found. UI rendering may be disabled.");
			if (staticVert) vkDestroyShaderModule(device, staticVert, nullptr);

			if (staticFrag) vkDestroyShaderModule(device, staticFrag, nullptr);
		} else {
			VkPipelineShaderStageCreateInfo shaderStages[2]{};
			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].module = staticVert;
			shaderStages[0].pName = "VSMain";
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].module = staticFrag;
			shaderStages[1].pName = "PSMain";

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
			pipelineInfo.layout = staticUiPipelineLayout;
			pipelineInfo.pDynamicState = &dynamicState;
			pipelineInfo.renderPass = renderPass;
			pipelineInfo.subpass = 0;


			if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &staticUiPipeline) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create static UI pipeline!");
			}

			vkDestroyShaderModule(device, staticVert, nullptr);
			vkDestroyShaderModule(device, staticFrag, nullptr);
		}
	}


	if (uiFontDescriptorSetLayout == VK_NULL_HANDLE) {
		VkDescriptorSetLayoutBinding textBindings[2]{};
		textBindings[0].binding = 0;
		textBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		textBindings[0].descriptorCount = 1;
		textBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		textBindings[1].binding = 1;
		textBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		textBindings[1].descriptorCount = 1;
		textBindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
		setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutInfo.bindingCount = 2;
		setLayoutInfo.pBindings = textBindings;


		if (vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &uiFontDescriptorSetLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create UI font descriptor set layout!");
		}
	}


	if (uiFontDescriptorSet == VK_NULL_HANDLE && uiFontDescriptorSetLayout != VK_NULL_HANDLE) {
		VkDescriptorSetAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &uiFontDescriptorSetLayout;


		if (vkAllocateDescriptorSets(device, &allocInfo, &uiFontDescriptorSet) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate UI font descriptor set!");
			uiFontDescriptorSet = VK_NULL_HANDLE;
		}
	}


	if (sdfTextPipelineLayout == VK_NULL_HANDLE && uiFontDescriptorSetLayout != VK_NULL_HANDLE) {
		VkPushConstantRange textPushConstantRange{};
		textPushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		textPushConstantRange.offset = 0;
		textPushConstantRange.size = sizeof(glm::vec2);

		VkPipelineLayoutCreateInfo textLayoutInfo{};
		textLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		textLayoutInfo.setLayoutCount = 1;
		textLayoutInfo.pSetLayouts = &uiFontDescriptorSetLayout;
		textLayoutInfo.pushConstantRangeCount = 1;
		textLayoutInfo.pPushConstantRanges = &textPushConstantRange;


		if (vkCreatePipelineLayout(device, &textLayoutInfo, nullptr, &sdfTextPipelineLayout) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create SDF text pipeline layout!");
		}
	}


	if (sdfTextPipeline == VK_NULL_HANDLE && sdfTextPipelineLayout != VK_NULL_HANDLE) {
		VkShaderModule textVert = loadShader("Shaders/UI/UI_SDF_Text.vert.spv");
		VkShaderModule textFrag = loadShader("Shaders/UI/UI_SDF_Text.frag.spv");

		if (!textVert || !textFrag) {
			TERRAENGINE_LOG(LogLevel::Warning, "UI SDF text shaders not found. Runtime UI text rendering may be disabled.");
			if (textVert) vkDestroyShaderModule(device, textVert, nullptr);

			if (textFrag) vkDestroyShaderModule(device, textFrag, nullptr);
		} else {
			VkPipelineShaderStageCreateInfo shaderStages[2]{};
			shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			shaderStages[0].module = textVert;
			shaderStages[0].pName = "VSMain";
			shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			shaderStages[1].module = textFrag;
			shaderStages[1].pName = "PSMain";

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
			pipelineInfo.layout = sdfTextPipelineLayout;
			pipelineInfo.pDynamicState = &dynamicState;
			pipelineInfo.renderPass = renderPass;
			pipelineInfo.subpass = 0;


			if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &sdfTextPipeline) != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Error, "failed to create SDF text pipeline!");
			}

			vkDestroyShaderModule(device, textVert, nullptr);
			vkDestroyShaderModule(device, textFrag, nullptr);
		}
	}
}


struct UIStaticPushConstants
{
	glm::vec2 viewportSize;
	uint32_t textureIndex;
	uint32_t useTexture;
};


void VulkanContext::Impl::RenderUI(VkCommandBuffer cmd, const UIRenderBatch& batch) {
	if (batch.staticQuads.empty() && batch.immediateQuads.empty() && batch.textRuns.empty() && batch.imageRuns.empty()) return;
	if (uiStaticVertexBuffers.size() < MAX_FRAMES_IN_FLIGHT) {
		uiStaticVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	}
	if (uiSdfVertexBuffers.size() < MAX_FRAMES_IN_FLIGHT) {
		uiSdfVertexBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	}
	// BeginFrame has waited this exact slot's fence. Writing only the slot-owned
	// buffers prevents CPU UI generation from racing a different in-flight slot.
	VulkanBuffer& uiStaticVertexBuffer = uiStaticVertexBuffers[currentFrame];
	VulkanBuffer& uiSdfVertexBuffer = uiSdfVertexBuffers[currentFrame];

	auto appendQuadVertices = [](std::vector<float>& vertices, const glm::vec2& position, const glm::vec2& size, const glm::vec4& color, const glm::vec2& uvMin, const glm::vec2& uvMax) {
		const float x = position.x;
		const float y = position.y;
		const float w = size.x;
		const float h = size.y;

		auto addVertex = [&](float px, float py, float u, float v) {
			vertices.push_back(px); vertices.push_back(py);
			vertices.push_back(u); vertices.push_back(v);
			vertices.push_back(color.r); vertices.push_back(color.g);
			vertices.push_back(color.b); vertices.push_back(color.a);
		};

		addVertex(x, y, uvMin.x, uvMin.y);
		addVertex(x + w, y, uvMax.x, uvMin.y);
		addVertex(x, y + h, uvMin.x, uvMax.y);
		addVertex(x, y + h, uvMin.x, uvMax.y);
		addVertex(x + w, y, uvMax.x, uvMin.y);
		addVertex(x + w, y + h, uvMax.x, uvMax.y);
	};


	if (staticUiPipeline != VK_NULL_HANDLE) {
		std::vector<float> quadVertices;
		struct ImageDraw {
			uint32_t firstVertex;
			std::string texturePath;
			glm::vec4 color;
		};
		std::vector<ImageDraw> imageDraws;

		quadVertices.reserve((batch.staticQuads.size() + batch.immediateQuads.size() + batch.imageRuns.size()) * 6 * 8);


		for (const auto& quad : batch.staticQuads) {
			appendQuadVertices(quadVertices, quad.position, quad.size, quad.color, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
		}


		for (const auto& quad : batch.immediateQuads) {
			appendQuadVertices(
				quadVertices,
				quad.position,
				quad.size,
				quad.color,
				glm::vec2(quad.uvRect.x, quad.uvRect.y),
				glm::vec2(quad.uvRect.z, quad.uvRect.w));
		}

		const uint32_t staticVertexCount = static_cast<uint32_t>(quadVertices.size() / 8);


		for (const auto& image : batch.imageRuns) {
			ImageDraw draw;
			draw.firstVertex = static_cast<uint32_t>(quadVertices.size() / 8);
			draw.texturePath = image.texturePath;
			draw.color = image.color;
			imageDraws.push_back(draw);

			appendQuadVertices(quadVertices, image.position, image.size, image.color, glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 1.0f));
		}


		if (!quadVertices.empty()) {
			const VkDeviceSize bufferSize = quadVertices.size() * sizeof(float);
			if (uiStaticVertexBuffer.buffer == VK_NULL_HANDLE || uiStaticVertexBuffer.size < bufferSize) {

				if (uiStaticVertexBuffer.buffer != VK_NULL_HANDLE) {
					RetireBuffer(uiStaticVertexBuffer);
				}
				uiStaticVertexBuffer = CreateBuffer(bufferSize * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "UIStaticVertexBuffer");
			}

			void* data = nullptr;
			vmaMapMemory(allocator, uiStaticVertexBuffer.allocation, &data);
			memcpy(data, quadVertices.data(), bufferSize);
			// CPU_TO_GPU allocations are not guaranteed to be host-coherent on
			// every driver. Flush the exact UI vertex range before Vulkan reads it.
			vmaFlushAllocation(allocator, uiStaticVertexBuffer.allocation, 0, bufferSize);
			vmaUnmapMemory(allocator, uiStaticVertexBuffer.allocation);

			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, staticUiPipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, staticUiPipelineLayout, 0, 1, &globalDescriptorSets[currentFrame], 0, nullptr);

			VkBuffer vertexBuffers[] = { uiStaticVertexBuffer.buffer };
			VkDeviceSize offsets[] = { 0 };
			vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

			// Draw non-textured quads

			if (staticVertexCount > 0) {
				UIStaticPushConstants push{};
				push.viewportSize = batch.viewportSize;
				push.textureIndex = 0;
				push.useTexture = 0;
				vkCmdPushConstants(cmd, staticUiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UIStaticPushConstants), &push);
				vkCmdDraw(cmd, staticVertexCount, 1, 0, 0);
			}

			// Draw each textured image run

			for (const auto& draw : imageDraws) {
				UIStaticPushConstants push{};
				push.viewportSize = batch.viewportSize;
				push.useTexture = 0;
				push.textureIndex = 0;

				TerraEngine::UUID textureId = ResourceManager::Instance().ResolveAssetReference(draw.texturePath, AssetType::Texture);

				if (textureId != TerraEngine::EMPTY_UUID) {
					EnsureTexture(textureId);
					if (textureIndexMap.contains(textureId)) {
						push.textureIndex = textureIndexMap[textureId];
						push.useTexture = 1;
					}
				}				vkCmdPushConstants(cmd, staticUiPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(UIStaticPushConstants), &push);
				vkCmdDraw(cmd, 6, 1, draw.firstVertex, 0);
			}
		}
	}


	if (batch.textRuns.empty() || sdfTextPipeline == VK_NULL_HANDLE || uiFontDescriptorSet == VK_NULL_HANDLE) {
		return;
	}

	const std::filesystem::path fontLogicalPath = "Fonts/Roboto-Medium.ttf";
	const std::filesystem::path fontPath = ResourceManager::Instance().ResolveEngineAssetPath(fontLogicalPath);
	if (!uiFontAtlasSourceLoadAttempted || uiFontAtlasSourcePath != fontPath) {
		const std::vector<std::uint8_t> fontBytes = ResourceManager::Instance().ReadFileBytes(fontPath);
		uiFontAtlasSourceBytes.assign(fontBytes.begin(), fontBytes.end());
		uiFontAtlasSourcePath = fontPath;
		uiFontAtlasSourceLoadAttempted = true;
		uiFontAtlasMissingWarned = false;
	}
	if (uiFontAtlasSourceBytes.empty()) {
		if (!uiFontAtlasMissingWarned) {
			TERRAENGINE_LOG(LogLevel::Warning, "Runtime UI text font is unavailable. Missing or empty font: " << fontPath);
			uiFontAtlasMissingWarned = true;
		}
		return;
	}
	const UIFontAtlas& fontAtlas = uiTextRenderer.EnsureSignedDistanceFontAtlasFromBytes(fontLogicalPath.generic_string(), uiFontAtlasSourceBytes, uiFontAtlasPixelHeight);

	if (!fontAtlas.ready || fontAtlas.width <= 0 || fontAtlas.height <= 0 || fontAtlas.pixels.empty()) {
		if (!uiFontAtlasMissingWarned) {
			TERRAENGINE_LOG(LogLevel::Warning, "Runtime UI text atlas is unavailable. Missing or invalid font: " << fontPath);
			uiFontAtlasMissingWarned = true;
		}
		return;
	}

	auto destroyUIFontAtlasImage = [&]() {

		if (uiFontAtlasImage.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, uiFontAtlasImage.sampler, nullptr);
			uiFontAtlasImage.sampler = VK_NULL_HANDLE;
		}

		if (uiFontAtlasImage.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, uiFontAtlasImage.view, nullptr);
			uiFontAtlasImage.view = VK_NULL_HANDLE;
		}

		if (uiFontAtlasImage.image != VK_NULL_HANDLE && uiFontAtlasImage.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, uiFontAtlasImage.image, uiFontAtlasImage.allocation);
			uiFontAtlasImage.image = VK_NULL_HANDLE;
			uiFontAtlasImage.allocation = VK_NULL_HANDLE;
		}
		uiFontAtlasWidth = 0;
		uiFontAtlasHeight = 0;
	};


	if (uiFontAtlasImage.image == VK_NULL_HANDLE || uiFontAtlasWidth != fontAtlas.width || uiFontAtlasHeight != fontAtlas.height) {
		destroyUIFontAtlasImage();

		const VkDeviceSize imageSize = static_cast<VkDeviceSize>(fontAtlas.pixels.size());
		VulkanBuffer stagingBuffer = CreateBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "UIFontAtlasStagingBuffer");
		void* stagingData = nullptr;
		vmaMapMemory(allocator, stagingBuffer.allocation, &stagingData);
		memcpy(stagingData, fontAtlas.pixels.data(), static_cast<std::size_t>(imageSize));
		// Font atlas staging can also use non-coherent host-visible memory in
		// optimized builds, so flush before copying into the GPU-only image.
		vmaFlushAllocation(allocator, stagingBuffer.allocation, 0, imageSize);
		vmaUnmapMemory(allocator, stagingBuffer.allocation);

		VkImageCreateInfo imageInfo{};
		imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent = { static_cast<uint32_t>(fontAtlas.width), static_cast<uint32_t>(fontAtlas.height), 1 };
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 1;
		imageInfo.format = VK_FORMAT_R8_UNORM;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

		if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &uiFontAtlasImage.image, &uiFontAtlasImage.allocation, nullptr) != VK_SUCCESS) {
			vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);
			TERRAENGINE_LOG(LogLevel::Error, "Failed to allocate runtime UI font atlas image.");
			return;
		}

		TransitionImageLayout(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		CopyBufferToImage(stagingBuffer.buffer, uiFontAtlasImage.image, static_cast<uint32_t>(fontAtlas.width), static_cast<uint32_t>(fontAtlas.height));
		TransitionImageLayout(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vmaDestroyBuffer(allocator, stagingBuffer.buffer, stagingBuffer.allocation);

		uiFontAtlasImage.view = CreateImageView(uiFontAtlasImage.image, imageInfo.format, VK_IMAGE_ASPECT_COLOR_BIT);

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

		if (vkCreateSampler(device, &samplerInfo, nullptr, &uiFontAtlasImage.sampler) != VK_SUCCESS) {
			destroyUIFontAtlasImage();
			TERRAENGINE_LOG(LogLevel::Error, "Failed to create runtime UI font atlas sampler.");
			return;
		}

		uiFontAtlasWidth = fontAtlas.width;
		uiFontAtlasHeight = fontAtlas.height;

		VkDescriptorImageInfo sampledImageInfo{};
		sampledImageInfo.imageView = uiFontAtlasImage.view;
		sampledImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkDescriptorImageInfo samplerDescriptorInfo{};
		samplerDescriptorInfo.sampler = uiFontAtlasImage.sampler;

		VkWriteDescriptorSet writes[2]{};
		writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet = uiFontDescriptorSet;
		writes[0].dstBinding = 0;
		writes[0].descriptorCount = 1;
		writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
		writes[0].pImageInfo = &sampledImageInfo;
		writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet = uiFontDescriptorSet;
		writes[1].dstBinding = 1;
		writes[1].descriptorCount = 1;
		writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		writes[1].pImageInfo = &samplerDescriptorInfo;
		vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	}

	std::vector<float> textVertices;
	textVertices.reserve(batch.textRuns.size() * 96 * 8);


	for (const UITextRun& textRun : batch.textRuns) {
		if (textRun.text.empty() || textRun.fontSize <= 0.0f) {
			continue;
		}

		const float glyphScale = textRun.fontSize / std::max(fontAtlas.pixelHeight, 1.0f);
		float penX = textRun.position.x;
		float baselineY = textRun.position.y + textRun.fontSize;
		const float lineAdvance = textRun.fontSize * 1.2f;


		for (char character : textRun.text) {
			if (character == '\n') {
				penX = textRun.position.x;
				baselineY += lineAdvance;
				continue;
			}

			if (character == '\r') {
				continue;
			}

			const auto glyphIt = fontAtlas.glyphs.find(character);

			if (glyphIt == fontAtlas.glyphs.end()) {
				penX += textRun.fontSize * 0.35f;
				continue;
			}

			const UIFontGlyph& glyph = glyphIt->second;
			appendQuadVertices(
				textVertices,
				glm::vec2(penX + (glyph.bearing.x * glyphScale), baselineY + (glyph.bearing.y * glyphScale)),
				glyph.size * glyphScale,
				textRun.color,
				glyph.uvMin,
				glyph.uvMax);
			penX += glyph.advance * glyphScale;
		}
	}


	if (textVertices.empty()) {
		return;
	}

	const VkDeviceSize textBufferSize = textVertices.size() * sizeof(float);

	if (uiSdfVertexBuffer.buffer == VK_NULL_HANDLE || uiSdfVertexBuffer.size < textBufferSize) {

		if (uiSdfVertexBuffer.buffer != VK_NULL_HANDLE) {
			RetireBuffer(uiSdfVertexBuffer);
		}
		uiSdfVertexBuffer = CreateBuffer(textBufferSize * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, "UISdfVertexBuffer");
	}

	void* textData = nullptr;
	vmaMapMemory(allocator, uiSdfVertexBuffer.allocation, &textData);
	memcpy(textData, textVertices.data(), textBufferSize);
	// SDF text vertices are written by the CPU and consumed by the GPU in the
	// same submitted command buffer, so make the mapped writes explicitly visible.
	vmaFlushAllocation(allocator, uiSdfVertexBuffer.allocation, 0, textBufferSize);
	vmaUnmapMemory(allocator, uiSdfVertexBuffer.allocation);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfTextPipeline);

	glm::vec2 viewportSize = batch.viewportSize;
	vkCmdPushConstants(cmd, sdfTextPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::vec2), &viewportSize);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sdfTextPipelineLayout, 0, 1, &uiFontDescriptorSet, 0, nullptr);

	VkBuffer textVertexBuffers[] = { uiSdfVertexBuffer.buffer };
	VkDeviceSize textOffsets[] = { 0 };
	vkCmdBindVertexBuffers(cmd, 0, 1, textVertexBuffers, textOffsets);
	vkCmdDraw(cmd, static_cast<uint32_t>(textVertices.size() / 8), 1, 0, 0);
}
