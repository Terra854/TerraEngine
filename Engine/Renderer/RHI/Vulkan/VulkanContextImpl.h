#pragma once

#include "VulkanContext.h"
#include "VulkanCommonIncludes.h"

#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Scene/SceneManager.h"

#include <vector>
#include <set>
#include <array>
#include <cstring>
#include <optional>
#include <map>
#include <unordered_map>
#include <limits>
#include <stdexcept>

#ifdef NDEBUG
inline const bool enableValidationLayers = false;
#else
inline const bool enableValidationLayers = true;
#endif

inline const std::vector<const char*> validationLayers = {
	"VK_LAYER_KHRONOS_validation"
};

inline const std::vector<const char*> deviceExtensions = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Queue family indices used to set up graphics/present queues.
struct QueueFamilyIndices {
	std::optional<uint32_t> graphicsFamily;
	std::optional<uint32_t> presentFamily;

	bool isComplete() {
		return graphicsFamily.has_value() && presentFamily.has_value();
	}
};

// Swap chain capabilities returned by physical devices.
struct SwapChainSupportDetails {
	VkSurfaceCapabilitiesKHR capabilities;
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
};

// GPU buffer wrapper that keeps the VMA allocation alongside the VkBuffer.
struct VulkanBuffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	VkDeviceSize size;
};

// GPU image wrapper for textures and attachments.
struct VulkanImage {
	VkImage image;
	VmaAllocation allocation;
	VkImageView view;
	VkSampler sampler;
};

// Mesh buffers cached on the GPU.
struct VulkanMesh {
	VulkanBuffer vertexBuffer;
	VulkanBuffer indexBuffer;
	uint32_t indexCount;
	VkIndexType indexType;
};

// Pipeline pairing with its layout to avoid redundant lookups.
struct VulkanPipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<std::vector<DescriptorBinding>> bindingsPerSet;
	std::vector<VkPushConstantRange> pushConstants;
	bool supportsDefaultPushConstants = false;
};

// Push constant payload required by the preview renderer.
struct PushConstants {
	glm::mat4 modelViewProjection;
	glm::vec4 positionOffset;
	glm::vec4 positionScale;
	glm::vec4 uvOffsetScale;
};

struct VulkanContext::Impl {
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	VkQueue presentQueue = VK_NULL_HANDLE;

	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	std::vector<VkImage> swapchainImages;
	VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent;
	std::vector<VkImageView> swapchainImageViews;

	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> framebuffers;

	VkImage depthImage = VK_NULL_HANDLE;
	VmaAllocation depthImageAllocation = VK_NULL_HANDLE;
	VkImageView depthImageView = VK_NULL_HANDLE;

	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> commandBuffers;

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

	std::vector<VkSemaphore> imageAvailableSemaphores;
	std::vector<VkSemaphore> renderFinishedSemaphores;
	std::vector<VkFence> inFlightFences;

	VmaAllocator allocator = VK_NULL_HANDLE;
	WindowSystem* windowSystem = nullptr;

	uint32_t currentFrame = 0;
	bool framebufferResized = false;
	int MAX_FRAMES_IN_FLIGHT = 2;
	bool initialized = false;

	std::unordered_map<TerraEngine::UUID, VulkanMesh> meshCache;
	std::unordered_map<TerraEngine::UUID, VulkanImage> textureCache;
	std::unordered_map<TerraEngine::UUID, VulkanPipeline> pipelineCache;
	std::unordered_map<TerraEngine::UUID, std::unordered_map<TerraEngine::UUID, std::vector<VkDescriptorSet>>> materialDescriptorSets;

	VulkanImage defaultTexture;

	bool InitVulkan(WindowSystem& windowSystem);
	void Cleanup();

	bool CreateInstance();
	void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);
	bool SetupDebugMessenger();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();

	bool CreateSwapChain(WindowSystem& windowSystem);
	bool CreateImageViews();
	VkFormat FindDepthFormat();
	VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
	bool CreateDepthResources();
	bool CreateRenderPass();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateSyncObjects();

	bool CreateDescriptorPool();

	void CreateDefaultTexture();
	VulkanBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	VkCommandBuffer BeginSingleTimeCommands();
	void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
	VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

	VkFormat MapTextureFormat(uint8_t format, uint8_t channelType, uint8_t semantic);
	void EnsureTexture(TerraEngine::UUID textureId);
	std::vector<VkDescriptorSet> EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId);
	void EnsureMesh(TerraEngine::UUID meshId);
	void EnsurePipeline(TerraEngine::UUID shaderCollectionId);

	VkDescriptorType MapDescriptorType(DescriptorType type);
	VkShaderStageFlags StageVisibilityToVulkan(StageVisibilityMask visibility);
	void DestroyPipelines();
	bool BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants);

	bool CheckValidationLayerSupport();
	std::vector<const char*> GetRequiredExtensions();
	bool IsDeviceSuitable(VkPhysicalDevice device);
	bool CheckDeviceExtensionSupport(VkPhysicalDevice device);
	QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device);
	SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice device);
	VkSurfaceFormatKHR ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats);
	VkPresentModeKHR ChooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes);
	VkExtent2D ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, WindowSystem& windowSystem);

	void CleanupSwapChain();
	void RecreateSwapChain();

	static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);
	VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger);
	void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator);
};
