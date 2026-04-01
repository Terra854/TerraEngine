#pragma once

#include "VulkanContext.h"
#include "VulkanCommonIncludes.h"
#include "VulkanHiZ.h"
#include "VulkanVRS.h"

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
#include <mutex>
#include <limits>
#include <stdexcept>
#include <filesystem>

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
	VK_KHR_MAINTENANCE3_EXTENSION_NAME,
	VK_EXT_MESH_SHADER_EXTENSION_NAME,
	VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
	VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME
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
	VulkanBuffer meshShaderVertexBuffer;
	VulkanBuffer meshletVertexRemapBuffer;
	VulkanBuffer meshSkinningBuffer;
	VulkanBuffer meshDecodeInfoBuffer;
	VulkanBuffer indexBuffer;
	VulkanBuffer meshletBuffer;
	VulkanBuffer meshletBoundsBuffer;
	uint32_t indexCount;
	VkIndexType indexType;
	uint32_t meshletCount;
	bool hasMeshletBounds = false;
	uint32_t bindlessIndex = 0xFFFFFFFFu;
	std::unordered_map<VkDescriptorSetLayout, VkDescriptorSet> descriptorSetsByLayout;
};

// Pipeline pairing with its layout to avoid redundant lookups.
struct VulkanPipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
	std::vector<VkDescriptorSetLayout> setLayouts;
	std::vector<std::vector<DescriptorBinding>> bindingsPerSet;
	std::vector<VkPushConstantRange> pushConstants;
	bool supportsDefaultPushConstants = false;
	VkShaderStageFlags defaultPushConstantStages = 0;
	bool hasTaskStage = false;
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
	uint32_t ShadingModel;
	uint32_t Flags;
	uint32_t BaseColorTextureIndex;
	uint32_t NormalTextureIndex;
	uint32_t MaskTextureIndex;
	uint32_t OcclusionTextureIndex;
	uint32_t EmissiveTextureIndex;
	uint32_t Padding[1];
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
	static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
	bool initialized = false;
	bool editorMode = false;

	VkPhysicalDeviceVulkan12Features vulkan12Features{};
	VkPhysicalDeviceVulkan13Features vulkan13Features{};
	VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrsFeatures{};
	bool meshShaderSupported = false;
	bool taskShaderSupported = false;

	// Bindless Resources
	static constexpr uint32_t MAX_BINDLESS_TEXTURES = 1024;
	static constexpr uint32_t MAX_BINDLESS_BUFFERS = 1024;
	static constexpr uint32_t MAX_BINDLESS_MATERIALS = 1024;
	static constexpr uint32_t MAX_BINDLESS_DRAWS = 4096;
	static constexpr uint32_t MAX_GLOBAL_LIGHTS = 1024;
	static constexpr uint32_t MAX_LIGHT_CLUSTERS = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
	static constexpr uint32_t MAX_LIGHT_INDEX_ENTRIES = MAX_LIGHT_CLUSTERS * MAX_GLOBAL_LIGHTS;
	static constexpr uint32_t MAX_SHADOW_RESOURCE_SLOTS = 4u;
	static constexpr uint32_t SHADOW_MAP_RESOLUTION = 1024u;
	static constexpr uint32_t POINT_SHADOW_MAP_RESOLUTION = 1024u;
	static constexpr uint32_t LTC_LUT_RESOLUTION = 64u;
	VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool globalDescriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> globalDescriptorSets;
	
	std::vector<VulkanBuffer> globalMaterialSSBOs;
	std::vector<VulkanBuffer> globalFrameCullingSSBOs;
	std::vector<VulkanBuffer> globalMeshletCountersSSBOs;
	std::vector<VulkanBuffer> globalLightSSBOs;
	std::vector<VulkanBuffer> lightAnalysisInputSSBOs;
		std::vector<VulkanBuffer> clusterGridSSBOs;
		std::vector<VulkanBuffer> lightIndexSSBOs;
		std::vector<VulkanBuffer> clusterDepthSSBOs;
	std::vector<VulkanBuffer> lightAnalysisSSBOs;
				std::vector<VulkanBuffer> indirectDrawBuffers;
				VulkanHiZ hiZ;
				VulkanVRS vrs;
				
				VulkanImage directionalShadowDepth;
						VulkanImage directionalShadowColor;
	VkFramebuffer directionalShadowFramebuffer = VK_NULL_HANDLE;
	uint32_t directionalShadowTextureIndex = 0u;
	VulkanImage spotShadowDepth;
	VulkanImage spotShadowColor;
	VkFramebuffer spotShadowFramebuffer = VK_NULL_HANDLE;
	uint32_t spotShadowTextureIndex = 0u;
	VulkanImage areaShadowDepth;
	VulkanImage areaShadowColor;
	VkFramebuffer areaShadowFramebuffer = VK_NULL_HANDLE;
	uint32_t areaShadowTextureIndex = 0u;
	VulkanImage pointShadowCubeDepth;
	VulkanImage pointShadowCubeColor;
	std::array<VkImageView, 6> pointShadowDepthFaceViews{};
	std::array<VkImageView, 6> pointShadowColorFaceViews{};
	std::array<VkFramebuffer, 6> pointShadowFaceFramebuffers{};
	VulkanImage ltcAmplitudeTexture;
	VulkanImage ltcInverseTexture;
	uint32_t ltcAmplitudeTextureIndex = 0u;
	uint32_t ltcInverseTextureIndex = 0u;
	VkRenderPass shadowRenderPass = VK_NULL_HANDLE;
	VkPipelineLayout lightingComputePipelineLayout = VK_NULL_HANDLE;
	VkPipeline clusterGridComputePipeline = VK_NULL_HANDLE;
	VkPipeline clusterDepthMaskComputePipeline = VK_NULL_HANDLE;
	VkPipeline clusterLightCountComputePipeline = VK_NULL_HANDLE;
	VkPipeline clusterLightPrefixComputePipeline = VK_NULL_HANDLE;
	VkPipeline clusterLightFillComputePipeline = VK_NULL_HANDLE;
	VkPipeline lightAnalysisComputePipeline = VK_NULL_HANDLE;
	VkPipeline hiZComputePipeline = VK_NULL_HANDLE;
	VkPipeline vrsComputePipeline = VK_NULL_HANDLE;
	VkPipelineLayout hiZPipelineLayout = VK_NULL_HANDLE;
	VkPipelineLayout vrsPipelineLayout = VK_NULL_HANDLE;

	struct GpuLightAnalysisViewSnapshot
	{
		std::unordered_map<TerraEngine::UUID, GPULightAnalysisRecord> recordsByLightId;
		std::uint32_t visibleCandidateCount = 0u;
		bool valid = false;
	};

	struct TemporalLightSelectionState
	{
		float lastCompositeScore = 0.0f;
		float lastGpuVisibilityScore = 0.0f;
		float lastScreenInfluence = 0.0f;
		std::uint64_t lastSeenFrameSerial = 0u;
		bool retainedLastFrame = false;
		bool shadowedLastFrame = false;
	};

	struct ViewLightingTemporalState
	{
		std::unordered_map<TerraEngine::UUID, TemporalLightSelectionState> lights;
		std::array<TerraEngine::UUID, MAX_SHADOW_RESOURCE_SLOTS> previousShadowOwners{};
		glm::vec3 lastCameraWorldPosition = glm::vec3(0.0f);
		glm::mat4 lastViewMatrix = glm::mat4(1.0f);
		glm::mat4 lastProjectionMatrix = glm::mat4(1.0f);
		std::uint64_t frameSerial = 0u;
		bool valid = false;
	};

	struct LightBudgetApplicationResult
	{
		std::vector<FrameLightCandidate> uploadedCandidates;
		std::vector<FrameLightCandidate> retainedLights;
		FrameBudgetDiagnosticsSnapshot diagnostics;
	};

	static constexpr uint32_t kStatsViewCount = 2u;
	static constexpr uint32_t kMaxStatsFrameSlots = 8u;
	static constexpr uint32_t kStatsSlotCount = kStatsViewCount * kMaxStatsFrameSlots;
	std::array<RenderStatsSnapshot, kStatsViewCount> latestStatsByView{};
	std::array<RenderStatsSnapshot, kStatsSlotCount> pendingStatsBySlot{};
	std::array<FrameBudgetDiagnosticsSnapshot, kStatsViewCount> latestBudgetDiagnosticsByView{};
	std::array<FrameBudgetDiagnosticsSnapshot, kStatsSlotCount> pendingBudgetDiagnosticsBySlot{};
	std::array<std::vector<FrameLightCandidate>, kStatsSlotCount> pendingLightAnalysisCandidatesBySlot{};
	std::array<GpuLightAnalysisViewSnapshot, kStatsViewCount> latestGpuLightAnalysisByView{};
	std::array<ViewLightingTemporalState, kStatsViewCount> lightingTemporalStateByView{};
	std::array<ClusterValidationSnapshot, MAX_FRAMES_IN_FLIGHT> pendingClusterValidationByFrame{};
	ClusterValidationSnapshot previousClusterValidationSnapshot{};
	bool previousClusterValidationSnapshotValid = false;
	std::filesystem::path clusterValidationCapturePath = std::filesystem::path("Tools") / "LightingValidation" / "results" / "latest-cluster-validation.json";
	// Validation capture is opt-in because it keeps large lighting buffers
	// CPU-visible and reads them back after every frame, which is too
	// expensive for normal editor and game sessions.
	bool clusterValidationCaptureEnabled = false;
	bool globalTaskShaderCullingEnabled = true;
	mutable std::mutex statsMutex;

	std::vector<uint32_t> freeTextureIndices;
	std::unordered_map<TerraEngine::UUID, uint32_t> textureIndexMap;
	uint32_t nextTextureIndex = 0;

	std::vector<uint32_t> freeMaterialIndices;
	std::unordered_map<TerraEngine::UUID, uint32_t> materialIndexMap;
	uint32_t nextMaterialIndex = 0;

	std::vector<uint32_t> freeBufferIndices;
	uint32_t nextBufferIndex = 0;

	bool CreateGlobalDescriptorResources();
	void CreateMaterialBufferResources();
	void CreateCullingBufferResources();
		void CreateLightingBufferResources();
		void CreateIndirectDrawResources();
		bool EnsureShadowRenderResources();
		void UploadLightingData(const std::vector<GPULight>& lights);
		void UploadLightAnalysisInputData(const std::vector<GPULight>& lights);
			void GenerateHiZPyramid(VkCommandBuffer cmd, VkImageView depthView);
		void GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView);
	
		void ResetMeshletCounters(uint32_t statsSlotIndex);
	
	void UpdateFrameCullingData(uint32_t statsSlotIndex, const FrameCullingDataGPU& frameData);
	LightBudgetApplicationResult ApplyFrameLightBudgets(const FrameRenderData& frameData, RenderViewType viewType);
	MeshletCullingCountersGPU ReadMeshletCounters(uint32_t statsSlotIndex) const;
	uint32_t GetStatsSlotIndex(uint32_t frameIndex, VulkanContext::ViewType view) const;
	void RecordPendingStats(uint32_t statsSlotIndex, const RenderStatsSnapshot& snapshot);
	void RecordPendingBudgetDiagnostics(uint32_t statsSlotIndex, const FrameBudgetDiagnosticsSnapshot& snapshot);
	void RecordPendingLightAnalysisCandidates(uint32_t statsSlotIndex, const std::vector<FrameLightCandidate>& candidates);
	void PublishCompletedFrameStats(uint32_t frameIndex);
	RenderStatsSnapshot GetLatestStats(RenderViewType viewType) const;
	FrameBudgetDiagnosticsSnapshot GetLatestBudgetDiagnostics(RenderViewType viewType) const;
	GpuLightAnalysisViewSnapshot GetLatestGpuLightAnalysis(RenderViewType viewType) const;
	void RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot);
	ClusterValidationSnapshot ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const;
	void WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const;

	uint32_t AllocateTextureIndex();
	void UpdateGlobalTexture(uint32_t index, const VulkanImage& image);

	uint32_t AllocateBufferIndex();
	void UpdateGlobalBuffer(uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
	void UpdateGlobalHiZ(const VulkanHiZ& hiZ);

	uint32_t AllocateMaterialIndex();

	// Editor / Offscreen Resources
    struct PushConstants {
	    alignas(16) glm::mat4 viewProj;
        alignas(16) glm::mat4 model;
	    uint32_t textureIndex;
	    uint32_t materialIndex;
		uint32_t taskCullingEnabled;
		uint32_t statsSlotIndex;
		float cullingEpsilon;
		float padding[3];
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
	VulkanImage defaultNormalTexture;
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
    void ClearResources();

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
	VulkanBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, const char* name = nullptr);
	void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	VkCommandBuffer BeginSingleTimeCommands();
	void EndSingleTimeCommands(VkCommandBuffer commandBuffer);
	void TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
	void CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
	VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);

	VkFormat MapTextureFormat(uint32_t vkFormat);
	void EnsureTexture(TerraEngine::UUID textureId);
	std::vector<VkDescriptorSet> EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId);
	void InvalidateMaterial(TerraEngine::UUID materialId);
	void InvalidateMesh(TerraEngine::UUID meshId);
	void InvalidateTexture(TerraEngine::UUID textureId);
	void InvalidateResource(TerraEngine::UUID id);
	void EnsureMesh(TerraEngine::UUID meshId);
	void EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass, uint32_t materialFlags);
	VkDescriptorSet EnsureMeshDescriptorSet(TerraEngine::UUID meshId, const VulkanPipeline& pipeline);

	VkDescriptorType MapDescriptorType(DescriptorType type);
	VkShaderStageFlags StageVisibilityToVulkan(StageVisibilityMask visibility);
	void DestroyPipelines();
	bool BuildPipelineLayouts(const ShaderCollection& collection, std::vector<VkDescriptorSetLayout>& setLayouts, std::vector<std::vector<DescriptorBinding>>& bindingsPerSet, std::vector<VkPushConstantRange>& pushConstants, bool& supportsDefaultPushConstants);
	void EnsureWorkerPools(uint32_t frameIndex, size_t workerCount);
	VkCommandBuffer AcquireWorkerCommandBuffer(uint32_t frameIndex, size_t workerIndex);
	VkCommandBuffer AcquirePrimaryCommandBuffer(uint32_t frameIndex);
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
