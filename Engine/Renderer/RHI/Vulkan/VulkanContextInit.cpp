#include "VulkanContextImpl.h"

#include <algorithm>

bool VulkanContext::Impl::InitVulkan(WindowSystem& windowSystem) {
	// Spin up instance, device, swapchain, and default resources so rendering can begin.
	this->windowSystem = &windowSystem;
	volkInitialize();

	if (!CreateInstance()) return false;
	if (!SetupDebugMessenger()) return false;
	
	auto* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
	if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create window surface!");
		return false;
	}

	if (!PickPhysicalDevice()) return false;
	if (!CreateLogicalDevice()) return false;

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

	if (!CreateSwapChain(windowSystem)) return false;
	if (!CreateImageViews()) return false;
	if (!CreateDepthResources()) return false;
	if (!CreateRenderPass()) return false;
	if (!CreateFramebuffers()) return false;
	if (!CreateCommandPool()) return false;
	if (!CreateSyncObjects()) return false;
	if (!CreateDescriptorPool()) return false;
		if (!CreateGlobalDescriptorResources()) return false;
	CreateMaterialBufferResources();

	workerCommandPools.resize(MAX_FRAMES_IN_FLIGHT);
	workerCommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

	try {
		CreateDefaultTexture();
	} catch (const std::exception& e) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to create default texture: " << e.what());
		Cleanup();
		return false;
	}
	
	initialized = true;
	return true;
}

void VulkanContext::Impl::Cleanup() {
	if (device) {
		vkDeviceWaitIdle(device);
	}

	DestroyWorkerPools();
	CleanupSwapChain();

	for (auto& [id, mesh] : meshCache) {
		vmaDestroyBuffer(allocator, mesh.indexBuffer.buffer, mesh.indexBuffer.allocation);
		vmaDestroyBuffer(allocator, mesh.vertexBuffer.buffer, mesh.vertexBuffer.allocation);
	}
	meshCache.clear();

	for (auto& [id, tex] : textureCache) {
		if (device) {
			vkDestroySampler(device, tex.sampler, nullptr);
			vkDestroyImageView(device, tex.view, nullptr);
		}
		vmaDestroyImage(allocator, tex.image, tex.allocation);
	}
	textureCache.clear();

	if (offscreenImage != VK_NULL_HANDLE) {
		if (device) {
			vkDestroySampler(device, offscreenSampler, nullptr);
			vkDestroyFramebuffer(device, offscreenFramebuffer, nullptr);
			vkDestroyImageView(device, offscreenImageView, nullptr);
			vkDestroyRenderPass(device, offscreenRenderPass, nullptr); // offscreenRenderPass is separate
		}
		vmaDestroyImage(allocator, offscreenImage, offscreenAllocation);
		offscreenImage = VK_NULL_HANDLE;
		offscreenAllocation = VK_NULL_HANDLE;
		offscreenImageView = VK_NULL_HANDLE;
		offscreenFramebuffer = VK_NULL_HANDLE;
		offscreenSampler = VK_NULL_HANDLE;
		offscreenRenderPass = VK_NULL_HANDLE;
	}

	DestroyPipelines();

	if (device) {
		vkDestroySampler(device, defaultTexture.sampler, nullptr);
		vkDestroyImageView(device, defaultTexture.view, nullptr);
	}
	vmaDestroyImage(allocator, defaultTexture.image, defaultTexture.allocation);
	vmaDestroyBuffer(allocator, globalMaterialSSBO.buffer, globalMaterialSSBO.allocation);

	for (size_t i = 0; i < renderFinishedSemaphores.size(); i++) {
		if (device) {
			vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
			vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
			vkDestroySemaphore(device, uiSemaphores[i], nullptr);
			vkDestroyFence(device, inFlightFences[i], nullptr);
		}
	}

	if (device) {
		vkDestroyImageView(device, depthImageView, nullptr);
		vmaDestroyImage(allocator, depthImage, depthImageAllocation);
	}

	if (device) {
		for (VkCommandPool pool : commandPools) {
			vkDestroyCommandPool(device, pool, nullptr);
		}
	}
	commandPools.clear();

	if (device) {
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		vkDestroyDescriptorPool(device, globalDescriptorPool, nullptr);
		vkDestroyDescriptorSetLayout(device, globalDescriptorSetLayout, nullptr);
	}

	if (allocator) vmaDestroyAllocator(allocator);

	DestroyWorkerPools();
	if (device) vkDestroyDevice(device, nullptr);

	if (enableValidationLayers && instance) {
		DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
	}

	if (instance) {
		vkDestroySurfaceKHR(instance, surface, nullptr);
		vkDestroyInstance(instance, nullptr);
	}
	initialized = false;
}

