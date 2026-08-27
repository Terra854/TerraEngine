// VulkanContextInit.cpp — Vulkan Backend Initialization Implementation
//
// Implements the startup and teardown logic for the Vulkan rendering backend.
// It handles instance creation, device selection, swapchain setup, and the configuration of standard render passes and framebuffers.
// This module ensures that the Vulkan environment is correctly initialized and safely destroyed during the engine lifecycle.

#include "VulkanContextImpl.h"
#include "VulkanFrameDiagnostics.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

bool VulkanContext::Impl::InitVulkan(WindowSystem& windowSystem) {
	// Spin up instance, device, swapchain, and default resources so rendering can begin.
	this->windowSystem = &windowSystem;
	deviceLost = false;
	deviceLostLogged = false;
	++renderSessionGeneration;
	// A newly initialized Vulkan device never shares presentation-compatible
	// objects with its predecessor, even if an embedding host reuses this context.
	++backendResourceGeneration;
	reusableSecondaryCommandSignatures.clear();
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestRendererRecoverySnapshot = TerraEngine::Renderer::MakeActiveRecoverySnapshot(renderSessionGeneration);
	}
	skipFrame = false;
	editorRenderSubmitted = false;
	volkInitialize();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateInstance()) {
		RecordInitializationFailure("VK_ERROR_INITIALIZATION_FAILED", "Vulkan instance creation");
		return false;
	}
	if (!SetupDebugMessenger()) {
		RecordInitializationFailure("VK_ERROR_INITIALIZATION_FAILED", "Vulkan debug messenger setup");
		return false;
	}
	
	auto* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create window surface!");
		RecordInitializationFailure("VK_ERROR_SURFACE_LOST_KHR", "Vulkan window surface creation");
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!PickPhysicalDevice()) {
		RecordInitializationFailure("VK_ERROR_INCOMPATIBLE_DRIVER", "Vulkan physical device selection");
		return false;
	}
	if (!CreateLogicalDevice()) {
		RecordInitializationFailure("VK_ERROR_INITIALIZATION_FAILED", "Vulkan logical device creation");
		return false;
	}

	VmaVulkanFunctions vulkanFunctions = {};
	vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	
	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	allocatorInfo.physicalDevice = physicalDevice;
	allocatorInfo.device = device;
	allocatorInfo.instance = instance;
	allocatorInfo.pVulkanFunctions = &vulkanFunctions;
	
	vmaCreateAllocator(&allocatorInfo, &allocator);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateSwapChain(windowSystem)) return false;
	if (!CreateCommandPool()) return false;
	if (!CreateImageViews()) return false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateDepthResources()) return false;

	VkFormat depthFmt = FindDepthFormat();
	for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
		for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
			for (VulkanHiZ& frameHiZ : frameHiZPair) {
				frameHiZ.Create(device, allocator, swapchainExtent.width, swapchainExtent.height, depthFmt);
			}
		}
	}
	for (std::array<uint32_t, MAX_FRAMES_IN_FLIGHT>& readIndices : hiZReadIndexByViewFrame) {
		readIndices.fill(0u);
	}
	for (std::array<bool, MAX_FRAMES_IN_FLIGHT>& pendingSwaps : hiZWritePendingSwapByViewFrame) {
		pendingSwaps.fill(false);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vrsFeatures.pipelineFragmentShadingRate) {
		vrs.Create(device, physicalDevice, allocator, swapchainExtent.width, swapchainExtent.height);
		TransitionImageLayout(vrs.image, VK_FORMAT_R8_UINT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateRenderPass()) return false;
	if (!CreateFramebuffers()) return false;
	if (!CreateSyncObjects()) return false;
	if (!presentationLifecycle.Configure(MAX_FRAMES_IN_FLIGHT, surfaceMinimumImageCount,
		requestedSwapchainImageCount, static_cast<std::uint32_t>(swapchainImages.size()))) return false;
	// Publish the initial native key so later VK_SUBOPTIMAL_KHR results stay
	// advisory until GLFW has actually reported a different framebuffer extent.
	presentationRecreation.SetActiveExtent({ swapchainExtent.width, swapchainExtent.height });
	editorCompletionTracker.SetPresentationGeneration(presentationLifecycle.Generation());
	if (!CreateGpuTimestampQueries()) return false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateDescriptorPool()) return false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateGlobalDescriptorResources()) return false;
	
	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		UpdateGlobalHiZ(frameIndex);
	}

	try {
		CreateDefaultTexture();
	} catch (const std::exception& e) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create default texture: " << e.what());
		Cleanup();
		return false;
	}
	CreateEnvironmentLightingResources();
	CreateMaterialBufferResources();
	CreateCullingBufferResources();
	CreateLightingBufferResources();
	CreateAmbientOcclusionResources();
	CreateIndirectDrawResources();

	workerCommandPools.resize(MAX_FRAMES_IN_FLIGHT);
	workerCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	
	initialized = true;
	return true;
}

bool VulkanContext::Impl::EnsureVulkanAnimationComputePipelines()
{
	// The immutable animation plan uses the normal bindless globals at set 0 and
	// frame-slot animation storage at set 1. Keeping both layouts in this exact
	// order matches the HLSL ABI without changing any graphics descriptor usage.
	const auto hasAllRequiredPipelines = [&]() {
		using Stage = TerraEngine::Animation::GpuAnimationStage;
		return animationComputePipelines.layout != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Sample)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Blend)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Hierarchy)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Procedural)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Ragdoll)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Morph)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Bounds)] != VK_NULL_HANDLE
			&& animationComputePipelines.byStage[static_cast<std::size_t>(Stage::Palette)] != VK_NULL_HANDLE;
	};
	if (hasAllRequiredPipelines()) return true;
	if (device == VK_NULL_HANDLE
		|| globalDescriptorSetLayout == VK_NULL_HANDLE
		|| animationDescriptorSetLayout == VK_NULL_HANDLE)
	{
		return false;
	}

	// Never leave a partial pipeline set usable: a missing optional shader must
	// select the renderer's bind-pose fallback rather than record some stages.
	DestroyVulkanAnimationComputePipelines();
	const std::array<VkDescriptorSetLayout, 2> setLayouts{
		globalDescriptorSetLayout,
		animationDescriptorSetLayout };
	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0U;
	pushRange.size = sizeof(TerraEngine::Animation::GpuAnimationPushConstants);
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
	layoutInfo.pSetLayouts = setLayouts.data();
	layoutInfo.pushConstantRangeCount = 1U;
	layoutInfo.pPushConstantRanges = &pushRange;
	if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &animationComputePipelines.layout) != VK_SUCCESS)
	{
		animationComputePipelines.layout = VK_NULL_HANDLE;
		return false;
	}

	const auto createPipeline = [&](TerraEngine::Animation::GpuAnimationStage stage, const char* shaderPath) {
		std::vector<std::uint8_t> shaderBytes;
		try {
			shaderBytes = ResourceManager::Instance().ReadFileBytes(
				ResourceManager::Instance().ResolveEngineAssetPath(shaderPath));
		} catch (const std::exception&) {
			return false;
		}
		if (shaderBytes.empty() || (shaderBytes.size() % sizeof(std::uint32_t)) != 0U) return false;

		VkShaderModuleCreateInfo moduleInfo{};
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = shaderBytes.size();
		moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes.data());
		VkShaderModule module = VK_NULL_HANDLE;
		if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;

		VkPipelineShaderStageCreateInfo stageInfo{};
		stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stageInfo.module = module;
		stageInfo.pName = "main";
		VkComputePipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipelineInfo.stage = stageInfo;
		pipelineInfo.layout = animationComputePipelines.layout;
		VkPipeline& pipeline = animationComputePipelines.byStage[static_cast<std::size_t>(stage)];
		const VkResult result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1U, &pipelineInfo, nullptr, &pipeline);
		vkDestroyShaderModule(device, module, nullptr);
		return result == VK_SUCCESS;
	};

	using Stage = TerraEngine::Animation::GpuAnimationStage;
	const bool created = createPipeline(Stage::Sample, "Shaders/Animation/AnimationSample.comp.spv")
		&& createPipeline(Stage::Blend, "Shaders/Animation/AnimationBlend.comp.spv")
		&& createPipeline(Stage::Hierarchy, "Shaders/Animation/AnimationHierarchy.comp.spv")
		&& createPipeline(Stage::Procedural, "Shaders/Animation/AnimationProcedural.comp.spv")
		&& createPipeline(Stage::Ragdoll, "Shaders/Animation/AnimationRagdoll.comp.spv")
		&& createPipeline(Stage::Morph, "Shaders/Animation/AnimationMorph.comp.spv")
		&& createPipeline(Stage::Bounds, "Shaders/Animation/AnimationBounds.comp.spv")
		&& createPipeline(Stage::Palette, "Shaders/Animation/AnimationPalette.comp.spv");
	if (!created)
	{
		TERRAENGINE_LOG(LogLevel::Warning,
			"GPU animation compute shaders are unavailable; animated draws will use their safe bind-pose fallback.");
		DestroyVulkanAnimationComputePipelines();
		return false;
	}
	return true;
}

void VulkanContext::Impl::DestroyVulkanAnimationComputePipelines()
{
	if (device != VK_NULL_HANDLE)
	{
		for (VkPipeline& pipeline : animationComputePipelines.byStage)
		{
			if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(device, pipeline, nullptr);
			pipeline = VK_NULL_HANDLE;
		}
		if (animationComputePipelines.layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(device, animationComputePipelines.layout, nullptr);
		}
	}
	animationComputePipelines = {};
}

	// All three compute stages share one explicit descriptor ABI. Generated
	// arguments are still not consumed by graphics, but their compute command is
	// independently measured and can fail open without affecting CPU draws.
bool VulkanContext::Impl::EnsureVulkanVirtualGeometryExecutorPipeline()
{
	if(virtualGeometryExecutorPipelines.layout != VK_NULL_HANDLE
		&& virtualGeometryExecutorPipelines.compaction != VK_NULL_HANDLE
		&& virtualGeometryExecutorPipelines.traversal != VK_NULL_HANDLE
		&& virtualGeometryExecutorPipelines.indirectGeneration != VK_NULL_HANDLE
		&& virtualGeometryExecutorDescriptorSetLayout != VK_NULL_HANDLE) {
		virtualGeometryExecutorUnavailableReason.clear();
		return true;
	}
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE) {
		virtualGeometryExecutorUnavailableReason = "Vulkan device or descriptor pool is unavailable.";
		return false;
	}

	DestroyVulkanVirtualGeometryExecutorPipeline();
	std::array<VkDescriptorSetLayoutBinding, 7> bindings{};
	for(std::uint32_t binding = 0u; binding < bindings.size(); ++binding) {
		bindings[binding].binding = binding;
		bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1u;
		bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
	setLayoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &virtualGeometryExecutorDescriptorSetLayout) != VK_SUCCESS) {
		virtualGeometryExecutorUnavailableReason = "Could not create the virtual-geometry compaction descriptor layout.";
		DestroyVulkanVirtualGeometryExecutorPipeline();
		return false;
	}

	const VkPushConstantRange pushRange{
		VK_SHADER_STAGE_COMPUTE_BIT,
		0u,
		static_cast<std::uint32_t>(sizeof(TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryCompactionPushConstants)) };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1u;
	pipelineLayoutInfo.pSetLayouts = &virtualGeometryExecutorDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1u;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &virtualGeometryExecutorPipelines.layout) != VK_SUCCESS) {
		virtualGeometryExecutorUnavailableReason = "Could not create the virtual-geometry compaction pipeline layout.";
		DestroyVulkanVirtualGeometryExecutorPipeline();
		return false;
	}

	const auto createPipeline = [this](const char* shaderPath, VkPipeline& pipeline) {
		std::vector<std::uint8_t> shaderBytes;
		try { shaderBytes = ResourceManager::Instance().ReadFileBytes(ResourceManager::Instance().ResolveEngineAssetPath(shaderPath)); }
		catch(const std::exception&) { return false; }
		if(shaderBytes.empty() || shaderBytes.size() % sizeof(std::uint32_t) != 0u) return false;
		VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = shaderBytes.size(); moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes.data());
		VkShaderModule module = VK_NULL_HANDLE;
		if(vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;
		VkPipelineShaderStageCreateInfo shaderStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT; shaderStage.module = module; shaderStage.pName = "main";
		VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pipelineInfo.stage = shaderStage; pipelineInfo.layout = virtualGeometryExecutorPipelines.layout;
		const bool created = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS;
		vkDestroyShaderModule(device, module, nullptr); return created;
	};
	if(!createPipeline("Shaders/VirtualGeometry/VirtualGeometryTraversal.comp.spv", virtualGeometryExecutorPipelines.traversal)
		|| !createPipeline("Shaders/VirtualGeometry/VirtualGeometryPacketCompaction.comp.spv", virtualGeometryExecutorPipelines.compaction)
		|| !createPipeline("Shaders/VirtualGeometry/VirtualGeometryIndirectArguments.comp.spv", virtualGeometryExecutorPipelines.indirectGeneration)) {
		virtualGeometryExecutorUnavailableReason = "One or more virtual-geometry traversal, compaction, or indirect SPIR-V pipelines are unavailable.";
		DestroyVulkanVirtualGeometryExecutorPipeline();
		return false;
	}
	virtualGeometryExecutorUnavailableReason.clear();
	return true;
}

void VulkanContext::Impl::DestroyVulkanVirtualGeometryExecutorPipeline()
{
	if(device != VK_NULL_HANDLE) {
		if(virtualGeometryExecutorPipelines.traversal != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryExecutorPipelines.traversal, nullptr);
		if(virtualGeometryExecutorPipelines.compaction != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, virtualGeometryExecutorPipelines.compaction, nullptr);
		}
		if(virtualGeometryExecutorPipelines.indirectGeneration != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryExecutorPipelines.indirectGeneration, nullptr);
		if(virtualGeometryExecutorPipelines.layout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, virtualGeometryExecutorPipelines.layout, nullptr);
		}
		if(virtualGeometryExecutorDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, virtualGeometryExecutorDescriptorSetLayout, nullptr);
		}
	}
	virtualGeometryExecutorPipelines = {};
	virtualGeometryExecutorDescriptorSetLayout = VK_NULL_HANDLE;
}

