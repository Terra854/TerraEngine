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

bool VulkanContext::Impl::InitVulkan(WindowSystem& windowSystem) {
	// Spin up instance, device, swapchain, and default resources so rendering can begin.
	this->windowSystem = &windowSystem;
	deviceLost = false;
	deviceLostLogged = false;
	++renderSessionGeneration;
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
				InitializeHiZPyramidToFarDepth(frameHiZ);
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

// VulkanContext::Impl::Cleanup is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::Cleanup(bool skipDeviceIdle) {
	if (device && !skipDeviceIdle) {
		const VkResult idleResult = vkDeviceWaitIdle(device);
		if (idleResult != VK_SUCCESS)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Vulkan cleanup could not prove the device idle before teardown: " << idleResult);
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
		vmaDestroyBuffer(allocator, mesh.meshDecodeInfoBuffer.buffer, mesh.meshDecodeInfoBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.meshletBuffer.buffer, mesh.meshletBuffer.allocation);
		if (mesh.meshletSectionBuffer.buffer != VK_NULL_HANDLE) {
			vmaDestroyBuffer(allocator, mesh.meshletSectionBuffer.buffer, mesh.meshletSectionBuffer.allocation);
		}
		vmaDestroyBuffer(allocator, mesh.meshletBoundsBuffer.buffer, mesh.meshletBoundsBuffer.allocation);
	}
	meshCache.clear();

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
	activeVirtualGeometryPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryBatchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCompactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryGeneratedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	activeVirtualGeometryCounterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;

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

	auto DestroyOffscreenResources = [&](OffscreenResources& res) {
		if (device) {
			if (res.sampler != VK_NULL_HANDLE) vkDestroySampler(device, res.sampler, nullptr);
			if (res.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, res.framebuffer, nullptr);
			if (res.view != VK_NULL_HANDLE) vkDestroyImageView(device, res.view, nullptr);
			if (res.depthView != VK_NULL_HANDLE) vkDestroyImageView(device, res.depthView, nullptr);
		}
		if (res.image != VK_NULL_HANDLE && res.allocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, res.image, res.allocation);
		}
		if (res.depthImage != VK_NULL_HANDLE && res.depthAllocation != VK_NULL_HANDLE) {
			vmaDestroyImage(allocator, res.depthImage, res.depthAllocation);
		}
		res = {};
	};

	DestroyOffscreenResources(sceneView);
	DestroyOffscreenResources(gameView);
	DestroyOffscreenResources(captureView);
	
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiStaticVertexBuffer.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, uiStaticVertexBuffer.buffer, uiStaticVertexBuffer.allocation);
		uiStaticVertexBuffer.buffer = VK_NULL_HANDLE;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiInstanceBuffer.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, uiInstanceBuffer.buffer, uiInstanceBuffer.allocation);
		uiInstanceBuffer.buffer = VK_NULL_HANDLE;
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (uiSdfVertexBuffer.buffer != VK_NULL_HANDLE) {
		vmaDestroyBuffer(allocator, uiSdfVertexBuffer.buffer, uiSdfVertexBuffer.allocation);
		uiSdfVertexBuffer.buffer = VK_NULL_HANDLE;
	}

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

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
		if (device) {
			vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
			vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
			vkDestroySemaphore(device, uiSemaphores[i], nullptr);
			vkDestroyFence(device, inFlightFences[i], nullptr);
		}
	}
	renderFinishedSemaphores.clear();
	imageAvailableSemaphores.clear();
	uiSemaphores.clear();
	inFlightFences.clear();

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
	}
	commandPools.clear();
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
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		vkDestroyDescriptorPool(device, globalDescriptorPool, nullptr);
		vkDestroyDescriptorSetLayout(device, globalDescriptorSetLayout, nullptr);
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (validationLayersEnabled) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();

		PopulateDebugMessengerCreateInfo(debugCreateInfo);
		createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
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

		VkPhysicalDeviceMeshShaderFeaturesEXT queriedMeshFeatures{};
		queriedMeshFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;

		VkPhysicalDeviceFeatures2 queriedFeatures{};
		queriedFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		queriedFeatures.pNext = &queriedMeshFeatures;
		vkGetPhysicalDeviceFeatures2(physicalDevice, &queriedFeatures);
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
	deviceFeatures.samplerAnisotropy = VK_TRUE;

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

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.pNext = &features2; // Chain features here

	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();

	createInfo.pEnabledFeatures = nullptr; // Must be null when using pNext for features

	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();

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

	vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
	return true;
}

// VulkanContext::Impl::CreateSwapChain is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateSwapChain(WindowSystem& windowSystem) {
	// Assemble swapchain matching the current surface capabilities.
	SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(physicalDevice);

	VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
	VkExtent2D extent = ChooseSwapExtent(swapChainSupport.capabilities, windowSystem);

	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}

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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create swap chain!");
		return false;
	}

	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
	swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());

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

