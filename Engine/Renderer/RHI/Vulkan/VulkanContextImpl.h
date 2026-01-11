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
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
	VK_KHR_MAINTENANCE3_EXTENSION_NAME
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
	glm::mat4 viewProj;
    glm::mat4 model;
	uint32_t textureIndex;
	uint32_t materialIndex;
    uint32_t padding[2];
};

// Must match shader definition
struct alignas(16) MaterialData {
	glm::vec4 BaseColorFactor;
	glm::vec4 EmissiveFactor;
	glm::vec4 UVTransform; // xy = scale, zw = offset
	float MetallicFactor;
	float RoughnessFactor;
	float NormalScale;
	float OcclusionStrength;
	float AlphaCutoff;
	uint32_t AlphaMode;
	uint32_t Flags;
	uint32_t BaseColorTextureIndex;
	uint32_t NormalTextureIndex;
	uint32_t MaskTextureIndex;
	uint32_t OcclusionTextureIndex;
	uint32_t EmissiveTextureIndex;
	uint32_t Padding[2];
};

struct PipelineKey {
	TerraEngine::UUID shaderCollectionId;
	uint32_t materialFlags;

	bool operator==(const PipelineKey& other) const {
		return shaderCollectionId == other.shaderCollectionId && materialFlags == other.materialFlags;
	}
};

struct PipelineKeyHash {
	std::size_t operator()(const PipelineKey& k) const {
		std::size_t h1 = std::hash<TerraEngine::UUID>{}(k.shaderCollectionId);
		std::size_t h2 = std::hash<uint32_t>{}(k.materialFlags);
		return h1 ^ (h2 << 1);
	}
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

	std::vector<VkCommandPool> commandPools;
	std::vector<VkCommandBuffer> commandBuffers;

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

	std::vector<VkSemaphore> imageAvailableSemaphores;
	std::vector<VkSemaphore> renderFinishedSemaphores;
	std::vector<VkSemaphore> uiSemaphores;
	std::vector<VkFence> inFlightFences;

	VmaAllocator allocator = VK_NULL_HANDLE;
	WindowSystem* windowSystem = nullptr;

	uint32_t currentFrame = 0;
	bool framebufferResized = false;
	int MAX_FRAMES_IN_FLIGHT = 2;
	bool initialized = false;
	bool editorMode = false;

	VkPhysicalDeviceVulkan12Features vulkan12Features{};
	VkPhysicalDeviceVulkan13Features vulkan13Features{};

	// Bindless Resources
	static constexpr uint32_t MAX_BINDLESS_TEXTURES = 1024;
	static constexpr uint32_t MAX_BINDLESS_MATERIALS = 1024;
	VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool globalDescriptorPool = VK_NULL_HANDLE;
	VkDescriptorSet globalDescriptorSet = VK_NULL_HANDLE;
	
	VulkanBuffer globalMaterialSSBO;

	std::vector<uint32_t> freeTextureIndices;
	std::unordered_map<TerraEngine::UUID, uint32_t> textureIndexMap;
	uint32_t nextTextureIndex = 0;

	std::vector<uint32_t> freeMaterialIndices;
	std::unordered_map<TerraEngine::UUID, uint32_t> materialIndexMap;
	uint32_t nextMaterialIndex = 0;

	bool CreateGlobalDescriptorResources();
	void CreateMaterialBufferResources();

	uint32_t AllocateTextureIndex();
	void UpdateGlobalTexture(uint32_t index, const VulkanImage& image);

	uint32_t AllocateMaterialIndex();

	// Editor / Offscreen Resources
    struct PushConstants {
	    alignas(16) glm::mat4 viewProj;
        alignas(16) glm::mat4 model;
	    uint32_t textureIndex;
	    uint32_t materialIndex;
        uint32_t padding[2];
    };

    struct OffscreenResources {
	    VkImage image = VK_NULL_HANDLE;
	    VmaAllocation allocation = VK_NULL_HANDLE;
	    VkImageView view = VK_NULL_HANDLE;
	    VkSampler sampler = VK_NULL_HANDLE;
	    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    };
    OffscreenResources sceneView;
    OffscreenResources gameView;

	VkDescriptorSet offscreenDescriptorSet = VK_NULL_HANDLE;
	VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;

	std::unordered_map<TerraEngine::UUID, VulkanMesh> meshCache;
	std::unordered_map<TerraEngine::UUID, VulkanImage> textureCache;
	// Pipelines keyed by RenderPass then PipelineKey (ShaderID + Flags)
	std::unordered_map<VkRenderPass, std::unordered_map<PipelineKey, VulkanPipeline, PipelineKeyHash>> pipelineCache;

	VulkanImage defaultTexture;
	std::vector<std::vector<VkCommandPool>> workerCommandPools; // Per-frame command pools for worker threads
	std::vector<std::vector<std::vector<VkCommandBuffer>>> workerCommandBuffers; // Per-frame, per-worker secondary command buffers
	// Track additional primary command buffers allocated when rendering multiple
	// cameras per frame so they can be freed after the frame fence completes.
	std::vector<std::vector<VkCommandBuffer>> extraPrimaryCommandBuffers;
	std::vector<std::vector<VkCommandBuffer>> pendingPrimaryCommandBuffers;
	std::vector<std::vector<size_t>> workerBufferUsage; // Tracks next available secondary buffer index per worker per frame
	bool swapchainImageAcquired = false;
	bool skipFrame = false;
	uint32_t acquiredImageIndex = 0;

	bool InitVulkan(WindowSystem& windowSystem);
	void Cleanup();

	void InitEditorMode();
	void CreateOffscreenResources(OffscreenResources& res);
	void RecreateOffscreenResources(); // Handle resize

    // Grid Renderer
    VkPipeline sceneViewGridPipeline = VK_NULL_HANDLE;
    VkPipelineLayout sceneViewGridPipelineLayout = VK_NULL_HANDLE;
    
    struct GridPushConstants {
        glm::mat4 view;
        glm::mat4 proj;
        glm::mat4 inverseView;
        glm::mat4 inverseProj;
    };

    void CreateSceneViewGridPipeline(VkRenderPass renderPass);
    void RenderSceneViewGrid(VkCommandBuffer cmd, const FrameRenderData& frameData);

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
	void InvalidateMaterial(TerraEngine::UUID materialId);
	void InvalidateMesh(TerraEngine::UUID meshId);
	void InvalidateTexture(TerraEngine::UUID textureId);
	void EnsureMesh(TerraEngine::UUID meshId);
	void EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass, uint32_t materialFlags);

	VkDescriptorType MapDescriptorType(DescriptorType type);
	VkShaderStageFlags StageVisibilityToVulkan(StageVisibilityMask visibility);
	void DestroyPipelines();
	bool BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants);
	void EnsureWorkerPools(uint32_t frameIndex, size_t workerCount);
	VkCommandBuffer AcquireWorkerCommandBuffer(uint32_t frameIndex, size_t workerIndex);
	void DestroyWorkerPools();

	bool CheckValidationLayerSupport();
	std::vector<const char*> GetRequiredExtensions();
	bool IsDeviceSuitable(VkPhysicalDevice device);
	int RateDeviceSuitability(VkPhysicalDevice device);
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