// The production decoder has a deliberately small, private ABI: a request,
// canonical ExpandedProduction bytes, an output buffer, and a host-visible
// completion record. Compressed profiles never reach this pipeline.
bool VulkanContext::Impl::EnsureVulkanVirtualGeometryProductionDecodePipeline()
{
	if(virtualGeometryProductionDecodePipelines.layout != VK_NULL_HANDLE
		&& virtualGeometryProductionDecodePipelines.expandedProductionDecode != VK_NULL_HANDLE
		&& virtualGeometryProductionDecodeDescriptorSetLayout != VK_NULL_HANDLE) {
		virtualGeometryProductionDecodeUnavailableReason.clear();
		return true;
	}
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE) {
		virtualGeometryProductionDecodeUnavailableReason = "Vulkan device or descriptor pool is unavailable.";
		return false;
	}

	DestroyVulkanVirtualGeometryProductionDecodePipeline();
	std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
	for(std::uint32_t binding = 0u; binding < bindings.size(); ++binding) {
		bindings[binding].binding = binding;
		bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1u;
		bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
	setLayoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr,
		&virtualGeometryProductionDecodeDescriptorSetLayout) != VK_SUCCESS) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not create the ExpandedProduction decode descriptor layout.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	const VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0u, 16u };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount = 1u;
	pipelineLayoutInfo.pSetLayouts = &virtualGeometryProductionDecodeDescriptorSetLayout;
	pipelineLayoutInfo.pushConstantRangeCount = 1u;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr,
		&virtualGeometryProductionDecodePipelines.layout) != VK_SUCCESS) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not create the ExpandedProduction decode pipeline layout.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	std::vector<std::uint8_t> shaderBytes;
	try {
		shaderBytes = ResourceManager::Instance().ReadFileBytes(
			ResourceManager::Instance().ResolveEngineAssetPath(
				"Shaders/VirtualGeometry/VirtualGeometryProductionDecode.comp.spv"));
	}
	catch(const std::exception&) {
		virtualGeometryProductionDecodeUnavailableReason = "ExpandedProduction decode SPIR-V is unavailable.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	if(shaderBytes.empty() || shaderBytes.size() % sizeof(std::uint32_t) != 0u) {
		virtualGeometryProductionDecodeUnavailableReason = "ExpandedProduction decode SPIR-V has an invalid byte length.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	moduleInfo.codeSize = shaderBytes.size();
	moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes.data());
	VkShaderModule module = VK_NULL_HANDLE;
	if(vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not create the ExpandedProduction decode shader module.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	VkPipelineShaderStageCreateInfo shaderStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
	shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	shaderStage.module = module;
	shaderStage.pName = "main";
	VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
	pipelineInfo.stage = shaderStage;
	pipelineInfo.layout = virtualGeometryProductionDecodePipelines.layout;
	const bool created = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1u,
		&pipelineInfo, nullptr, &virtualGeometryProductionDecodePipelines.expandedProductionDecode) == VK_SUCCESS;
	vkDestroyShaderModule(device, module, nullptr);
	if(!created) {
		virtualGeometryProductionDecodeUnavailableReason = "Could not create the ExpandedProduction decode compute pipeline.";
		DestroyVulkanVirtualGeometryProductionDecodePipeline();
		return false;
	}
	virtualGeometryProductionDecodeUnavailableReason.clear();
	return true;
}

void VulkanContext::Impl::DestroyVulkanVirtualGeometryProductionDecodePipeline()
{
	if(device != VK_NULL_HANDLE) {
		if(virtualGeometryProductionDecodePipelines.expandedProductionDecode != VK_NULL_HANDLE)
			vkDestroyPipeline(device, virtualGeometryProductionDecodePipelines.expandedProductionDecode, nullptr);
		if(virtualGeometryProductionDecodePipelines.layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(device, virtualGeometryProductionDecodePipelines.layout, nullptr);
		if(virtualGeometryProductionDecodeDescriptorSetLayout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(device, virtualGeometryProductionDecodeDescriptorSetLayout, nullptr);
	}
	virtualGeometryProductionDecodePipelines = {};
	virtualGeometryProductionDecodeDescriptorSetLayout = VK_NULL_HANDLE;
	// A recreated layout cannot use an old descriptor set. The pool owns old
	// sets until backend teardown, while the next preparation allocates a set
	// compatible with the new layout.
	for(auto& resources : virtualGeometryProductionDecodeResourcesByFrame)
		resources.descriptorSet = VK_NULL_HANDLE;
}

// Visibility is a separate compute chain from packet traversal. Its four
// storage-buffer bindings match the small, pass-local ABI used by the raster,
// resolve, and shading-bin shader assets.
bool VulkanContext::Impl::EnsureVulkanVirtualGeometryVisibilityPipelines()
{
	if(virtualGeometryVisibilityPipelines.layout != VK_NULL_HANDLE
		&& virtualGeometryVisibilityPipelines.triangleSetup != VK_NULL_HANDLE
		&& virtualGeometryVisibilityPipelines.computeRaster != VK_NULL_HANDLE
		&& virtualGeometryVisibilityPipelines.visibilityResolve != VK_NULL_HANDLE
		&& virtualGeometryVisibilityPipelines.shadingBins != VK_NULL_HANDLE
		&& virtualGeometryVisibilityDescriptorSetLayout != VK_NULL_HANDLE) {
		virtualGeometryVisibilityUnavailableReason.clear();
		return true;
	}
	if(device == VK_NULL_HANDLE || descriptorPool == VK_NULL_HANDLE
		|| globalDescriptorSetLayout == VK_NULL_HANDLE) {
		virtualGeometryVisibilityUnavailableReason = "Vulkan device, descriptor pool, or global descriptor layout is unavailable.";
		return false;
	}

	DestroyVulkanVirtualGeometryVisibilityPipelines();
	std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
	for(std::uint32_t binding = 0u; binding < bindings.size(); ++binding) {
		bindings[binding].binding = binding;
		bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[binding].descriptorCount = 1u;
		bindings[binding].stageFlags =
			VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
	setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	setLayoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
	setLayoutInfo.pBindings = bindings.data();
	if(vkCreateDescriptorSetLayout(device, &setLayoutInfo, nullptr, &virtualGeometryVisibilityDescriptorSetLayout) != VK_SUCCESS) {
		virtualGeometryVisibilityUnavailableReason = "Could not create the virtual-geometry visibility descriptor layout.";
		return false;
	}
	// Compute raster additionally receives explicit target width/height. Resolve
	// and shading bins use the first 16 bytes of the same compatible range.
	const VkPushConstantRange pushRange{
		VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
		0u,
		24u };
	const std::array<VkDescriptorSetLayout, 2u> setLayouts{
		globalDescriptorSetLayout,
		virtualGeometryVisibilityDescriptorSetLayout };
	VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
	pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipelineLayoutInfo.setLayoutCount =
		static_cast<std::uint32_t>(setLayouts.size());
	pipelineLayoutInfo.pSetLayouts = setLayouts.data();
	pipelineLayoutInfo.pushConstantRangeCount = 1u;
	pipelineLayoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &virtualGeometryVisibilityPipelines.layout) != VK_SUCCESS) {
		virtualGeometryVisibilityUnavailableReason = "Could not create the virtual-geometry visibility pipeline layout.";
		DestroyVulkanVirtualGeometryVisibilityPipelines();
		return false;
	}
	const auto createPipeline = [this](const char* shaderPath, VkPipeline& pipeline) {
		std::vector<std::uint8_t> shaderBytes;
		try { shaderBytes = ResourceManager::Instance().ReadFileBytes(ResourceManager::Instance().ResolveEngineAssetPath(shaderPath)); }
		catch(const std::exception&) { return false; }
		if(shaderBytes.empty() || shaderBytes.size() % sizeof(std::uint32_t) != 0u) return false;
		VkShaderModuleCreateInfo moduleInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		moduleInfo.codeSize = shaderBytes.size();
		moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(shaderBytes.data());
		VkShaderModule module = VK_NULL_HANDLE;
		if(vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) return false;
		VkPipelineShaderStageCreateInfo shaderStage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
		shaderStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		shaderStage.module = module;
		shaderStage.pName = "main";
		VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
		pipelineInfo.stage = shaderStage;
		pipelineInfo.layout = virtualGeometryVisibilityPipelines.layout;
		const bool created = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline) == VK_SUCCESS;
		vkDestroyShaderModule(device, module, nullptr);
		return created;
	};
	if(!createPipeline("Shaders/VirtualGeometry/VirtualGeometryTriangleSetup.comp.spv", virtualGeometryVisibilityPipelines.triangleSetup)
		|| !createPipeline("Shaders/VirtualGeometry/VirtualGeometryComputeRaster.comp.spv", virtualGeometryVisibilityPipelines.computeRaster)
		|| !createPipeline("Shaders/VirtualGeometry/VirtualGeometryVisibilityResolve.comp.spv", virtualGeometryVisibilityPipelines.visibilityResolve)
		|| !createPipeline("Shaders/VirtualGeometry/VirtualGeometryShadingBins.comp.spv", virtualGeometryVisibilityPipelines.shadingBins)) {
		virtualGeometryVisibilityUnavailableReason = "One or more virtual-geometry visibility SPIR-V pipelines are unavailable.";
		DestroyVulkanVirtualGeometryVisibilityPipelines();
		return false;
	}
	virtualGeometryVisibilityUnavailableReason.clear();
	return true;
}

VkPipeline VulkanContext::Impl::EnsureVulkanVirtualGeometryCompositePipeline(
	VkRenderPass renderPass)
{
	if(renderPass == VK_NULL_HANDLE || !taskShaderSupported
		|| !EnsureVulkanVirtualGeometryVisibilityPipelines())
	{
		return VK_NULL_HANDLE;
	}
	if(const auto existing = virtualGeometryCompositePipelines.find(renderPass);
		existing != virtualGeometryCompositePipelines.end())
	{
		return existing->second;
	}
	const auto loadModule = [this](const char* shaderPath) -> VkShaderModule
	{
		std::vector<std::uint8_t> shaderBytes;
		try
		{
			shaderBytes = ResourceManager::Instance().ReadFileBytes(
				ResourceManager::Instance().ResolveEngineAssetPath(shaderPath));
		}
		catch(const std::exception&)
		{
			return VK_NULL_HANDLE;
		}
		if(shaderBytes.empty()
			|| shaderBytes.size() % sizeof(std::uint32_t) != 0u)
		{
			return VK_NULL_HANDLE;
		}
		VkShaderModuleCreateInfo moduleInfo{};
		moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = shaderBytes.size();
		moduleInfo.pCode = reinterpret_cast<const std::uint32_t*>(
			shaderBytes.data());
		VkShaderModule module = VK_NULL_HANDLE;
		return vkCreateShaderModule(device, &moduleInfo, nullptr, &module)
			== VK_SUCCESS ? module : VK_NULL_HANDLE;
	};
	VkShaderModule taskModule = loadModule(
		"Shaders/VirtualGeometry/VirtualGeometryComposite.task.spv");
	VkShaderModule meshModule = loadModule(
		"Shaders/VirtualGeometry/VirtualGeometryComposite.mesh.spv");
	VkShaderModule fragmentModule = loadModule(
		"Shaders/VirtualGeometry/VirtualGeometryComposite.frag.spv");
	if(taskModule == VK_NULL_HANDLE || meshModule == VK_NULL_HANDLE
		|| fragmentModule == VK_NULL_HANDLE)
	{
		if(taskModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, taskModule, nullptr);
		if(meshModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, meshModule, nullptr);
		if(fragmentModule != VK_NULL_HANDLE)
			vkDestroyShaderModule(device, fragmentModule, nullptr);
		return VK_NULL_HANDLE;
	}
	const std::array<VkPipelineShaderStageCreateInfo, 3u> shaderStages{{
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u,
			VK_SHADER_STAGE_TASK_BIT_EXT, taskModule, "main", nullptr },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u,
			VK_SHADER_STAGE_MESH_BIT_EXT, meshModule, "main", nullptr },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0u,
			VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr } }};
	VkPipelineViewportStateCreateInfo viewportState{};
	viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewportState.viewportCount = 1u;
	viewportState.scissorCount = 1u;
	VkPipelineRasterizationStateCreateInfo rasterization{};
	rasterization.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rasterization.polygonMode = VK_POLYGON_MODE_FILL;
	rasterization.cullMode = VK_CULL_MODE_NONE;
	rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rasterization.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo multisample{};
	multisample.sType =
		VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo depthStencil{};
	depthStencil.sType =
		VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
	const std::uint32_t colorAttachmentCount =
		UsesConventionalVisibilityAttachments(renderPass) ? 2u : 1u;
	std::array<VkPipelineColorBlendAttachmentState, 2u> blendAttachments{};
	for(std::uint32_t attachment = 0u;
		attachment < colorAttachmentCount;
		++attachment)
	{
		blendAttachments[attachment].blendEnable = VK_FALSE;
		blendAttachments[attachment].colorWriteMask =
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	}
	VkPipelineColorBlendStateCreateInfo blendState{};
	blendState.sType =
		VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blendState.attachmentCount = colorAttachmentCount;
	blendState.pAttachments = blendAttachments.data();
	const std::array<VkDynamicState, 2u> dynamicStates{
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamicState{};
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.dynamicStateCount =
		static_cast<std::uint32_t>(dynamicStates.size());
	dynamicState.pDynamicStates = dynamicStates.data();
	VkGraphicsPipelineCreateInfo pipelineInfo{};
	pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipelineInfo.stageCount =
		static_cast<std::uint32_t>(shaderStages.size());
	pipelineInfo.pStages = shaderStages.data();
	// Mesh pipelines synthesize their own vertices and primitives; Vulkan must
	// not receive legacy vertex-input or input-assembly state for this pass.
	pipelineInfo.pVertexInputState = nullptr;
	pipelineInfo.pInputAssemblyState = nullptr;
	pipelineInfo.pViewportState = &viewportState;
	pipelineInfo.pRasterizationState = &rasterization;
	pipelineInfo.pMultisampleState = &multisample;
	pipelineInfo.pDepthStencilState = &depthStencil;
	pipelineInfo.pColorBlendState = &blendState;
	pipelineInfo.pDynamicState = &dynamicState;
	pipelineInfo.layout = virtualGeometryVisibilityPipelines.layout;
	pipelineInfo.renderPass = renderPass;
	pipelineInfo.subpass = 0u;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult result = vkCreateGraphicsPipelines(
		device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline);
	vkDestroyShaderModule(device, fragmentModule, nullptr);
	vkDestroyShaderModule(device, meshModule, nullptr);
	vkDestroyShaderModule(device, taskModule, nullptr);
	if(result != VK_SUCCESS)
	{
		return VK_NULL_HANDLE;
	}
	virtualGeometryCompositePipelines.emplace(renderPass, pipeline);
	return pipeline;
}