bool VulkanContext::Impl::CreateInstance() {
	// Build the instance and verify that requested validation layers are present.
	if (enableValidationLayers && !CheckValidationLayerSupport()) {
		TERRAENGINE_LOG(LogLevel::Error, "validation layers requested, but not available!");
		return false;
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
	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();

		PopulateDebugMessengerCreateInfo(debugCreateInfo);
		createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
	} else {
		createInfo.enabledLayerCount = 0;
		createInfo.pNext = nullptr;
	}

	if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create instance!");
		return false;
	}

	volkLoadInstance(instance);
	return true;
}

void VulkanContext::Impl::PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
	// Configure debug messenger severity and message types before creation.
	createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = DebugCallback;
}

bool VulkanContext::Impl::SetupDebugMessenger() {
	// Install the debug messenger when validation layers are enabled.
	if (!enableValidationLayers) return true;

	VkDebugUtilsMessengerCreateInfoEXT createInfo;
	PopulateDebugMessengerCreateInfo(createInfo);

	if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to set up debug messenger!");
		return false;
	}
	return true;
}

bool VulkanContext::Impl::PickPhysicalDevice() {
	// Select the first suitable GPU that supports required queues and extensions.
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

	if (deviceCount == 0) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to find GPUs with Vulkan support!");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

	for (const auto& device : devices) {
		if (IsDeviceSuitable(device)) {
			physicalDevice = device;
			break;
		}
	}

	if (physicalDevice == VK_NULL_HANDLE) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to find a suitable GPU!");
		return false;
	}
	return true;
}

bool VulkanContext::Impl::CreateLogicalDevice() {
	// Build queues and logical device features needed for rendering.
	QueueFamilyIndices indices = FindQueueFamilies(physicalDevice);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};

	float queuePriority = 1.0f;
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
	// Buffer device address is often useful with bindless but strictly optional
	vulkan12Features.bufferDeviceAddress = VK_TRUE;

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

	if (enableValidationLayers) {
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	} else {
		createInfo.enabledLayerCount = 0;
	}

	if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create logical device!");
		return false;
	}

	volkLoadDevice(device);

	vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
	return true;
}

bool VulkanContext::Impl::CreateSwapChain(WindowSystem& windowSystem) {
	// Assemble swapchain matching the current surface capabilities.
	SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(physicalDevice);

	VkSurfaceFormatKHR surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
	VkPresentModeKHR presentMode = ChooseSwapPresentMode(swapChainSupport.presentModes);
	VkExtent2D extent = ChooseSwapExtent(swapChainSupport.capabilities, windowSystem);

	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
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

	if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create swap chain!");
		return false;
	}

	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
	swapchainImages.resize(imageCount);
	vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages.data());

	swapchainImageFormat = surfaceFormat.format;
	swapchainExtent = extent;
	MAX_FRAMES_IN_FLIGHT = static_cast<int>(swapchainImages.size());
	return true;
}