// VulkanContext::Impl::CreateRenderPass is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool VulkanContext::Impl::CreateRenderPass() {
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

		VkAttachmentDescription2 depthAttachment{};
		depthAttachment.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
		depthAttachment.format = depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

		VkSubpassDependency2 dependency{};
		dependency.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription2, 3> attachments = {colorAttachment, depthAttachment, vrsAttachment};
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

		VkAttachmentDescription depthAttachment{};
		depthAttachment.format = depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
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
	// Pair each swapchain view with the shared depth buffer and optional VRS image.
	framebuffers.resize(swapchainImageViews.size());

	for (size_t i = 0; i < swapchainImageViews.size(); i++) {
		std::vector<VkImageView> attachments = {
			swapchainImageViews[i],
			depthImageView
		};

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

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPools[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create command pool!");
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
	// Create semaphores and fences for every frame-in-flight slot.
	imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	uiSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
	inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
	extraPrimaryCommandBuffers.clear();
	extraPrimaryCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	pendingPrimaryCommandBuffers.clear();
	pendingPrimaryCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
	pendingPrimaryCommandBuffersSubmitted.clear();
	pendingPrimaryCommandBuffersSubmitted.resize(MAX_FRAMES_IN_FLIGHT, 0u);
	retiredBuffersByFrame.clear();
	retiredBuffersByFrame.resize(MAX_FRAMES_IN_FLIGHT);

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS ||
			vkCreateSemaphore(device, &semaphoreInfo, nullptr, &uiSemaphores[i]) != VK_SUCCESS ||
			vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create synchronization objects for a frame!");
			return false;
		}
	}
	return true;
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
	poolSizes[4].descriptorCount = 128;
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
		swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
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
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (formatCount != 0) {
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (presentModeCount != 0) {
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
	}
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
	framebuffers.clear();
	swapchainImageViews.clear();
}

// VulkanContext::Impl::RecreateSwapChain is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void VulkanContext::Impl::RecreateSwapChain() {
	if (deviceLost) {
		return;
	}
	// Block until a valid framebuffer size is available (e.g., when resizing).
	int width = 0, height = 0;
	auto* window = static_cast<GLFWwindow*>(windowSystem->GetNativeHandle());
	glfwGetFramebufferSize(window, &width, &height);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(window, &width, &height);
		glfwWaitEvents();
	}
	const VkResult idleResult = vkDeviceWaitIdle(device);
	if (MarkDeviceLost(idleResult, "swapchain recreation device idle")) {
		return;
	}
	if (idleResult != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "Skipping swapchain recreation because the device did not become idle: " << idleResult);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		auto DestroyShadowFramebuffer = [&](VkFramebuffer& framebuffer) {
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (shadowRenderPass != VK_NULL_HANDLE) {
			vkDestroyRenderPass(device, shadowRenderPass, nullptr);
			shadowRenderPass = VK_NULL_HANDLE;
		}
		activeShadowResourceRequest = {};
		shadowStorageRegions.fill({});
		shadowCacheEntries.clear();
	}
	previousClusterValidationSnapshotValid = false;
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ViewDepthHistoryState& depthHistory : depthHistoryByView) {
		depthHistory.valid = false;
		depthHistory.ambientOcclusionDepthValidByFrame.fill(false);
	}

	DestroyPipelines();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (device) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (staticUiPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, staticUiPipeline, nullptr);
			staticUiPipeline = VK_NULL_HANDLE;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sdfTextPipeline != VK_NULL_HANDLE) {
			vkDestroyPipeline(device, sdfTextPipeline, nullptr);
			sdfTextPipeline = VK_NULL_HANDLE;
		}
	}
	// Do NOT reset descriptor pool here. ImGui uses it and its font texture lives there.
	// if (descriptorPool) {
	// 	VkResult resetResult = vkResetDescriptorPool(device, descriptorPool, 0);
	// 	if (resetResult != VK_SUCCESS) {
	// 		TERRAENGINE_LOG(LogLevel::Warning, "Failed to reset descriptor pool during swapchain recreation");
	// 	}
	// }

	CleanupSwapChain();

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

	DestroyBufferVector(globalFrameCullingSSBOs);
	DestroyBufferVector(globalMeshletCountersSSBOs);
	DestroyBufferVector(globalLightSSBOs);
	DestroyBufferVector(lightAnalysisInputSSBOs);
	DestroyBufferVector(clusterGridSSBOs);
	DestroyBufferVector(lightIndexSSBOs);
	DestroyBufferVector(clusterDepthSSBOs);
	DestroyBufferVector(lightAnalysisSSBOs);
	DestroyBufferVector(indirectDrawBuffers);

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

	vkDestroyImageView(device, depthImageView, nullptr);
	vmaDestroyImage(allocator, depthImage, depthImageAllocation);

	depthImage = VK_NULL_HANDLE;
	depthImageAllocation = VK_NULL_HANDLE;
	depthImageView = VK_NULL_HANDLE;

	// Destroy sync objects to reset their state (e.g. clear signaled semaphores from failed frames)
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
		vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
		vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
		vkDestroySemaphore(device, uiSemaphores[i], nullptr);
		vkDestroyFence(device, inFlightFences[i], nullptr);
	}
	renderFinishedSemaphores.clear();
	imageAvailableSemaphores.clear();
	uiSemaphores.clear();
	inFlightFences.clear();

	CreateSwapChain(*windowSystem);
	CreateImageViews();
	CreateDepthResources();

	VkFormat depthFmt = FindDepthFormat();
	for (std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>& viewHiZFrames : hiZByViewFrame) {
		for (std::array<VulkanHiZ, 2>& frameHiZPair : viewHiZFrames) {
			for (VulkanHiZ& frameHiZ : frameHiZPair) {
				frameHiZ.Create(device, allocator, swapchainExtent.width, swapchainExtent.height, depthFmt);
				InitializeHiZPyramidToFarDepth(frameHiZ);
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

	CreateRenderPass();
	CreateFramebuffers();
	CreateSyncObjects(); // Recreate fresh sync objects
	workerCommandPools.resize(MAX_FRAMES_IN_FLIGHT);

	CreateCullingBufferResources();
	CreateLightingBufferResources();
	CreateAmbientOcclusionResources();
	CreateIndirectDrawResources();
	for (uint32_t frameIndex = 0u; frameIndex < MAX_FRAMES_IN_FLIGHT; ++frameIndex) {
		UpdateGlobalHiZ(frameIndex);
	}
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