void VulkanContext::Impl::DestroyVulkanVirtualGeometryVisibilityPipelines()
{
	if(device != VK_NULL_HANDLE) {
		for(auto& [renderPass, pipeline] : virtualGeometryCompositePipelines)
		{
			(void)renderPass;
			if(pipeline != VK_NULL_HANDLE)
				vkDestroyPipeline(device, pipeline, nullptr);
		}
		if(virtualGeometryVisibilityPipelines.triangleSetup != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryVisibilityPipelines.triangleSetup, nullptr);
		if(virtualGeometryVisibilityPipelines.computeRaster != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryVisibilityPipelines.computeRaster, nullptr);
		if(virtualGeometryVisibilityPipelines.visibilityResolve != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryVisibilityPipelines.visibilityResolve, nullptr);
		if(virtualGeometryVisibilityPipelines.shadingBins != VK_NULL_HANDLE) vkDestroyPipeline(device, virtualGeometryVisibilityPipelines.shadingBins, nullptr);
		if(virtualGeometryVisibilityPipelines.layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(device, virtualGeometryVisibilityPipelines.layout, nullptr);
		if(virtualGeometryVisibilityDescriptorSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device, virtualGeometryVisibilityDescriptorSetLayout, nullptr);
	}
	virtualGeometryVisibilityPipelines = {};
	virtualGeometryVisibilityDescriptorSetLayout = VK_NULL_HANDLE;
	virtualGeometryCompositePipelines.clear();
	for(auto& frameResources : virtualGeometryGpuPacketResourcesByFrame)
	{
		for(auto& resources : frameResources)
		{
			resources.visibilityTriangleSetupDescriptorSet = VK_NULL_HANDLE;
			resources.visibilityComputeRasterDescriptorSet = VK_NULL_HANDLE;
			resources.visibilityResolveDescriptorSet = VK_NULL_HANDLE;
			resources.visibilityShadingBinsDescriptorSet = VK_NULL_HANDLE;
		}
	}
}

bool VulkanContext::Impl::EnsureVulkanDeformationComputePipelines()
{
	using Stage = TerraEngine::Renderer::Vulkan::
		VulkanDeformationPipelineStage;
	const auto Pipeline = [this](Stage stage) -> VkPipeline
	{
		return deformationComputePipelines.byStage[
			static_cast<std::size_t>(stage)];
	};
	if(deformationComputePipelines.layout != VK_NULL_HANDLE
		&& Pipeline(Stage::Deform) != VK_NULL_HANDLE
		&& Pipeline(Stage::Bounds) != VK_NULL_HANDLE
		&& Pipeline(Stage::Refinement) != VK_NULL_HANDLE)
	{
		return true;
	}
	// Deformation shaders occupy descriptor set 2, so Vulkan requires valid
	// layouts for sets 0 and 1 even when CPU animation owns the palette and no
	// GPU animation dispatch exists. Create the structural animation layout here
	// instead of relying on an earlier GPU-animation frame to have done so.
	const bool animationLayoutReady =
		animationDescriptorSetLayout != VK_NULL_HANDLE
		|| EnsureVulkanAnimationDescriptorSets();
	if(device == VK_NULL_HANDLE
		|| globalDescriptorSetLayout == VK_NULL_HANDLE
		|| !animationLayoutReady
		|| deformationDescriptorSetLayout == VK_NULL_HANDLE)
	{
		return false;
	}
	DestroyVulkanDeformationComputePipelines();
	const std::array<VkDescriptorSetLayout, 3u> setLayouts{
		globalDescriptorSetLayout,
		animationDescriptorSetLayout,
		deformationDescriptorSetLayout };
	VkPushConstantRange pushRange{};
	pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pushRange.offset = 0u;
	pushRange.size = 16u;
	VkPipelineLayoutCreateInfo layoutInfo{};
	layoutInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layoutInfo.setLayoutCount =
		static_cast<std::uint32_t>(setLayouts.size());
	layoutInfo.pSetLayouts = setLayouts.data();
	layoutInfo.pushConstantRangeCount = 1u;
	layoutInfo.pPushConstantRanges = &pushRange;
	if(vkCreatePipelineLayout(
		device,
		&layoutInfo,
		nullptr,
		&deformationComputePipelines.layout) != VK_SUCCESS)
	{
		deformationComputePipelines.layout = VK_NULL_HANDLE;
		return false;
	}
	const auto CreatePipeline = [this](
		Stage stage,
		const char* shaderPath)
	{
		std::vector<std::uint8_t> shaderBytes;
		try
		{
			shaderBytes =
				ResourceManager::Instance().ReadFileBytes(
					ResourceManager::Instance().
						ResolveEngineAssetPath(shaderPath));
		}
		catch(const std::exception&)
		{
			return false;
		}
		if(shaderBytes.empty()
			|| shaderBytes.size() % sizeof(std::uint32_t) != 0u)
		{
			return false;
		}
		VkShaderModuleCreateInfo moduleInfo{};
		moduleInfo.sType =
			VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleInfo.codeSize = shaderBytes.size();
		moduleInfo.pCode =
			reinterpret_cast<const std::uint32_t*>(
				shaderBytes.data());
		VkShaderModule module = VK_NULL_HANDLE;
		if(vkCreateShaderModule(
			device,
			&moduleInfo,
			nullptr,
			&module) != VK_SUCCESS)
		{
			return false;
		}
		VkPipelineShaderStageCreateInfo stageInfo{};
		stageInfo.sType =
			VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		stageInfo.module = module;
		stageInfo.pName = "main";
		VkComputePipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType =
			VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		pipelineInfo.stage = stageInfo;
		pipelineInfo.layout =
			deformationComputePipelines.layout;
		VkPipeline& pipeline =
			deformationComputePipelines.byStage[
				static_cast<std::size_t>(stage)];
		const VkResult result = vkCreateComputePipelines(
			device,
			VK_NULL_HANDLE,
			1u,
			&pipelineInfo,
			nullptr,
			&pipeline);
		vkDestroyShaderModule(device, module, nullptr);
		return result == VK_SUCCESS;
	};
	const bool created =
		CreatePipeline(
			Stage::Deform,
			"Shaders/VirtualGeometry/AnimatedPageDeform.comp.spv")
		&& CreatePipeline(
			Stage::Bounds,
			"Shaders/VirtualGeometry/AnimatedClusterBounds.comp.spv")
		&& CreatePipeline(
			Stage::Refinement,
			"Shaders/VirtualGeometry/AnimatedRefinement.comp.spv");
	if(!created)
	{
		TERRAENGINE_LOG(
			LogLevel::Warning,
			"Animated virtual geometry compute shaders are unavailable; "
			"the renderer will retain conservative owner coverage.");
		DestroyVulkanDeformationComputePipelines();
		return false;
	}
	return true;
}

void VulkanContext::Impl::DestroyVulkanDeformationComputePipelines()
{
	if(device != VK_NULL_HANDLE)
	{
		for(VkPipeline& pipeline :
			deformationComputePipelines.byStage)
		{
			if(pipeline != VK_NULL_HANDLE)
			{
				vkDestroyPipeline(device, pipeline, nullptr);
			}
			pipeline = VK_NULL_HANDLE;
		}
		if(deformationComputePipelines.layout != VK_NULL_HANDLE)
		{
			vkDestroyPipelineLayout(
				device,
				deformationComputePipelines.layout,
				nullptr);
		}
	}
	deformationComputePipelines = {};
}

// VulkanContext::Impl::Cleanup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::Cleanup(bool skipDeviceIdle) {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	bool presentationFenceWaitAllowed = device != VK_NULL_HANDLE && !skipDeviceIdle && !deviceLost;
	if (device && !skipDeviceIdle) {
		const VkResult idleResult = WaitDeviceIdle();
		if (idleResult != VK_SUCCESS)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Vulkan cleanup could not prove the device idle before teardown: " << idleResult);
			// A lost device cannot signal further presentation fences. Terminal
			// device-loss cleanup therefore skips the otherwise blocking proof wait.
			if (idleResult == VK_ERROR_DEVICE_LOST) presentationFenceWaitAllowed = false;
		}
	}
	if (presentationFenceWaitAllowed) {
		// Device-idle alone does not prove that the presentation engine released a
		// present wait semaphore. Maintenance fences provide that explicit proof for
		// every queued present and must signal before their owning bundles are torn down.
		std::vector<VkFence> pendingPresentationFences;
		for (VkFence fence : presentationCompletionFences) {
			if (fence != VK_NULL_HANDLE) pendingPresentationFences.push_back(fence);
		}
		for (const RetiredPresentationResourceBundle& retired : retiredPresentationBundles) {
			for (VkFence fence : retired.resources.presentCompletionFences) {
				if (fence != VK_NULL_HANDLE) pendingPresentationFences.push_back(fence);
			}
		}
		for (const PendingUnextendedAcquireProof& proof : pendingUnextendedAcquireProofs) {
			if (proof.fence != VK_NULL_HANDLE) pendingPresentationFences.push_back(proof.fence);
		}
		if (!pendingPresentationFences.empty()) {
			const VkResult presentationIdleResult = vkWaitForFences(device,
				static_cast<std::uint32_t>(pendingPresentationFences.size()), pendingPresentationFences.data(),
				VK_TRUE, std::numeric_limits<std::uint64_t>::max());
			if (presentationIdleResult != VK_SUCCESS) {
				TERRAENGINE_LOG(LogLevel::Warning, "Vulkan cleanup could not prove presentation completion before teardown: "
					<< presentationIdleResult);
			}
		}
	}

	// Readback objects are not part of the normal frame arrays. Drain them
	// explicitly while the allocator, command pools, and device still exist.
	// Device-loss teardown is terminal for submitted work, while normal cleanup
	// has already waited for device idle above.
	CancelReadbackRequests(
		deviceLost ? "device_lost" : "canceled",
		deviceLost
			? "Vulkan image readback was canceled because the device was lost."
			: "Vulkan image readback was canceled during renderer shutdown.",
		true);
	for (VulkanConventionalVisibilityIdentityReadbackRequest& request :
		inFlightConventionalVisibilityIdentityReadbacks) {
		DestroyConventionalVisibilityIdentityReadbackRequestResources(request);
	}
	inFlightConventionalVisibilityIdentityReadbacks.clear();
	completedConventionalVisibilityIdentityReadbacks.clear();

	DestroyAmbientOcclusionResources();
	DestroyEnvironmentLightingResources();
	DestroyGpuTimestampQueries();
	if (hiZDescriptorPool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
		hiZDescriptorPool = VK_NULL_HANDLE;
		hiZDescriptorSets.clear();
	}
	if (aoDescriptorPool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
		aoDescriptorPool = VK_NULL_HANDLE;
		aoDescriptorSets.clear();
	}
	for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
		for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
			for (VulkanHiZ& frameHiZ : frameHiZPair) {
				frameHiZ.Destroy(device, allocator);
			}
		}
	}
	vrs.Destroy(device, allocator);

	DestroyWorkerPools();
	CleanupSwapChain();
	// Renderer teardown ends the presentation session. Normal cleanup waited for
	// the device above; device-loss teardown is terminal, so no live renderer can
	// acquire these handles after this point.
	for (RetiredPresentationResourceBundle& retired : retiredPresentationBundles) {
		DestroyPresentationResourceBundle(retired.resources);
	}
	retiredPresentationBundles.clear();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [id, mesh] : meshCache) {
		if (!mesh.descriptorSetsByLayout.empty() && descriptorPool != VK_NULL_HANDLE) {
			std::vector<VkDescriptorSet> descriptorSets;
			descriptorSets.reserve(mesh.descriptorSetsByLayout.size());
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& [layout, set] : mesh.descriptorSetsByLayout) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (set != VK_NULL_HANDLE) {
					descriptorSets.push_back(set);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!descriptorSets.empty()) {
				vkFreeDescriptorSets(device, descriptorPool, static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data());
			}
			mesh.descriptorSetsByLayout.clear();
		}

		vmaDestroyBuffer(allocator, mesh.indexBuffer.buffer, mesh.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshShaderVertexBuffer.buffer, mesh.meshShaderVertexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletVertexRemapBuffer.buffer, mesh.meshletVertexRemapBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshSkinningBuffer.buffer, mesh.meshSkinningBuffer.allocation);
		if (mesh.meshMorphDeltaBuffer.buffer != VK_NULL_HANDLE) {
			// Morph deltas are a separate graphics stream. They have the same mesh
			// lifetime as skinning bytes and must be released before VMA shutdown.
			vmaDestroyBuffer(allocator, mesh.meshMorphDeltaBuffer.buffer, mesh.meshMorphDeltaBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, mesh.meshDecodeInfoBuffer.buffer, mesh.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBuffer.buffer, mesh.meshletBuffer.allocation);
		if (mesh.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, mesh.meshletSectionBuffer.buffer, mesh.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

	// Grouped render-instance streams are frame-slot scratch buffers rather than
	// mesh-owned resources. Cleanup runs after the device-idle boundary, so each
	// live slot can be released directly before VMA and the Vulkan device vanish.
	for (VulkanBuffer& buffer : groupedInstanceBuffers)
	{
		DestroyBufferImmediate(buffer);
	}
	groupedInstanceCapacities.fill(0u);
	for (std::vector<RenderInstanceRecord>& mirror : groupedInstanceUploadMirrors)
	{
		mirror.clear();
	}
	if (groupedInstanceBufferIndex != INVALID_BINDLESS_BUFFER_INDEX)
	{
		freeBufferIndices.push_back(groupedInstanceBufferIndex);
		groupedInstanceBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	}

	// Virtual geometry packet resources are per-view, per-frame scratch buffers
	// published through the bindless table. They are not owned by meshCache, so
	// shutdown must drain them explicitly after the device is idle and before
	// VMA is destroyed; otherwise the final frame's packet, batch, compaction,
	// generated-submission, and counter-readback buffers survive teardown.
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
	activeVirtualGeometryPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryBatchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCompactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryGeneratedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCounterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;

	// Animation frame buffers are not part of meshCache. Destroy them while the
	// allocator remains valid, then release the dependent pipelines before their
	// descriptor-set layout and pool are torn down below.
	DestroyVulkanDeformationComputePipelines();
	for(VulkanDeformationFrameResources& resources :
		deformationFrameResources)
	{
		for(VulkanBuffer& buffer : resources.buffers)
		{
			DestroyBufferImmediate(buffer);
		}
		DestroyBufferImmediate(resources.graphicsBridgeBuffer);
		resources = {};
	}
	deformationFrameResources.clear();
	pendingDeformationImmutablePlan.reset();
	pendingDeformationCommandPlan = {};
	deformationComputeRecordedForCurrentFrame = false;
	recordedDeformationDispatchCountForCurrentFrame = 0u;
	activeDeformationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	if(device != VK_NULL_HANDLE
		&& descriptorPool != VK_NULL_HANDLE
		&& !deformationDescriptorSets.empty())
	{
		vkFreeDescriptorSets(
			device,
			descriptorPool,
			static_cast<std::uint32_t>(
				deformationDescriptorSets.size()),
			deformationDescriptorSets.data());
	}
	deformationDescriptorSets.clear();
	if(device != VK_NULL_HANDLE
		&& deformationDescriptorSetLayout != VK_NULL_HANDLE)
	{
		vkDestroyDescriptorSetLayout(
			device,
			deformationDescriptorSetLayout,
			nullptr);
	}
	deformationDescriptorSetLayout = VK_NULL_HANDLE;

	DestroyVulkanAnimationComputePipelines();
	DestroyVulkanVirtualGeometryExecutorPipeline();
	DestroyVulkanVirtualGeometryProductionDecodePipeline();
	DestroyVulkanVirtualGeometryVisibilityPipelines();
	for (VulkanAnimationFrameResources& resources : animationFrameResources) {
		for (VulkanBuffer& buffer : resources.buffers) {
			DestroyBufferImmediate(buffer);
		}
		DestroyBufferImmediate(resources.graphicsBridgeBuffer);
		resources = {};
	}
	animationFrameResources.clear();
	for (VulkanCpuAnimationFrameResources& resources
		: cpuAnimationFrameResources) {
		DestroyBufferImmediate(resources.paletteBuffer);
		DestroyBufferImmediate(resources.morphWeightBuffer);
		DestroyBufferImmediate(resources.graphicsBridgeBuffer);
		resources = {};
	}
	cpuAnimationFrameResources.clear();
	pendingAnimationImmutablePlan.reset();
	pendingCpuAnimationPlan.reset();
	pendingAnimationCommandPlan = {};
	pendingAnimationFrameId = 0U;
	animationComputeRecordedForCurrentFrame = false;
	animationComputeSubmissionMarkedForCurrentFrame = false;
	recordedAnimationDispatchCountForCurrentFrame = 0U;
	activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	completedAnimationGenerations.clear();
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		latestAnimationGpuBackendExecution = {};
	}
	if (device != VK_NULL_HANDLE && descriptorPool != VK_NULL_HANDLE && !animationDescriptorSets.empty()) {
		vkFreeDescriptorSets(device, descriptorPool,
			static_cast<std::uint32_t>(animationDescriptorSets.size()), animationDescriptorSets.data());
	}
	animationDescriptorSets.clear();
	if (device != VK_NULL_HANDLE && animationDescriptorSetLayout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(device, animationDescriptorSetLayout, nullptr);
	}
	animationDescriptorSetLayout = VK_NULL_HANDLE;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& retiredBuffers : retiredBuffersByFrame) {
		for (VulkanBuffer& retiredBuffer : retiredBuffers) {
			DestroyBufferImmediate(retiredBuffer);
		}
		retiredBuffers.clear();
	}
	retiredBuffersByFrame.clear();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& [id, tex] : textureCache) {
		if (device) {
			vkDestroySampler(device, tex.sampler, nullptr);
			vkDestroyImageView(device, tex.view, nullptr);
		}
		vmaDestroyImage(allocator, tex.image, tex.allocation);
	}
	textureCache.clear();

	DestroyOffscreenResourceSet(sceneView);
	DestroyOffscreenResourceSet(gameView);
	DestroyOffscreenResourceSet(captureView);
	for (RetiredOffscreenResourceSet& retired : retiredOffscreenResources) {
		DestroyOffscreenResourceSet(retired.resources);
	}
	retiredOffscreenResources.clear();
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device && offscreenRenderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(device, offscreenRenderPass, nullptr);
		offscreenRenderPass = VK_NULL_HANDLE;
	}

	DestroyPipelines();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		if (sceneViewGridPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, sceneViewGridPipeline, nullptr);
			sceneViewGridPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sceneViewGridPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, sceneViewGridPipelineLayout, nullptr);
			sceneViewGridPipelineLayout = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyBackgroundPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, skyBackgroundPipeline, nullptr);
			skyBackgroundPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (skyBackgroundPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, skyBackgroundPipelineLayout, nullptr);
			skyBackgroundPipelineLayout = VK_NULL_HANDLE;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		vkDestroySampler(device, defaultTexture.sampler, nullptr);
		vkDestroyImageView(device, defaultTexture.view, nullptr);
		vkDestroySampler(device, defaultNormalTexture.sampler, nullptr);
		vkDestroyImageView(device, defaultNormalTexture.view, nullptr);
		vkDestroySampler(device, defaultMaterialMaskTexture.sampler, nullptr);
		vkDestroyImageView(device, defaultMaterialMaskTexture.view, nullptr);
	}
	vmaDestroyImage(allocator, defaultTexture.image, defaultTexture.allocation);
	vmaDestroyImage(allocator, defaultNormalTexture.image, defaultNormalTexture.allocation);
	vmaDestroyImage(allocator, defaultMaterialMaskTexture.image, defaultMaterialMaskTexture.allocation);

	// UI Rendering cleanup
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		if (staticUiPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, staticUiPipeline, nullptr);
			staticUiPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (staticUiPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, staticUiPipelineLayout, nullptr);
			staticUiPipelineLayout = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (instancedUiPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, instancedUiPipeline, nullptr);
			instancedUiPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (instancedUiPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, instancedUiPipelineLayout, nullptr);
			instancedUiPipelineLayout = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sdfTextPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, sdfTextPipeline, nullptr);
			sdfTextPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sdfTextPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, sdfTextPipelineLayout, nullptr);
			sdfTextPipelineLayout = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, uiFontDescriptorSetLayout, nullptr);
			uiFontDescriptorSetLayout = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontAtlasImage.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, uiFontAtlasImage.sampler, nullptr);
			uiFontAtlasImage.sampler = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (uiFontAtlasImage.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, uiFontAtlasImage.view, nullptr);
			uiFontAtlasImage.view = VK_NULL_HANDLE;
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiFontAtlasImage.image != VK_NULL_HANDLE && uiFontAtlasImage.allocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, uiFontAtlasImage.image, uiFontAtlasImage.allocation);
		uiFontAtlasImage.image = VK_NULL_HANDLE;
		uiFontAtlasImage.allocation = VK_NULL_HANDLE;
	}
	uiFontDescriptorSet = VK_NULL_HANDLE;
	uiFontAtlasWidth = 0;
	uiFontAtlasHeight = 0;
	// Each runtime UI buffer belongs to one fence slot. Cleanup runs after the
	// device-idle lifetime gate, so every slot can now be destroyed together.
	for (VulkanBuffer& buffer : uiStaticVertexBuffers) {
		if (buffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
		}
	}
	uiStaticVertexBuffers.clear();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiInstanceBuffer.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, uiInstanceBuffer.buffer, uiInstanceBuffer.allocation);
		uiInstanceBuffer.buffer = VK_NULL_HANDLE;
	}
	for (VulkanBuffer& buffer : uiSdfVertexBuffers) {
		if (buffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
		}
	}
	uiSdfVertexBuffers.clear();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		if (clusterGridComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, clusterGridComputePipeline, nullptr);
			clusterGridComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterDepthMaskComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, clusterDepthMaskComputePipeline, nullptr);
			clusterDepthMaskComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterLightCountComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, clusterLightCountComputePipeline, nullptr);
			clusterLightCountComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterLightPrefixComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, clusterLightPrefixComputePipeline, nullptr);
			clusterLightPrefixComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (clusterLightFillComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, clusterLightFillComputePipeline, nullptr);
			clusterLightFillComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lightAnalysisComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, lightAnalysisComputePipeline, nullptr);
			lightAnalysisComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (hiZComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, hiZComputePipeline, nullptr);
			hiZComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vrsComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, vrsComputePipeline, nullptr);
			vrsComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (gtaoComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, gtaoComputePipeline, nullptr);
			gtaoComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (gtaoBlurComputePipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, gtaoBlurComputePipeline, nullptr);
			gtaoBlurComputePipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lightingComputePipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, lightingComputePipelineLayout, nullptr);
			lightingComputePipelineLayout = VK_NULL_HANDLE;
		}
		if (hiZDescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
		}
		if (hiZPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, hiZPipelineLayout, nullptr);
			hiZPipelineLayout = VK_NULL_HANDLE;
		}
		if (hiZDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, hiZDescriptorSetLayout, nullptr);
			hiZDescriptorSetLayout = VK_NULL_HANDLE;
		}
		if (aoDescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
		}
		if (aoPipelineLayout != VK_NULL_HANDLE) {
			vkDestroyPipelineLayout(device, aoPipelineLayout, nullptr);
			aoPipelineLayout = VK_NULL_HANDLE;
		}
		if (aoDescriptorSetLayout != VK_NULL_HANDLE) {
			vkDestroyDescriptorSetLayout(device, aoDescriptorSetLayout, nullptr);
			aoDescriptorSetLayout = VK_NULL_HANDLE;
		}
		auto DestroyShadowFramebuffer = [&](VkFramebuffer& framebuffer) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (framebuffer != VK_NULL_HANDLE) {
				vkDestroyFramebuffer(device, framebuffer, nullptr);
				framebuffer = VK_NULL_HANDLE;
			}
		};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : directionalShadowFramebuffers) {
			DestroyShadowFramebuffer(framebuffer);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : spotShadowFramebuffers) {
			DestroyShadowFramebuffer(framebuffer);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (VkFramebuffer& framebuffer : areaShadowFramebuffers) {
			DestroyShadowFramebuffer(framebuffer);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::array<VkFramebuffer, 6>& faceFramebuffers : pointShadowFaceFramebuffersBySlot) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (VkFramebuffer& framebuffer : faceFramebuffers) {
				DestroyShadowFramebuffer(framebuffer);
			}
		}
		auto DestroyShadowImageView = [&](VkImageView& imageView) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (imageView != VK_NULL_HANDLE) {
				vkDestroyImageView(device, imageView, nullptr);
				imageView = VK_NULL_HANDLE;
			}
		};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::array<VkImageView, 6>& faceViews : pointShadowDepthFaceViewsBySlot) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (VkImageView& faceView : faceViews) {
				DestroyShadowImageView(faceView);
			}
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::array<VkImageView, 6>& faceViews : pointShadowColorFaceViewsBySlot) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (VkImageView& faceView : faceViews) {
				DestroyShadowImageView(faceView);
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shadowRenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(device, shadowRenderPass, nullptr);
			shadowRenderPass = VK_NULL_HANDLE;
		}
		activeShadowResourceRequest = {};
		shadowStorageRegions.fill({});
		shadowCacheEntries.clear();
	}

	auto DestroyVulkanImage = [&](VulkanImage& image) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (device && image.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, image.sampler, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (device && image.view != VK_NULL_HANDLE) {
			vkDestroyImageView(device, image.view, nullptr);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (image.image != VK_NULL_HANDLE && image.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, image.image, image.allocation);
		}
		image.image = VK_NULL_HANDLE;
		image.allocation = VK_NULL_HANDLE;
		image.view = VK_NULL_HANDLE;
		image.sampler = VK_NULL_HANDLE;
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : directionalShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : spotShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : areaShadowColors) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeDepths) {
		DestroyVulkanImage(image);
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (VulkanImage& image : pointShadowCubeColors) {
		DestroyVulkanImage(image);
	}
	DestroyVulkanImage(ltcAmplitudeTexture);
	DestroyVulkanImage(ltcInverseTexture);

	auto DestroyBufferVector = [&](std::vector<VulkanBuffer>& buffers) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& buffer : buffers) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (buffer.buffer != VK_NULL_HANDLE) {
				vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
			}
		}
		buffers.clear();
	};

	DestroyBufferVector(globalMaterialSSBOs);
	DestroyBufferVector(globalFrameCullingSSBOs);
	DestroyBufferVector(globalMeshletCountersSSBOs);
	DestroyBufferVector(globalLightSSBOs);
	DestroyBufferVector(lightAnalysisInputSSBOs);
	DestroyBufferVector(clusterGridSSBOs);
	DestroyBufferVector(lightIndexSSBOs);
	DestroyBufferVector(clusterDepthSSBOs);
	DestroyBufferVector(lightAnalysisSSBOs);
	DestroyBufferVector(indirectDrawBuffers);
	DestroyAmbientOcclusionResources();

	for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
		for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
			for (VulkanHiZ& frameHiZ : frameHiZPair) {
				frameHiZ.Destroy(device, allocator);
			}
		}
	}
	vrs.Destroy(device, allocator);

	DestroyFrameSyncObjects();
	DestroyPresentationImageSyncObjects();
	if (device != VK_NULL_HANDLE) {
		for (const PendingUnextendedAcquireProof& proof : pendingUnextendedAcquireProofs) {
			if (proof.fence != VK_NULL_HANDLE) vkDestroyFence(device, proof.fence, nullptr);
		}
	}
	pendingUnextendedAcquireProofs.clear();
	unextendedRetirementTracker.Reset();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		vkDestroyImageView(device, depthImageView, nullptr);
		vmaDestroyImage(allocator, depthImage, depthImageAllocation);
		depthImageView = VK_NULL_HANDLE;
		depthImage = VK_NULL_HANDLE;
		depthImageAllocation = VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		for (VkCommandPool pool : commandPools) {
			vkDestroyCommandPool(device, pool, nullptr);
		}
		for (VkCommandPool pool : editorCommandPools) {
			vkDestroyCommandPool(device, pool, nullptr);
		}
	}
	commandPools.clear();
	editorCommandPools.clear();
	editorCommandBuffers.clear();
	if (device && immediateCommandPool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device, immediateCommandPool, nullptr);
		immediateCommandPool = VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		if (fallbackPointShadowSampler != VK_NULL_HANDLE) {
			vkDestroySampler(device, fallbackPointShadowSampler, nullptr);
			fallbackPointShadowSampler = VK_NULL_HANDLE;
		}
		vkDestroyDescriptorPool(device, editorDescriptorPool, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		vkDestroyDescriptorPool(device, globalDescriptorPool, nullptr);
		// Sampled-texture aliases live in the global descriptor sets. Destroying
		// their pool first invalidates those descriptor references, after which the
		// shared policy samplers can be released safely before the device dies.
		for(auto& [key, cachedSampler] : textureSamplerCache)
		{
			(void)key;
			if(cachedSampler.handle != VK_NULL_HANDLE)
				vkDestroySampler(device, cachedSampler.handle, nullptr);
		}
		textureSamplerCache.clear();
		sampledTextureAliases.clear();
		textureViewGenerations.clear();
		vkDestroyDescriptorSetLayout(device, globalDescriptorSetLayout, nullptr);
		editorDescriptorPool = VK_NULL_HANDLE;
		descriptorPool = VK_NULL_HANDLE;
		globalDescriptorPool = VK_NULL_HANDLE;
		globalDescriptorSetLayout = VK_NULL_HANDLE;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (allocator) vmaDestroyAllocator(allocator);
	allocator = VK_NULL_HANDLE;

	DestroyWorkerPools();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) vkDestroyDevice(device, nullptr);
	device = VK_NULL_HANDLE;
	physicalDevice = VK_NULL_HANDLE;
	maximumStorageBufferRange = 0U;

	if (validationLayersEnabled && instance) {
		DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
	}
	debugMessenger = VK_NULL_HANDLE;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (instance) {
		vkDestroySurfaceKHR(instance, surface, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
	surface = VK_NULL_HANDLE;
	instance = VK_NULL_HANDLE;
	initialized = false;
}

// VulkanContext::Impl::CreateInstance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateInstance() {
	// Build the instance and verify that requested validation layers are present.
	// This request comes from editor/export render settings in every compile
	// mode. If the Vulkan SDK layer manifest is absent, keep booting without
	// validation so packaged games and non-SDK machines still run.
	validationLayersEnabled = validationLayersRequested;
	if (validationLayersEnabled && !CheckValidationLayerSupport()) {
		// Linux developer machines may not expose the SDK layer manifest unless VK_LAYER_PATH is set.
		TERRAENGINE_LOG(LogLevel::Warning, "validation layers requested, but not available; continuing without validation layers.");
		validationLayersEnabled = false;
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "TerraEngine";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "TerraEngine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_3;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;

	auto extensions = GetRequiredExtensions();
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
	VkValidationFeatureEnableEXT validationFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
	VkValidationFeaturesEXT validationFeatures{};
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (validationLayersEnabled) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();

		PopulateDebugMessengerCreateInfo(debugCreateInfo);
		// Synchronization validation catches cross-submit image/layout hazards that
		// core validation cannot diagnose before they become a driver fault.
		validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
		validationFeatures.pNext = &debugCreateInfo;
		validationFeatures.enabledValidationFeatureCount = 1u;
		validationFeatures.pEnabledValidationFeatures = &validationFeature;
		createInfo.pNext = &validationFeatures;
	} else {
		createInfo.enabledLayerCount = 0;
		createInfo.pNext = nullptr;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create instance!");
		return false;
	}

	volkLoadInstance(instance);
	return true;
}