bool VulkanContext::Impl::CreateImageViews() {
	// Build image views for every swapchain image.
	swapchainImageViews.resize(swapchainImages.size());

	for (size_t i = 0; i < swapchainImages.size(); i++) {
		swapchainImageViews[i] = CreateImageView(swapchainImages[i], swapchainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
		if (swapchainImageViews[i] == VK_NULL_HANDLE) return false;
	}
	return true;
}

VkFormat VulkanContext::Impl::FindDepthFormat() {
	// Find a depth format compatible with the GPU for depth testing.
	return FindSupportedFormat(
		{VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

VkFormat VulkanContext::Impl::FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
	// Pick the first candidate that supports requested tiling and features.
	for (VkFormat format : candidates) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
			return format;
		} else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
			return format;
		}
	}
	throw std::runtime_error("failed to find supported format!");
}

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
	imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo = {};
	allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

	if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &depthImage, &depthImageAllocation, nullptr) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create depth image!");
		return false;
	}

	depthImageView = CreateImageView(depthImage, chosenDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
	return true;
}

bool VulkanContext::Impl::CreateRenderPass() {
	// Create a render pass that clears color and depth and outputs to the swapchain.
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

	if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create render pass!");
		return false;
	}
	return true;
}

bool VulkanContext::Impl::CreateFramebuffers() {
	// Pair each swapchain view with the shared depth buffer.
	framebuffers.resize(swapchainImageViews.size());

	for (size_t i = 0; i < swapchainImageViews.size(); i++) {
		std::array<VkImageView, 2> attachments = {
			swapchainImageViews[i],
			depthImageView
		};

		VkFramebufferCreateInfo framebufferInfo{};
		framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferInfo.renderPass = renderPass;
		framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		framebufferInfo.pAttachments = attachments.data();
		framebufferInfo.width = swapchainExtent.width;
		framebufferInfo.height = swapchainExtent.height;
		framebufferInfo.layers = 1;

		if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to create framebuffer!");
			return false;
		}
	}
	return true;
}

bool VulkanContext::Impl::CreateCommandPool() {
	// Create a command pool that allows buffer resets for per-frame recording.
	QueueFamilyIndices queueFamilyIndices = FindQueueFamilies(physicalDevice);

	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

	commandPools.resize(MAX_FRAMES_IN_FLIGHT);
	commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

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

		if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffers[i]) != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Error, "failed to allocate command buffers!");
			return false;
		}
	}
	return true;
}

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

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

	VkFenceCreateInfo fenceInfo{};
	fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

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

	if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
		TERRAENGINE_LOG(LogLevel::Error, "failed to create descriptor pool!");
		return false;
	}
	return true;
}

bool VulkanContext::Impl::CheckValidationLayerSupport() {
	// Ensure requested validation layers are available before enabling them.
	uint32_t layerCount;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

	std::vector<VkLayerProperties> availableLayers(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

	for (const char* layerName : validationLayers) {
		bool layerFound = false;
		for (const auto& layerProperties : availableLayers) {
			if (strcmp(layerName, layerProperties.layerName) == 0) {
				layerFound = true;
				break;
			}
		}

		if (!layerFound) {
			return false;
		}
	}

	return true;
}

std::vector<const char*> VulkanContext::Impl::GetRequiredExtensions() {
	// Gather extensions required by GLFW and validation layers.
	uint32_t glfwExtensionCount = 0;
	const char** glfwExtensions;
	glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

	std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

	if (enableValidationLayers) {
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}

	return extensions;
}

bool VulkanContext::Impl::IsDeviceSuitable(VkPhysicalDevice device) {
	// Check for queues, extensions, and swapchain support.
	QueueFamilyIndices indices = FindQueueFamilies(device);
	bool extensionsSupported = CheckDeviceExtensionSupport(device);
	bool swapChainAdequate = false;
	if (extensionsSupported) {
		SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
		swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	if (!indices.isComplete() || !extensionsSupported || !swapChainAdequate) {
		return false;
	}

	// Check for Descriptor Indexing support (Vulkan 1.2)
	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &features12;

	vkGetPhysicalDeviceFeatures2(device, &features2);

	if (!features12.descriptorIndexing || !features12.runtimeDescriptorArray || 
		!features12.shaderSampledImageArrayNonUniformIndexing || 
		!features12.descriptorBindingPartiallyBound || 
		!features12.descriptorBindingVariableDescriptorCount) {
		return false;
	}

	return true;
}

bool VulkanContext::Impl::CheckDeviceExtensionSupport(VkPhysicalDevice device) {
	// Verify the device supports the extensions we require (swapchain).
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

	std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

	for (const auto& extension : availableExtensions) {
		requiredExtensions.erase(extension.extensionName);
	}

	return requiredExtensions.empty();
}

QueueFamilyIndices VulkanContext::Impl::FindQueueFamilies(VkPhysicalDevice device) {
	// Locate queue families capable of graphics and present operations.
	QueueFamilyIndices indices;
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	int i = 0;
	for (const auto& queueFamily : queueFamilies) {
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			indices.graphicsFamily = i;
		}
		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
		if (presentSupport) {
			indices.presentFamily = i;
		}
		if (indices.isComplete()) break;
		i++;
	}
	return indices;
}

SwapChainSupportDetails VulkanContext::Impl::QuerySwapChainSupport(VkPhysicalDevice device) {
	// Query swapchain capabilities for the given physical device.
	SwapChainSupportDetails details;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

	uint32_t formatCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
	if (formatCount != 0) {
		details.formats.resize(formatCount);
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
	}

	uint32_t presentModeCount;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
	if (presentModeCount != 0) {
		details.presentModes.resize(presentModeCount);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
	}
	return details;
}

VkSurfaceFormatKHR VulkanContext::Impl::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
	// Prefer SRGB if available, otherwise pick the first format.
	for (const auto& availableFormat : availableFormats) {
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			return availableFormat;
		}
	}
	return availableFormats[0];
}