// VulkanContext::Impl::PopulateDebugMessengerCreateInfo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
	// Configure debug messenger severity and message types before creation.
	createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = DebugCallback;
}

// VulkanContext::Impl::SetupDebugMessenger is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::SetupDebugMessenger() {
	// Install the debug messenger when validation layers are enabled.
	if (!validationLayersEnabled) return true;

	VkDebugUtilsMessengerCreateInfoEXT createInfo;
	PopulateDebugMessengerCreateInfo(createInfo);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to set up debug messenger!");
		return false;
	}
	return true;
}

// VulkanContext::Impl::PickPhysicalDevice is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::PickPhysicalDevice() {
	// Select the best suitable GPU using a scoring system.
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (deviceCount == 0) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to find GPUs with Vulkan support!");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	// Use an ordered map to automatically sort candidates by score.
	std::multimap<int, VkPhysicalDevice> candidates;

	for (const auto& device : devices) {
		int score = RateDeviceSuitability(device);
		candidates.insert(std::make_pair(score, device));
	}

	// Check if the best candidate is suitable at all.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (candidates.rbegin()->first > 0) {
		physicalDevice = candidates.rbegin()->second;
		
		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
		maximumStorageBufferRange = deviceProperties.limits.maxStorageBufferRange;

		VkPhysicalDeviceMeshShaderFeaturesEXT queriedMeshFeatures{};
		queriedMeshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

		VkPhysicalDeviceFeatures2 queriedFeatures{};
		queriedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		queriedFeatures.pNext = &queriedMeshFeatures;
		vkGetPhysicalDeviceFeatures2(physicalDevice, &queriedFeatures);
		samplerAnisotropySupported = queriedFeatures.features.samplerAnisotropy == VK_TRUE;
		maximumSamplerAnisotropy = samplerAnisotropySupported
			? std::max(1.0F, deviceProperties.limits.maxSamplerAnisotropy) : 1.0F;
		effectiveTerrainAnisotropy = samplerAnisotropySupported
			? std::clamp(requestedTerrainAnisotropy, 1.0F, maximumSamplerAnisotropy) : 1.0F;
		terrainAnisotropyFallbackActive = !samplerAnisotropySupported
			|| effectiveTerrainAnisotropy < requestedTerrainAnisotropy;
		meshShaderSupported = queriedMeshFeatures.meshShader == VK_TRUE;
		taskShaderSupported = queriedMeshFeatures.taskShader == VK_TRUE;
		
		uint32_t driverVersion = deviceProperties.driverVersion;
		uint32_t apiVersion = deviceProperties.apiVersion;
		selectedPhysicalDeviceName = deviceProperties.deviceName;
		selectedDriverVersionString = std::to_string(driverVersion);
		selectedApiVersionString = TerraEngine::Renderer::Vulkan::FormatVulkanVersion(apiVersion);
		
		TERRAENGINE_LOG(LogLevel::Debug, "Selected GPU: " << deviceProperties.deviceName);
		TERRAENGINE_LOG(LogLevel::Debug, "  Vendor ID: " << deviceProperties.vendorID);
		TERRAENGINE_LOG(LogLevel::Debug, "  Device Type: " << deviceProperties.deviceType);
		TERRAENGINE_LOG(LogLevel::Debug, "  Driver Version: " << driverVersion);
		TERRAENGINE_LOG(LogLevel::Debug, "  API Version: " << selectedApiVersionString);
		TERRAENGINE_LOG(LogLevel::Debug, "  Mesh Shader Support: " << (meshShaderSupported ? "yes" : "no"));
		TERRAENGINE_LOG(LogLevel::Debug, "  Task Shader Support: " << (taskShaderSupported ? "yes" : "no"));
		TERRAENGINE_LOG(LogLevel::Debug, "  Sampler Anisotropy: "
			<< (samplerAnisotropySupported ? "yes" : "no")
			<< " (max " << maximumSamplerAnisotropy << ")");
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "failed to find a suitable GPU!");
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!meshShaderSupported) {
		TERRAENGINE_LOG(LogLevel::Error, "Selected GPU does not expose VK_EXT_mesh_shader meshShader support.");
		return false;
	}

	return true;
}

// VulkanContext::Impl::RateDeviceSuitability is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
int VulkanContext::Impl::RateDeviceSuitability(VkPhysicalDevice device) {
	if (!IsDeviceSuitable(device)) {
		return 0;
	}

	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(device, &deviceProperties);

	VkPhysicalDeviceFeatures deviceFeatures;
	vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

	int score = 0;

	// Discrete GPUs have a significant performance advantage
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
		score += 1000;
	}

	// Maximum possible size of textures affects graphics quality
	score += deviceProperties.limits.maxImageDimension2D;

	return score;
}

// VulkanContext::Impl::CreateLogicalDevice is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateLogicalDevice() {
	// Build queues and logical device features needed for rendering.
	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};

	float queuePriority = 1.0f;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (uint32_t queueFamily : uniqueQueueFamilies) {
		VkDeviceQueueCreateInfo queueCreateInfo{};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures{};
	// An otherwise valid device remains usable when anisotropy is absent. Terrain
	// policy normalizes authored anisotropic intent to safe trilinear filtering.
	deviceFeatures.samplerAnisotropy = samplerAnisotropySupported ? VK_TRUE : VK_FALSE;
	// Conventional visibility has a blended target0 beside a typed identity
	// target, so the selected device must expose and enable independent blending.
	deviceFeatures.independentBlend = VK_TRUE;

	// Enable Descriptor Indexing features
	vulkan12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	vulkan12Features.descriptorIndexing = VK_TRUE;
	vulkan12Features.runtimeDescriptorArray = VK_TRUE;
	vulkan12Features.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	vulkan12Features.descriptorBindingPartiallyBound = VK_TRUE;
	vulkan12Features.descriptorBindingVariableDescriptorCount = VK_TRUE;
	vulkan12Features.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	vulkan12Features.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
	vulkan12Features.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
	vulkan12Features.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
	vulkan12Features.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
	vulkan12Features.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
	// Buffer device address is often useful with bindless but strictly optional
	vulkan12Features.bufferDeviceAddress = VK_TRUE;

	vulkan13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	vulkan13Features.shaderDemoteToHelperInvocation = VK_TRUE;

	// Mesh shaders are the required runtime geometry path for this feature.
	meshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
	meshShaderFeatures.meshShader = VK_TRUE;
	// Enable task shaders only when the selected GPU advertises the capability.
	meshShaderFeatures.taskShader = taskShaderSupported ? VK_TRUE : VK_FALSE;

	vrsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
	
	// Query actual support before enabling
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR supportedVrsFeatures{};
	supportedVrsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
	VkPhysicalDeviceFeatures2 features{};
	features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features.pNext = &supportedVrsFeatures;
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR supportedPresentationFenceFeatures{};
	supportedPresentationFenceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
	VkPhysicalDeviceFeatures2 presentationFeaturesQuery{};
	presentationFeaturesQuery.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	presentationFeaturesQuery.pNext = &supportedPresentationFenceFeatures;
	vkGetPhysicalDeviceFeatures2(physicalDevice, &presentationFeaturesQuery);

	vrsFeatures.attachmentFragmentShadingRate = supportedVrsFeatures.attachmentFragmentShadingRate;
	vrsFeatures.pipelineFragmentShadingRate = supportedVrsFeatures.pipelineFragmentShadingRate;
	vrsFeatures.primitiveFragmentShadingRate = supportedVrsFeatures.primitiveFragmentShadingRate;

	vulkan12Features.pNext = &vulkan13Features;
	vulkan13Features.pNext = &meshShaderFeatures;
	meshShaderFeatures.pNext = &vrsFeatures;
	vrsFeatures.pNext = nullptr;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.features = deviceFeatures;
	features2.pNext = &vulkan12Features;

	// Diagnostic checkpoints are optional and must not make NVIDIA-specific
	// functionality a portability requirement for AMD, Intel, or software GPUs.
	std::vector<VkExtensionProperties> availableDeviceExtensions;
	uint32_t availableDeviceExtensionCount = 0u;
	vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableDeviceExtensionCount, nullptr);
	availableDeviceExtensions.resize(availableDeviceExtensionCount);
	vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &availableDeviceExtensionCount, availableDeviceExtensions.data());
	std::vector<const char*> enabledDeviceExtensions = deviceExtensions;
	const auto hasDeviceExtension = [&availableDeviceExtensions](const char* name) {
		return std::any_of(availableDeviceExtensions.begin(), availableDeviceExtensions.end(),
			[name](const VkExtensionProperties& extension) {
				return std::strcmp(extension.extensionName, name) == 0;
			});
	};
	// Pair the device extension with the instance-side surface-maintenance lineage
	// selected in GetRequiredExtensions. Enabling only the device half would violate
	// the extension dependency even though the present-fence structs are aliases.
	const bool useKhrMaintenance = surfaceMaintenanceExtensionName != nullptr
		&& std::strcmp(surfaceMaintenanceExtensionName, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME) == 0;
	const bool useExtMaintenance = surfaceMaintenanceExtensionName != nullptr
		&& std::strcmp(surfaceMaintenanceExtensionName, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) == 0;
	const char* maintenanceExtensionName = useKhrMaintenance
		&& hasDeviceExtension(VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
		? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
		: (useExtMaintenance && hasDeviceExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)
			? VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
			: nullptr);
	presentationFencesEnabled = maintenanceExtensionName != nullptr
		&& supportedPresentationFenceFeatures.swapchainMaintenance1 == VK_TRUE;
	VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR presentationFenceFeatures{};
	if (presentationFencesEnabled)
	{
		presentationFenceFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
		presentationFenceFeatures.swapchainMaintenance1 = VK_TRUE;
		enabledDeviceExtensions.push_back(maintenanceExtensionName);
		// Append optional presentation completion proof after the required
		// feature chain without changing any existing feature ownership.
		vrsFeatures.pNext = &presentationFenceFeatures;
	}
	diagnosticCheckpointsEnabled = std::any_of(
		availableDeviceExtensions.begin(),
		availableDeviceExtensions.end(),
		[](const VkExtensionProperties& extension) {
			return std::strcmp(extension.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME) == 0;
		});
	if (diagnosticCheckpointsEnabled)
	{
		enabledDeviceExtensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
	}

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.pNext = &features2; // Chain features here

	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();

	createInfo.pEnabledFeatures = nullptr; // Must be null when using pNext for features

	createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	// Device layers are deprecated; instance-level validation layers cover device calls.
	createInfo.enabledLayerCount = 0;
	createInfo.ppEnabledLayerNames = nullptr;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create logical device!");
		return false;
	}

	volkLoadDevice(device);
	diagnosticCheckpointsEnabled = diagnosticCheckpointsEnabled
		&& vkCmdSetCheckpointNV != nullptr
		&& vkGetQueueCheckpointDataNV != nullptr;
	if (presentationFencesEnabled) {
		TERRAENGINE_LOG(LogLevel::Info, "[Vulkan] Presentation completion fences enabled via "
			<< maintenanceExtensionName << " with " << surfaceMaintenanceExtensionName);
	} else {
		TERRAENGINE_LOG(LogLevel::Warning, "[Vulkan] Swapchain maintenance present fences unavailable; resize retirement uses the bounded fail-closed fallback.");
	}

	vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
	return true;
}

// VulkanContext::Impl::CreateSwapChain is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateSwapChain(WindowSystem& windowSystem, VkSwapchainKHR oldSwapchain) {
	// Assemble swapchain matching the current surface capabilities.
	SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(physicalDevice);
	if (!swapChainSupport.valid || swapChainSupport.formats.empty() || swapChainSupport.presentModes.empty()) {
		TERRAENGINE_LOG(LogLevel::Error, "Swapchain support query did not return a complete usable surface snapshot.");
		return false;
	}

	VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
	VkExtent2D extent = ChooseSwapExtent(swapChainSupport.capabilities, windowSystem);
	if (extent.width == 0 || extent.height == 0) {
		// Minimized windows are a normal presentation state. Return to the frame
		// loop and retry after a later resize event instead of blocking in GLFW.
		presentationLifecycle.PublishRecreation(
			TerraEngine::Renderer::Vulkan::PresentationRecreationOutcome::DeferredZeroExtent,
			0u, 0u, 0u);
		return false;
	}

	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}
	surfaceMinimumImageCount = swapChainSupport.capabilities.minImageCount;
	requestedSwapchainImageCount = imageCount;

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = surface;

	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = surfaceFormat.format;
	createInfo.imageColorSpace = surfaceFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);
	uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (indices.graphicsFamily != indices.presentFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	} else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = oldSwapchain;

	VkSwapchainKHR candidateSwapchain = VK_NULL_HANDLE;
	// Vulkan treats supplying oldSwapchain as a handoff attempt. Do not restore
	// or acquire that chain after this call, even if candidate construction fails.
	oldSwapchainConsumedByCreateAttempt = oldSwapchain != VK_NULL_HANDLE;
	const VkResult createResult = vkCreateSwapchainKHR(device, &createInfo, nullptr, &candidateSwapchain);
	if (createResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create swap chain!");
		return false;
	}

	std::uint32_t actualImageCount = 0;
	VkResult enumerateResult = vkGetSwapchainImagesKHR(device, candidateSwapchain, &actualImageCount, nullptr);
	if (enumerateResult != VK_SUCCESS || actualImageCount < requestedSwapchainImageCount) {
		vkDestroySwapchainKHR(device, candidateSwapchain, nullptr);
		TERRAENGINE_LOG(LogLevel::Error, "Failed to enumerate swapchain image count: " << enumerateResult);
		return false;
	}
	std::vector<VkImage> candidateImages(actualImageCount);
	enumerateResult = vkGetSwapchainImagesKHR(device, candidateSwapchain, &actualImageCount, candidateImages.data());
	if ((enumerateResult != VK_SUCCESS && enumerateResult != VK_INCOMPLETE)
		|| actualImageCount < requestedSwapchainImageCount) {
		vkDestroySwapchainKHR(device, candidateSwapchain, nullptr);
		TERRAENGINE_LOG(LogLevel::Error, "Failed to enumerate swapchain images: " << enumerateResult);
		return false;
	}
	if (enumerateResult == VK_INCOMPLETE) {
		candidateImages.resize(actualImageCount);
		TERRAENGINE_LOG(LogLevel::Warning, "Swapchain image enumeration returned VK_INCOMPLETE; using the complete prefix and requesting recreation if acquisition exceeds it.");
	}

	// Publish the candidate only after both enumeration calls and ownership
	// validation succeeded. Callers can keep the prior bundle on any failure.
	swapchain = candidateSwapchain;
	swapchainImages = std::move(candidateImages);

	swapchainImageFormat = surfaceFormat.format;
	swapchainExtent = extent;
	return true;
}

// VulkanContext::Impl::CreateImageViews is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateImageViews() {
	// Build image views for every swapchain image.
	swapchainImageViews.resize(swapchainImages.size());

	for (size_t i = 0; i < swapchainImages.size(); i++) {
		swapchainImageViews[i] = CreateImageView(swapchainImages[i], swapchainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (swapchainImageViews[i] == VK_NULL_HANDLE) return false;
	}
	return true;
}

// VulkanContext::Impl::FindDepthFormat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkFormat VulkanContext::Impl::FindDepthFormat() {
	// Find a depth format compatible with the GPU for depth testing.
	return FindSupportedFormat(
		{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

// VulkanContext::Impl::FindSupportedFormat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkFormat VulkanContext::Impl::FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
	// Pick the first candidate that supports requested tiling and features.
	for (VkFormat format : candidates) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
			return format;
		} else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
			return format;
		}
	}
	throw std::runtime_error("failed to find supported format!");
}