VkPresentModeKHR VulkanContext::Impl::ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
	// Use mailbox for low latency when possible, otherwise fallback to FIFO.
	for (const auto& availablePresentMode : availablePresentModes) {
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			return availablePresentMode;
		}
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

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

void VulkanContext::Impl::CleanupSwapChain() {
	// Destroy swapchain-related resources while keeping device-level objects intact.
	if (device) {
		for (auto framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
		if (renderPass) vkDestroyRenderPass(device, renderPass, nullptr);
		for (auto imageView : swapchainImageViews) vkDestroyImageView(device, imageView, nullptr);
		if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
	}
	framebuffers.clear();
	swapchainImageViews.clear();
}

void VulkanContext::Impl::RecreateSwapChain() {
	// Block until a valid framebuffer size is available (e.g., when resizing).
	int width = 0, height = 0;
	auto* window = static_cast<GLFWwindow*>(windowSystem->GetNativeHandle());
	glfwGetFramebufferSize(window, &width, &height);
	while (width == 0 || height == 0) {
		glfwGetFramebufferSize(window, &width, &height);
		glfwWaitEvents();
	}
	vkDeviceWaitIdle(device);

	DestroyPipelines();
	if (descriptorPool) {
		VkResult resetResult = vkResetDescriptorPool(device, descriptorPool, 0);
		if (resetResult != VK_SUCCESS) {
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to reset descriptor pool during swapchain recreation");
		}
	}

	CleanupSwapChain();

	vkDestroyImageView(device, depthImageView, nullptr);
	vmaDestroyImage(allocator, depthImage, depthImageAllocation);

	// Destroy sync objects to reset their state (e.g. clear signaled semaphores from failed frames)
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
	CreateRenderPass();
	CreateFramebuffers();
	CreateSyncObjects(); // Recreate fresh sync objects
	workerCommandPools.resize(MAX_FRAMES_IN_FLIGHT);
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::Impl::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
	// Route validation messages through the engine logger so users see them in one place.
	LogLevel level = LogLevel::Info;
	if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) level = LogLevel::Error;
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) level = LogLevel::Warning;
	else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) level = LogLevel::Info;
	
	TERRAENGINE_LOG(level, "[Vulkan] " << pCallbackData->pMessage);
	return VK_FALSE;
}

VkResult VulkanContext::Impl::CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
	// Dynamically fetch the debug messenger extension function.
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr) {
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	} else {
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}
}

void VulkanContext::Impl::DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
	// Safely tear down the debug messenger when the instance is destroyed.
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr) {
		func(instance, debugMessenger, pAllocator);
	}
}