// VulkanContext::Impl::CreateDepthResources is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateDepthResources() {
	// Allocate a depth attachment that matches the swapchain extent.
	VkFormat chosenDepthFormat = FindDepthFormat();
	this->depthFormat = chosenDepthFormat;

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent.width = swapchainExtent.width;
	imageInfo.extent.height = swapchainExtent.height;
	imageInfo.extent.depth = 1;
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.format = chosenDepthFormat;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &depthImage, &depthImageAllocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create depth image!");
		return false;
	}
	vmaSetAllocationName(allocator, depthImageAllocation, "Main Depth Image");

	depthImageView = CreateImageView(depthImage, chosenDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	TransitionImageLayout(depthImage, chosenDepthFormat, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
	return true;
}

// VulkanContext::Impl::UsesConventionalVisibilityAttachments determines whether
// a pass owns the paired color/identity attachment contract. Shadow and editor
// presentation passes deliberately remain target0-only.
bool VulkanContext::Impl::UsesConventionalVisibilityAttachments(VkRenderPass pass) const
{
	return pass != VK_NULL_HANDLE
		&& (pass == offscreenRenderPass || (!editorMode && pass == renderPass));
}

// VulkanContext::Impl::CreateConventionalVisibilityIdentityAttachment creates
// the integer attachment that matches Default.ConventionalVisibilityIdentity.
bool VulkanContext::Impl::CreateConventionalVisibilityIdentityAttachment(
	VkExtent2D extent, ConventionalVisibilityIdentityAttachment& attachment)
{
	if (extent.width == 0u || extent.height == 0u) {
		TERRAENGINE_LOG(LogLevel::Error,
			"Cannot create a conventional visibility identity attachment with a zero extent.");
		return false;
	}
	VkFormatProperties formatProperties{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice,
		VK_FORMAT_R32G32B32A32_UINT, &formatProperties);
	const VkFormatFeatureFlags requiredFeatures =
		VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
	if ((formatProperties.optimalTilingFeatures & requiredFeatures) != requiredFeatures) {
		// There is no alternate identity representation in the conventional ABI:
		// fail creation rather than silently pairing the identity shader with an
		// incompatible attachment format.
		TERRAENGINE_LOG(LogLevel::Error,
			"VK_FORMAT_R32G32B32A32_UINT does not support the required identity attachment features.");
		return false;
	}

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = { extent.width, extent.height, 1u };
	imageInfo.mipLevels = 1u;
	imageInfo.arrayLayers = 1u;
	imageInfo.format = VK_FORMAT_R32G32B32A32_UINT;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocationInfo{};
	allocationInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	if (vmaCreateImage(allocator, &imageInfo, &allocationInfo,
		&attachment.image, &attachment.allocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error,
			"Failed to create a conventional visibility identity image.");
		attachment = {};
		return false;
	}
	vmaSetAllocationName(allocator, attachment.allocation,
		"Conventional Visibility Identity Attachment");
	attachment.view = CreateImageView(attachment.image,
		VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_ASPECT_COLOR_BIT);
	if (attachment.view == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error,
			"Failed to create a conventional visibility identity image view.");
		DestroyConventionalVisibilityIdentityAttachment(attachment);
		return false;
	}
	return true;
}

// VulkanContext::Impl::DestroyConventionalVisibilityIdentityAttachment releases
// the view before its VMA-owned image, preserving Vulkan object lifetime rules.
void VulkanContext::Impl::DestroyConventionalVisibilityIdentityAttachment(
	ConventionalVisibilityIdentityAttachment& attachment)
{
	if (device != VK_NULL_HANDLE && attachment.view != VK_NULL_HANDLE) {
		vkDestroyImageView(device, attachment.view, nullptr);
	}
	if (attachment.image != VK_NULL_HANDLE && attachment.allocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, attachment.image, attachment.allocation);
	}
	attachment = {};
}

// VulkanContext::Impl::CreateRenderPass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateRenderPass() {
	if (editorMode) {
		// The swapchain pass only presents ImGui. Scene View and Game View render
		// into separate offscreen targets with their own depth images, so attaching
		// the runtime depth image here adds no visual result. More importantly, one
		// shared depth attachment across several swapchain images permits UI frames
		// in flight to clear/write the same image during resize churn. Keep editor
		// presentation color-only and remove that cross-frame GPU hazard entirely.
		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = swapchainImageFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = 0u;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1u;
		subpass.pColorAttachments = &colorAttachmentRef;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0u;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = 1u;
		renderPassInfo.pAttachments = &colorAttachment;
		renderPassInfo.subpassCount = 1u;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1u;
		renderPassInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create editor presentation render pass!");
			return false;
		}
		return true;
	}

	if (UsesVrsAttachment()) {
		// Use RenderPass2 to support VRS attachment
		VkAttachmentDescription2 colorAttachment{};
		colorAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		colorAttachment.format = swapchainImageFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
		depthAttachment.format = depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		// CreateDepthResources initializes this shared image once. Preserve its
		// attachment layout so each frame does not race the prior frame through an
		// implicit UNDEFINED discard transition.
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

		VkSubpassDependency2 dependency{};
		dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription2, 4> attachments = {
			colorAttachment, identityAttachment, depthAttachment, vrsAttachment};
		VkRenderPassCreateInfo2 renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateRenderPass2(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create render pass 2!");
			return false;
		}
	} else {
		// Fallback to standard RenderPass
		VkAttachmentDescription colorAttachment{};
		colorAttachment.format = swapchainImageFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
		depthAttachment.format = depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		// The swapchain framebuffers share one depth image. Its cross-frame barrier
		// expects the render pass to enter and leave attachment layout unchanged.
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

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription, 3> attachments = {
			colorAttachment, identityAttachment, depthAttachment};
		VkRenderPassCreateInfo renderPassInfo{};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassInfo.pAttachments = attachments.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create render pass!");
			return false;
		}
	}
	return true;
}

// VulkanContext::Impl::CreateFramebuffers is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateFramebuffers() {
	// Editor presentation is color-only. Runtime framebuffers retain depth and
	// optional VRS plus one image-owned integer identity target. Keeping identity
	// images per swapchain image avoids cross-frame writes to a shared attachment.
	framebuffers.resize(swapchainImageViews.size());
	presentationVisibilityIdentities.clear();
	if (!editorMode) {
		presentationVisibilityIdentities.resize(swapchainImageViews.size());
	}

	for (size_t i = 0; i < swapchainImageViews.size(); i++) {
		std::vector<VkImageView> attachments = {
			swapchainImageViews[i]
		};
		if (!editorMode) {
			attachments.push_back(depthImageView);
			if (!CreateConventionalVisibilityIdentityAttachment(
				swapchainExtent, presentationVisibilityIdentities[i])) {
				TERRAENGINE_LOG(LogLevel::Error,
					"Failed to create the runtime conventional visibility identity attachment.");
				return false;
			}
			// The render-pass attachment order is color, identity, depth, then VRS.
			attachments.insert(attachments.begin() + 1,
				presentationVisibilityIdentities[i].view);
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (UsesVrsAttachment()) {
			attachments.push_back(vrs.view);
		}

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = renderPass;
		framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		framebufferInfo.pAttachments = attachments.data();
		framebufferInfo.width = swapchainExtent.width;
		framebufferInfo.height = swapchainExtent.height;
		framebufferInfo.layers = 1;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create framebuffer!");
			return false;
		}
	}
	return true;
}

// VulkanContext::Impl::CreateCommandPool is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateCommandPool() {
	// Create command pools that separate per-frame rendering from immediate
	// uploads. Texture imports and layout transitions can happen while the
	// editor has recorded Scene/Game/UI command buffers, so single-time upload
	// commands must not share the frame pools that BeginFrame resets.
	QueueFamilyIndices queueFamilyIndices = FindQueueFamilies(physicalDevice);

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

	VkCommandPoolCreateInfo immediatePoolInfo = poolInfo;
	immediatePoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	if (vkCreateCommandPool(device, &immediatePoolInfo, nullptr, &immediateCommandPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create immediate upload command pool!");
		return false;
	}

	commandPools.resize(MAX_FRAMES_IN_FLIGHT);
	commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	editorCommandPools.resize(MAX_FRAMES_IN_FLIGHT);
	editorCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPools[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create command pool!");
			return false;
		}
		// UI command buffers must be reset even on project-hub frames where no
		// render camera exists and the scene command pool never reaches BeginFrame.
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &editorCommandPools[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create editor command pool!");
			return false;
		}
		VkCommandBufferAllocateInfo editorAllocInfo{};
		editorAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		editorAllocInfo.commandPool = editorCommandPools[i];
		editorAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		editorAllocInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &editorAllocInfo, &editorCommandBuffers[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate editor command buffer!");
			return false;
		}

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = commandPools[i];
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffers[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffers!");
			return false;
		}
	}
	return true;
}

// VulkanContext::Impl::CreateSyncObjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateSyncObjects() {
	// Frame-owned synchronization is independent from swapchain image ownership.
	presentationCompletionCoverageValid = presentationFencesEnabled;
	imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
	uiSemaphores.resize(swapchainImages.size());
	if (presentationFencesEnabled) availablePresentationCompletionFences.resize(swapchainImages.size());
	InitializeFrameTracking();

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	for (std::size_t index = 0; index < MAX_FRAMES_IN_FLIGHT; ++index) {
		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[index]) != VK_SUCCESS
			|| vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[index]) != VK_SUCCESS
			|| vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[index]) != VK_SUCCESS) {
			DestroyFrameSyncObjects();
			DestroyPresentationImageSyncObjects();
			TERRAENGINE_LOG(LogLevel::Error, "failed to create frame-owned synchronization objects!");
			return false;
		}
	}
	for (std::size_t imageIndex = 0; imageIndex < uiSemaphores.size(); ++imageIndex) {
		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &uiSemaphores[imageIndex]) != VK_SUCCESS) {
			DestroyFrameSyncObjects();
			DestroyPresentationImageSyncObjects();
			TERRAENGINE_LOG(LogLevel::Error, "failed to create image-owned present synchronization objects!");
			return false;
		}
		if (presentationFencesEnabled
			&& vkCreateFence(device, &fenceInfo, nullptr, &availablePresentationCompletionFences[imageIndex]) != VK_SUCCESS) {
			DestroyFrameSyncObjects();
			DestroyPresentationImageSyncObjects();
			TERRAENGINE_LOG(LogLevel::Error, "failed to create image-owned presentation completion fences!");
			return false;
		}
	}
	return true;
}

void VulkanContext::Impl::InitializeFrameTracking() {
	// Startup tracking owns command-buffer and deferred-retirement containers.
	// Swapchain recreation must not clear them before their fences complete.
	if (extraPrimaryCommandBuffers.empty()) extraPrimaryCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	if (pendingPrimaryCommandBuffers.empty()) pendingPrimaryCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	if (pendingPrimaryCommandBuffersSubmitted.empty()) pendingPrimaryCommandBuffersSubmitted.resize(MAX_FRAMES_IN_FLIGHT, 0u);
	if (retiredBuffersByFrame.empty()) retiredBuffersByFrame.resize(MAX_FRAMES_IN_FLIGHT);
	editorCompletionTracker.ResetFrameCount(MAX_FRAMES_IN_FLIGHT);
}

void VulkanContext::Impl::DestroyFrameSyncObjects() {
	if (device != VK_NULL_HANDLE) {
		for (VkSemaphore semaphore : imageAvailableSemaphores) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkSemaphore semaphore : renderFinishedSemaphores) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkFence fence : inFlightFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
	}
	imageAvailableSemaphores.clear();
	renderFinishedSemaphores.clear();
	inFlightFences.clear();
}

void VulkanContext::Impl::DestroyPresentationImageSyncObjects() {
	if (device != VK_NULL_HANDLE) {
		for (VkSemaphore semaphore : uiSemaphores) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkFence fence : presentationCompletionFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		for (VkFence fence : availablePresentationCompletionFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
	}
	uiSemaphores.clear();
	presentationCompletionFences.clear();
	availablePresentationCompletionFences.clear();
	acquiredPresentationImage.reset();
}

VulkanContext::Impl::PresentationResourceBundle VulkanContext::Impl::TakePresentationResourceBundle() {
	PresentationResourceBundle bundle;
	bundle.swapchain = std::exchange(swapchain, VK_NULL_HANDLE);
	bundle.images = std::move(swapchainImages);
	bundle.imageFormat = std::exchange(swapchainImageFormat, VK_FORMAT_UNDEFINED);
	bundle.extent = std::exchange(swapchainExtent, VkExtent2D{});
	bundle.imageViews = std::move(swapchainImageViews);
	bundle.renderPass = std::exchange(renderPass, VK_NULL_HANDLE);
	bundle.framebuffers = std::move(framebuffers);
	bundle.depthImage = std::exchange(depthImage, VK_NULL_HANDLE);
	bundle.depthAllocation = std::exchange(depthImageAllocation, VK_NULL_HANDLE);
	bundle.depthView = std::exchange(depthImageView, VK_NULL_HANDLE);
	bundle.visibilityIdentities = std::move(presentationVisibilityIdentities);
	bundle.acquireByFrame = std::move(imageAvailableSemaphores);
	bundle.editorOffscreenReadyByFrame = std::move(renderFinishedSemaphores);
	bundle.presentReadyByImage = std::move(uiSemaphores);
	bundle.presentCompletionFences = std::move(presentationCompletionFences);
	bundle.availablePresentCompletionFences = std::move(availablePresentationCompletionFences);
	bundle.presentCompletionCoverageValid = std::exchange(presentationCompletionCoverageValid, false);
	bundle.frameFences = std::move(inFlightFences);
	// A moved-from vector is valid but its size is unspecified. Clear every raw-
	// handle collection explicitly so candidate creation cannot retain duplicate handles.
	imageAvailableSemaphores.clear();
	renderFinishedSemaphores.clear();
	uiSemaphores.clear();
	presentationCompletionFences.clear();
	availablePresentationCompletionFences.clear();
	inFlightFences.clear();
	return bundle;
}

void VulkanContext::Impl::RestorePresentationResourceBundle(PresentationResourceBundle&& bundle) {
	swapchain = std::exchange(bundle.swapchain, VK_NULL_HANDLE);
	swapchainImages = std::move(bundle.images);
	swapchainImageFormat = std::exchange(bundle.imageFormat, VK_FORMAT_UNDEFINED);
	swapchainExtent = std::exchange(bundle.extent, VkExtent2D{});
	swapchainImageViews = std::move(bundle.imageViews);
	renderPass = std::exchange(bundle.renderPass, VK_NULL_HANDLE);
	framebuffers = std::move(bundle.framebuffers);
	depthImage = std::exchange(bundle.depthImage, VK_NULL_HANDLE);
	depthImageAllocation = std::exchange(bundle.depthAllocation, VK_NULL_HANDLE);
	depthImageView = std::exchange(bundle.depthView, VK_NULL_HANDLE);
	presentationVisibilityIdentities = std::move(bundle.visibilityIdentities);
	imageAvailableSemaphores = std::move(bundle.acquireByFrame);
	renderFinishedSemaphores = std::move(bundle.editorOffscreenReadyByFrame);
	uiSemaphores = std::move(bundle.presentReadyByImage);
	presentationCompletionFences = std::move(bundle.presentCompletionFences);
	availablePresentationCompletionFences = std::move(bundle.availablePresentCompletionFences);
	presentationCompletionCoverageValid = std::exchange(bundle.presentCompletionCoverageValid, false);
	inFlightFences = std::move(bundle.frameFences);
}

void VulkanContext::Impl::DestroyPresentationResourceBundle(PresentationResourceBundle& bundle) {
	if (device != VK_NULL_HANDLE) {
		for (VkFramebuffer framebuffer : bundle.framebuffers) if (framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (bundle.renderPass != VK_NULL_HANDLE) vkDestroyRenderPass(device, bundle.renderPass, nullptr);
		for (VkImageView view : bundle.imageViews) if (view != VK_NULL_HANDLE) vkDestroyImageView(device, view, nullptr);
		if (bundle.depthView != VK_NULL_HANDLE) vkDestroyImageView(device, bundle.depthView, nullptr);
		for (VkSemaphore semaphore : bundle.acquireByFrame) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkSemaphore semaphore : bundle.editorOffscreenReadyByFrame) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkSemaphore semaphore : bundle.presentReadyByImage) if (semaphore != VK_NULL_HANDLE) vkDestroySemaphore(device, semaphore, nullptr);
		for (VkFence fence : bundle.presentCompletionFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		for (VkFence fence : bundle.availablePresentCompletionFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		for (VkFence fence : bundle.frameFences) if (fence != VK_NULL_HANDLE) vkDestroyFence(device, fence, nullptr);
		if (bundle.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device, bundle.swapchain, nullptr);
	}
	if (bundle.depthImage != VK_NULL_HANDLE && bundle.depthAllocation != VK_NULL_HANDLE) {
		vmaDestroyImage(allocator, bundle.depthImage, bundle.depthAllocation);
	}
	for (ConventionalVisibilityIdentityAttachment& identity : bundle.visibilityIdentities) {
		DestroyConventionalVisibilityIdentityAttachment(identity);
	}
	bundle = {};
}

void VulkanContext::Impl::RetirePresentationResourceBundle(
	PresentationResourceBundle&& bundle, std::uint64_t generation)
{
	if (bundle.swapchain == VK_NULL_HANDLE)
	{
		return;
	}

	// Image-owned present semaphores can outlive a render fence. Keep the full
	// bundle together until a per-present fence proves every queued present has
	// completed; without the optional extension retain a bounded backlog rather
	// than guessing from vkDeviceWaitIdle.
	retiredPresentationBundles.push_back({ std::move(bundle), generation });
}

void VulkanContext::Impl::CollectCompletedPresentationResourceBundles()
{
	if (device == VK_NULL_HANDLE)
	{
		return;
	}

	for (auto it = retiredPresentationBundles.begin(); it != retiredPresentationBundles.end();)
	{
		// A bundle is eligible for retirement only when every present in its
		// generation carried a completion fence. Fence presence alone is insufficient:
		// a later preparation failure may have allowed an unfenced present.
		bool completed = it->resources.presentCompletionCoverageValid;
		for (VkFence fence : it->resources.presentCompletionFences)
		{
			if (fence == VK_NULL_HANDLE || vkGetFenceStatus(device, fence) != VK_SUCCESS)
			{
				completed = false;
				break;
			}
		}
		if (!completed)
		{
			++it;
			continue;
		}

		// The fence proof satisfies the lifecycle rule before the image-owned
		// present waits, views, and old swapchain are destroyed.
		DestroyPresentationResourceBundle(it->resources);
		it = retiredPresentationBundles.erase(it);
	}
}

bool VulkanContext::Impl::PreparePresentationCompletionFence(
	std::uint32_t imageIndex, VkPresentInfoKHR& presentInfo,
	VkSwapchainPresentFenceInfoKHR& completionInfo)
{
	if (!presentationFencesEnabled)
	{
		presentationCompletionCoverageValid = false;
		return false;
	}
	if (imageIndex >= uiSemaphores.size())
	{
		presentationCompletionCoverageValid = false;
		return false;
	}

	// Poll every outstanding present without waiting on the CPU. Only signaled
	// fences enter the reusable pool; unsignaled fences remain immutable until the
	// presentation engine releases their associated wait semaphores.
	for (auto it = presentationCompletionFences.begin(); it != presentationCompletionFences.end();) {
		const VkResult status = vkGetFenceStatus(device, *it);
		if (status == VK_SUCCESS) {
			availablePresentationCompletionFences.push_back(*it);
			it = presentationCompletionFences.erase(it);
		} else if (status == VK_NOT_READY) {
			++it;
		} else {
			presentationCompletionCoverageValid = false;
			return false;
		}
	}

	VkFence completionFence = VK_NULL_HANDLE;
	if (!availablePresentationCompletionFences.empty()) {
		completionFence = availablePresentationCompletionFences.back();
		availablePresentationCompletionFences.pop_back();
		if (vkResetFences(device, 1u, &completionFence) != VK_SUCCESS) {
			availablePresentationCompletionFences.push_back(completionFence);
			presentationCompletionCoverageValid = false;
			return false;
		}
	} else {
		// There can be no more outstanding presents than swapchain images: acquiring
		// an image proves its previous present completed, so its fence should already
		// have entered the pool above. Refuse an inconsistent state instead of growing
		// an unbounded fence history while a compositor or driver is stalled.
		presentationCompletionCoverageValid = false;
		TERRAENGINE_LOG(LogLevel::Warning, "No reusable presentation completion fence was available after swapchain image acquisition; retirement will fail closed.");
		return false;
	}
	presentationCompletionFences.push_back(completionFence);

	completionInfo = {};
	completionInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR;
	completionInfo.swapchainCount = 1u;
	completionInfo.pFences = &presentationCompletionFences.back();
	completionInfo.pNext = presentInfo.pNext;
	presentInfo.pNext = &completionInfo;
	return true;
}

void VulkanContext::Impl::ResolveUnenqueuedPresentationFence(
	bool fencePrepared, VkResult presentResult)
{
	if (!fencePrepared
		|| (presentResult != VK_ERROR_OUT_OF_HOST_MEMORY
			&& presentResult != VK_ERROR_OUT_OF_DEVICE_MEMORY)
		|| presentationCompletionFences.empty())
	{
		return;
	}

	// Vulkan guarantees that these allocation failures enqueue no presentation
	// work and leave referenced synchronization untouched. Preparation reserved an
	// unsignaled per-present fence before the rejected call, so replace it with a signaled
	// reusable fence representing that no presentation ownership was created.
	VkFence completionFence = presentationCompletionFences.back();
	presentationCompletionFences.pop_back();
	if (completionFence != VK_NULL_HANDLE) {
		vkDestroyFence(device, completionFence, nullptr);
		completionFence = VK_NULL_HANDLE;
	}
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	if (vkCreateFence(device, &fenceInfo, nullptr, &completionFence) != VK_SUCCESS) {
		presentationFencesEnabled = false;
		TERRAENGINE_LOG(LogLevel::Warning, "Could not restore a signaled presentation fence after an unenqueued present allocation failure.");
	} else {
		availablePresentationCompletionFences.push_back(completionFence);
	}
}

VkFence VulkanContext::Impl::PrepareUnextendedAcquireProof()
{
	CollectCompletedUnextendedAcquireProofs();
	if (device == VK_NULL_HANDLE || deviceLost || !unextendedRetirementTracker.NeedsAcquireProof())
	{
		return VK_NULL_HANDLE;
	}

	// Successful acquire fences normally signal immediately or within one frame.
	// Bound the history so a stalled driver cannot turn fallback evidence into an
	// unbounded host allocation; later frames keep polling the existing proofs.
	const std::size_t maximumProofs = std::max<std::size_t>(
		1u, swapchainImages.size() + static_cast<std::size_t>(MAX_FRAMES_IN_FLIGHT));
	if (pendingUnextendedAcquireProofs.size() >= maximumProofs)
	{
		return VK_NULL_HANDLE;
	}

	VkFence proofFence = VK_NULL_HANDLE;
	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (vkCreateFence(device, &fenceInfo, nullptr, &proofFence) != VK_SUCCESS)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Could not allocate an acquire-history proof fence; retired presentation resources remain retained.");
		return VK_NULL_HANDLE;
	}
	return proofFence;
}

void VulkanContext::Impl::ResolveUnextendedAcquireProof(
	VkFence proofFence, VkResult acquireResult, std::uint32_t imageIndex)
{
	if (proofFence == VK_NULL_HANDLE)
	{
		return;
	}
	if (acquireResult == VK_SUCCESS || acquireResult == VK_SUBOPTIMAL_KHR)
	{
		// Store identity only after acquisition succeeded. The fence is polled on
		// later frames, preserving the editor's bounded acquire wait behavior.
		pendingUnextendedAcquireProofs.push_back({
			proofFence, presentationLifecycle.Generation(), imageIndex });
		CollectCompletedUnextendedAcquireProofs();
		return;
	}

	// Failed or timed-out acquisition publishes no image and no pending fence
	// signal operation, so the one-shot proof object has no ownership to retain.
	vkDestroyFence(device, proofFence, nullptr);
}

void VulkanContext::Impl::CollectCompletedUnextendedAcquireProofs()
{
	if (device == VK_NULL_HANDLE)
	{
		return;
	}

	for (auto it = pendingUnextendedAcquireProofs.begin(); it != pendingUnextendedAcquireProofs.end();)
	{
		const VkResult status = vkGetFenceStatus(device, it->fence);
		if (status == VK_NOT_READY)
		{
			++it;
			continue;
		}
		if (status != VK_SUCCESS)
		{
			MarkDeviceLost(status, "unextended presentation acquire proof");
			return;
		}

		const bool releasesRetiredBundles = unextendedRetirementTracker.RecordCompletedAcquire(
			it->generation, it->imageIndex);
		vkDestroyFence(device, it->fence, nullptr);
		it = pendingUnextendedAcquireProofs.erase(it);
		if (!releasesRetiredBundles)
		{
			continue;
		}

		// Khronos' unextended workaround proves the first present of the newest
		// chain completed. Every older present on the same queue, and therefore all
		// retired swapchains and their wait semaphores, can now be released together.
		for (RetiredPresentationResourceBundle& retired : retiredPresentationBundles) {
			DestroyPresentationResourceBundle(retired.resources);
		}
		retiredPresentationBundles.clear();
		if (presentationRecreation.RequestRetryAfterRetirement()) {
			framebufferResized = true;
		}
	}
}

void VulkanContext::Impl::RecordUnextendedPresent(
	std::uint32_t imageIndex, VkResult presentResult)
{
	// Allocation failures are the two results Vulkan explicitly defines as not
	// enqueued. Other results conservatively retain a present-history anchor.
	if (presentResult == VK_ERROR_OUT_OF_HOST_MEMORY
		|| presentResult == VK_ERROR_OUT_OF_DEVICE_MEMORY)
	{
		return;
	}
	unextendedRetirementTracker.RecordPresent(
		presentationLifecycle.Generation(), imageIndex);
}

// VulkanContext::Impl::CreateDescriptorPool is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateDescriptorPool() {
	// Pool sized with a mix of common descriptor types so different shaders can allocate safely.
	std::array<VkDescriptorPoolSize, 7> poolSizes{};
	poolSizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	poolSizes[0].descriptorCount = 256;
	poolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	poolSizes[1].descriptorCount = 256;
	poolSizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	poolSizes[2].descriptorCount = 256;
	poolSizes[3].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	poolSizes[3].descriptorCount = 256;
	poolSizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	// Animation owns 15 storage-buffer bindings for every frame in flight (45
	// persistent descriptors), while every resident mesh also consumes several
	// storage descriptors. Keep one shared pool, but size it for both persistent
	// consumers so ordinary asset counts cannot starve animation publication.
	poolSizes[4].descriptorCount = 4096;
	poolSizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	poolSizes[5].descriptorCount = 64;
	poolSizes[6].type = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
	poolSizes[6].descriptorCount = 64;

	VkDescriptorPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT | VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
	poolInfo.pPoolSizes = poolSizes.data();
	poolInfo.maxSets = 512;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create descriptor pool!");
		return false;
	}

	// Keep short-lived editor descriptors out of the engine pool so asset imports
	// cannot race or starve UI allocation. Current Dear ImGui Vulkan backends use
	// both combined descriptors and separate sampled-image/sampler bindings, so
	// the pool must declare capacity for all three descriptor types it may request.
	std::array<VkDescriptorPoolSize, 3> editorPoolSizes{};
	editorPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	editorPoolSizes[0].descriptorCount = 64u;
	editorPoolSizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
	editorPoolSizes[1].descriptorCount = 64u;
	editorPoolSizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
	editorPoolSizes[2].descriptorCount = 64u;

	VkDescriptorPoolCreateInfo editorPoolInfo{};
	editorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	editorPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	editorPoolInfo.poolSizeCount = static_cast<std::uint32_t>(editorPoolSizes.size());
	editorPoolInfo.pPoolSizes = editorPoolSizes.data();
	editorPoolInfo.maxSets = 64u;
	if (vkCreateDescriptorPool(device, &editorPoolInfo, nullptr, &editorDescriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create editor descriptor pool!");
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		descriptorPool = VK_NULL_HANDLE;
		return false;
	}
	return true;
}

// VulkanContext::Impl::CheckValidationLayerSupport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CheckValidationLayerSupport() {
	// Ensure requested validation layers are available before enabling them.
	uint32_t layerCount;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

	std::vector<VkLayerProperties> availableLayers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const char* layerName : validationLayers) {
		bool layerFound = false;
		for (const auto& layerProperties : availableLayers) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (strcmp(layerName, layerProperties.layerName) == 0) {
				layerFound = true;
				break;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!layerFound) {
			return false;
		}
	}

	return true;
}

// VulkanContext::Impl::GetRequiredExtensions is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<const char*> VulkanContext::Impl::GetRequiredExtensions() {
	// Gather extensions required by GLFW and validation layers.
	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
	surfaceMaintenanceExtensionName = nullptr;
	const auto appendUniqueExtension = [&extensions](const char* name) {
		const bool alreadyPresent = std::any_of(extensions.begin(), extensions.end(),
			[name](const char* extension) { return std::strcmp(extension, name) == 0; });
		if (!alreadyPresent) extensions.push_back(name);
	};

	// Swapchain present fences depend on surface-maintenance1 and
	// get-surface-capabilities2 at instance scope. Prefer the original EXT lineage
	// because older validation layers understand it even when a newer driver also
	// advertises the promoted KHR alias. The later device selection must match it.
	std::uint32_t availableExtensionCount = 0u;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr) == VK_SUCCESS) {
		std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
		if (vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.data()) == VK_SUCCESS) {
			const auto hasInstanceExtension = [&availableExtensions](const char* name) {
				return std::any_of(availableExtensions.begin(), availableExtensions.end(),
					[name](const VkExtensionProperties& extension) {
						return std::strcmp(extension.extensionName, name) == 0;
					});
			};
			if (hasInstanceExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)) {
				const auto maintenanceLineage = TerraEngine::Renderer::Vulkan::SelectMaintenanceExtensionLineage(
					hasInstanceExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME),
					hasInstanceExtension(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME));
				if (maintenanceLineage == TerraEngine::Renderer::Vulkan::MaintenanceExtensionLineage::Ext) {
					surfaceMaintenanceExtensionName = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
				} else if (maintenanceLineage == TerraEngine::Renderer::Vulkan::MaintenanceExtensionLineage::Khr) {
					surfaceMaintenanceExtensionName = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
				}
				if (surfaceMaintenanceExtensionName != nullptr) {
					appendUniqueExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
					appendUniqueExtension(surfaceMaintenanceExtensionName);
				}
			}
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (validationLayersEnabled) {
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	return extensions;
}

// VulkanContext::Impl::IsDeviceSuitable is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::IsDeviceSuitable(VkPhysicalDevice device) {
	// Check for queues, extensions, and swapchain support.
	QueueFamilyIndices indices = FindQueueFamilies(device);
	bool extensionsSupported = CheckDeviceExtensionSupport(device);
	bool swapChainAdequate = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (extensionsSupported) {
		SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
		swapChainAdequate = swapChainSupport.valid
			&& !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!indices.isComplete() || !extensionsSupported || !swapChainAdequate) {
		return false;
	}

	// Check for Descriptor Indexing support (Vulkan 1.2)
	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	VkPhysicalDeviceVulkan13Features features13{};
	features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

	VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures{};
	meshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

	features12.pNext = &features13;
	features13.pNext = &meshFeatures;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features12;

	vkGetPhysicalDeviceFeatures2(device, &features2);

	// The conventional visibility pass intentionally gives target0-only content
	// a masked, non-blended identity slot. Vulkan requires independentBlend when
	// color attachment blend states differ, so reject unsupported devices early.
	if (features2.features.independentBlend != VK_TRUE) {
		VkPhysicalDeviceProperties deviceProperties{};
		vkGetPhysicalDeviceProperties(device, &deviceProperties);
		TERRAENGINE_LOG(LogLevel::Warning, "Rejecting Vulkan device '"
			<< deviceProperties.deviceName
			<< "': independentBlend is required for the conventional two-target visibility pass.");
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!features12.descriptorIndexing || !features12.runtimeDescriptorArray || 
		!features12.shaderSampledImageArrayNonUniformIndexing || 
		!features12.descriptorBindingPartiallyBound || 
		!features12.descriptorBindingVariableDescriptorCount ||
		!features13.shaderDemoteToHelperInvocation ||
		!meshFeatures.meshShader) {
		return false;
	}

	return true;
}

// VulkanContext::Impl::CheckDeviceExtensionSupport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CheckDeviceExtensionSupport(VkPhysicalDevice device) {
	// Verify the device supports the extensions we require (swapchain).
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

	std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& extension : availableExtensions) {
		requiredExtensions.erase(extension.extensionName);
	}

	return requiredExtensions.empty();
}

// VulkanContext::Impl::FindQueueFamilies is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
QueueFamilyIndices VulkanContext::Impl::FindQueueFamilies(VkPhysicalDevice device) {
	// Locate queue families capable of graphics and present operations.
	QueueFamilyIndices indices;
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	int i = 0;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& queueFamily : queueFamilies) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}
		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (presentSupport) {
			indices.presentFamily = i;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (indices.isComplete()) break;
		i++;
	}
	return indices;
}

// VulkanContext::Impl::QuerySwapChainSupport is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
SwapChainSupportDetails VulkanContext::Impl::QuerySwapChainSupport(VkPhysicalDevice device) {
	// Query swapchain capabilities for the given physical device.
	SwapChainSupportDetails details;
	if (device == VK_NULL_HANDLE || surface == VK_NULL_HANDLE) {
		return details;
	}
	const VkResult capabilitiesResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
	if (capabilitiesResult != VK_SUCCESS) {
		return details;
	}

	bool formatsComplete = false;
	for (std::uint32_t attempt = 0u; attempt < 2u && !formatsComplete; ++attempt) {
		// VK_INCOMPLETE reports how many entries were written, not necessarily the
		// new required capacity. Query the count again before the one bounded retry.
		std::uint32_t formatCount = 0u;
		const VkResult countResult = vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
		if (countResult != VK_SUCCESS || formatCount == 0u) return details;
		details.formats.resize(formatCount);
		const VkResult fillResult = vkGetPhysicalDeviceSurfaceFormatsKHR(
			device, surface, &formatCount, details.formats.data());
		if (fillResult == VK_SUCCESS && formatCount > 0u) {
			details.formats.resize(formatCount);
			formatsComplete = true;
		} else if (fillResult != VK_INCOMPLETE) {
			details.formats.clear();
			return details;
		}
	}
	if (!formatsComplete) {
		details.formats.clear();
		return details;
	}

	bool presentModesComplete = false;
	for (std::uint32_t attempt = 0u; attempt < 2u && !presentModesComplete; ++attempt) {
		std::uint32_t presentModeCount = 0u;
		const VkResult countResult = vkGetPhysicalDeviceSurfacePresentModesKHR(
			device, surface, &presentModeCount, nullptr);
		if (countResult != VK_SUCCESS || presentModeCount == 0u) return details;
		details.presentModes.resize(presentModeCount);
		const VkResult fillResult = vkGetPhysicalDeviceSurfacePresentModesKHR(
			device, surface, &presentModeCount, details.presentModes.data());
		if (fillResult == VK_SUCCESS && presentModeCount > 0u) {
			details.presentModes.resize(presentModeCount);
			presentModesComplete = true;
		} else if (fillResult != VK_INCOMPLETE) {
			details.presentModes.clear();
			return details;
		}
	}
	if (!presentModesComplete) {
		details.presentModes.clear();
		return details;
	}
	details.valid = true;
	return details;
}

// VulkanContext::Impl::ChooseSwapSurfaceFormat is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkSurfaceFormatKHR VulkanContext::Impl::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
	// Prefer SRGB if available, otherwise pick the first format.
	for (const auto& availableFormat : availableFormats) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}
	return availableFormats[0];
}

// VulkanContext::Impl::ChooseSwapPresentMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkPresentModeKHR VulkanContext::Impl::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
	// Use mailbox for low latency when possible, otherwise fallback to FIFO.
	for (const auto& availablePresentMode : availablePresentModes) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return availablePresentMode;
		}
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

// VulkanContext::Impl::ChooseSwapExtent is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkExtent2D VulkanContext::Impl::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, WindowSystem& windowSystem) {
	// Match the window framebuffer size to keep the swapchain in sync with the viewport.
	if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
		return capabilities.currentExtent;
	} else {
		int width, height;
		auto* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
		glfwGetFramebufferSize(window, &width, &height);

		VkExtent2D actualExtent = {
			static_cast<uint32_t>(width),
			static_cast<uint32_t>(height)
		};

		actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
		actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

		return actualExtent;
	}
}

// VulkanContext::Impl::CleanupSwapChain is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::CleanupSwapChain() {
	// Destroy swapchain-related resources while keeping device-level objects intact.
	if (device) {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto imageView : swapchainImageViews) vkDestroyImageView(device, imageView, nullptr);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
	}
	for (ConventionalVisibilityIdentityAttachment& identity : presentationVisibilityIdentities) {
		DestroyConventionalVisibilityIdentityAttachment(identity);
	}
	presentationVisibilityIdentities.clear();
	framebuffers.clear();
	swapchainImageViews.clear();
	swapchainImages.clear();
	renderPass = VK_NULL_HANDLE;
	swapchain = VK_NULL_HANDLE;
}

// VulkanContext::Impl::RecreateSwapChain is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecreateSwapChain() {
	std::lock_guard<std::recursive_mutex> stateLock(deviceStateMutex);
	if (deviceLost) {
		presentationLifecycle.MarkDeviceLost();
		return;
	}

	int width = 0;
	int height = 0;
	auto* window = static_cast<GLFWwindow*>(windowSystem->GetNativeHandle());
	glfwGetFramebufferSize(window, &width, &height);
	if (width <= 0 || height <= 0) {
		RecordFramebufferResize(width, height);
		presentationLifecycle.PublishRecreation(
			TerraEngine::Renderer::Vulkan::PresentationRecreationOutcome::DeferredZeroExtent,
			surfaceMinimumImageCount, requestedSwapchainImageCount,
			static_cast<std::uint32_t>(swapchainImages.size()));
		return;
	}
	CollectCompletedPresentationResourceBundles();
	if (presentationRecreation.OldSwapchainWasConsumed() && !retiredPresentationBundles.empty()) {
		// Do not create a VK_NULL_HANDLE replacement beside the chain that was
		// already supplied as oldSwapchain. Some window systems reject that as a
		// competing native-window chain; wait for explicit present completion.
		framebufferResized = true;
		return;
	}
	if (retiredPresentationBundles.size() >= MAX_RETIRED_PRESENTATION_BUNDLES) {
		// Do not make an unbounded chain of old presentation waits when optional
		// present-completion fences are unavailable or the compositor is stalled.
		// The active chain remains intact; a later revision retries after proof.
		presentationRecreation.PublishRetirementBlocked(false);
		// Keep presenting the active chain so its acquire-history anchor can
		// eventually free a backlog slot. The proof path re-arms this request.
		framebufferResized = false;
		TERRAENGINE_LOG(LogLevel::Warning, "Deferring swapchain recreation because the bounded retired-presentation backlog is still awaiting completion proof.");
		return;
	}

	presentationLifecycle.RequestRecreation();
	const VkResult idleResult = WaitDeviceIdle();
	if (MarkDeviceLost(idleResult, "swapchain recreation device idle")) {
		presentationLifecycle.MarkDeviceLost();
		return;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping swapchain recreation because the device did not become idle: " << idleResult);
		return;
	}
	editorCompletionTracker.RecordProtectedIdle();

	// Keep the complete old presentation bundle alive while the candidate is
	// built. Once it is supplied to vkCreateSwapchainKHR, however, that attempt
	// becomes irreversible and the old chain may not be restored or acquired.
	PresentationResourceBundle activeBundle = TakePresentationResourceBundle();
	oldSwapchainConsumedByCreateAttempt = false;
	VulkanVRS activeVrs = vrs;
	vrs = {};
	bool candidateVrsCreated = false;

	const bool swapchainCreated = CreateSwapChain(*windowSystem, activeBundle.swapchain);
	bool candidateCreated = swapchainCreated && CreateImageViews();
	if (candidateCreated && !editorMode) {
		candidateCreated = CreateDepthResources();
		if (candidateCreated && vrsFeatures.pipelineFragmentShadingRate) {
			vrs.Create(device, physicalDevice, allocator, swapchainExtent.width, swapchainExtent.height);
			candidateVrsCreated = vrs.view != VK_NULL_HANDLE;
			if (candidateVrsCreated) {
				candidateCreated = TransitionImageLayout(vrs.image, VK_FORMAT_R8_UINT,
					VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR);
			}
		}
	}
	candidateCreated = candidateCreated && CreateRenderPass()
		&& CreateFramebuffers() && CreateSyncObjects();
	if (candidateCreated) {
		candidateCreated = presentationLifecycle.Configure(
			MAX_FRAMES_IN_FLIGHT, surfaceMinimumImageCount,
			requestedSwapchainImageCount,
			static_cast<std::uint32_t>(swapchainImages.size()));
	}

	if (!candidateCreated) {
		PresentationResourceBundle failedCandidate = TakePresentationResourceBundle();
		DestroyPresentationResourceBundle(failedCandidate);
		if (candidateVrsCreated || vrs.view != VK_NULL_HANDLE) vrs.Destroy(device, allocator);
		vrs = activeVrs;
		if (oldSwapchainConsumedByCreateAttempt) {
			// The old chain was supplied as oldSwapchain. Device-idle above proves
			// submitted queue work is done, but it does not make the old handle
			// reusable after the API ownership handoff attempt. Retire its image
			// synchronization until completion proof and remain safely suspended.
			const bool oldBundleHasExplicitCompletionProof = activeBundle.presentCompletionCoverageValid;
			RetirePresentationResourceBundle(std::move(activeBundle), presentationLifecycle.Generation());
			// No replacement chain exists to provide the unextended acquire-history
			// anchor for this newly retired bundle. Only maintenance fences can recover.
			unextendedRetirementTracker.Reset();
			presentationRecreation.PublishRetirementBlocked(true);
			framebufferResized = true;
			if (oldBundleHasExplicitCompletionProof) {
				TERRAENGINE_LOG(LogLevel::Error, "Presentation candidate failed after oldSwapchain handoff; suspended until maintenance-fence proof releases the retired chain.");
			} else {
				// Vulkan has no standards-defined unextended completion proof when the
				// old chain was retired and no replacement chain exists. Remain terminal
				// and fail closed instead of relying on device-idle and risking driver use-after-free.
				TERRAENGINE_LOG(LogLevel::Error, "Presentation candidate failed after oldSwapchain handoff without maintenance fences; presentation is terminally suspended fail-closed.");
			}
			return;
		}
		RestorePresentationResourceBundle(std::move(activeBundle));
		presentationRecreation.PublishFailure(false);
		framebufferResized = true;
		presentationLifecycle.RequestRecreation();
		TERRAENGINE_LOG(LogLevel::Error, "Presentation candidate creation failed; the previous bundle remains active and retry stays pending.");
		return;
	}

	editorCompletionTracker.SetPresentationGeneration(presentationLifecycle.Generation());
	// The complete candidate is now active. Advance only after every dependent
	// object has been created, so a failed candidate leaves existing publications
	// valid and the next frame can retry against the prior generation.
	++backendResourceGeneration;
	reusableSecondaryCommandSignatures.clear();
	activeRenderFramePublication.reset();
	RetirePresentationResourceBundle(std::move(activeBundle), presentationLifecycle.Generation());
	CollectCompletedPresentationResourceBundles();
	if (activeVrs.view != VK_NULL_HANDLE) activeVrs.Destroy(device, allocator);

	if (!editorMode) {
		// These objects depend on extent or render-pass compatibility, while
		// shadows, global buffers, descriptors, caches, scenes, and CPU assets do not.
		DestroyAmbientOcclusionResources();
		if (hiZDescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, hiZDescriptorPool, nullptr);
			hiZDescriptorPool = VK_NULL_HANDLE;
			hiZDescriptorSets.clear();
		}
		if (aoDescriptorPool != VK_NULL_HANDLE) {
			vkDestroyDescriptorPool(device, aoDescriptorPool, nullptr);
			aoDescriptorPool = VK_NULL_HANDLE;
			aoDescriptorSets.clear();
		}
		for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
			for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
				for (VulkanHiZ& frameHiZ : frameHiZPair) frameHiZ.Destroy(device, allocator);
			}
		}
		DestroyPipelines();
		if (staticUiPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, staticUiPipeline, nullptr);
			staticUiPipeline = VK_NULL_HANDLE;
		}
		if (sdfTextPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, sdfTextPipeline, nullptr);
			sdfTextPipeline = VK_NULL_HANDLE;
		}

		const VkFormat depthFmt = FindDepthFormat();
		for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
			for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
				for (VulkanHiZ& frameHiZ : frameHiZPair) {
					frameHiZ.Create(device, allocator, swapchainExtent.width, swapchainExtent.height, depthFmt);
				}
			}
		}
		for (std::array<std::uint32_t, MAX_FRAMES_IN_FLIGHT>& indices : hiZReadIndexByViewFrame) indices.fill(0u);
		for (std::array<bool, MAX_FRAMES_IN_FLIGHT>& swaps : hiZWritePendingSwapByViewFrame) swaps.fill(false);
		CreateAmbientOcclusionResources();
		for (std::uint32_t frameIndex = 0; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
			UpdateGlobalHiZ(frameIndex);
		}
	}

	editorRenderSubmitted = false;
	swapchainImageAcquired = false;
	swapchainAcquireWaitConsumed = false;
	acquiredPresentationImage.reset();
	PublishPresentationRecreationSuccess();
	TERRAENGINE_LOG(LogLevel::Info, "Presentation candidate published atomically. generation="
		<< presentationLifecycle.Generation() << ", images=" << swapchainImages.size());
}

// VulkanContext::Impl::DebugCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::Impl::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
	// Route validation messages through the engine logger so users see them in one place.
	const char* message = (pCallbackData != nullptr && pCallbackData->pMessage != nullptr) ? pCallbackData->pMessage : "";

	LogLevel level = LogLevel::Debug;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) level = LogLevel::Error;
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = LogLevel::Warning;
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) level = LogLevel::Info;
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) level = LogLevel::Debug;
	
	TERRAENGINE_LOG(level, "[Vulkan] " << message);
	return VK_FALSE;
}

// VulkanContext::Impl::CreateDebugUtilsMessengerEXT is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
VkResult VulkanContext::Impl::CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	// Dynamically fetch the debug messenger extension function.
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (func != nullptr) {
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	} else {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
}

// VulkanContext::Impl::DestroyDebugUtilsMessengerEXT is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
	// Safely tear down the debug messenger when the instance is destroyed.
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (func != nullptr) {
		func(instance, debugMessenger, pAllocator);
	}
}
