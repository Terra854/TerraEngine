// VulkanContextImpl.h — Vulkan Backend Implementation Details
//
// Defines the private implementation state for the VulkanContext, encapsulating internal handles and caches.
// It includes definitions for descriptor layouts, pipeline keys, and frame-specific synchronization objects.
// This header serves as the internal bridge between the public VulkanContext API and the detailed Vulkan backend logic.

#pragma once

#include "VulkanContext.h"
#include "VulkanCommonIncludes.h"
#include "VulkanHiZ.h"
#include "VulkanVRS.h"
#include "VulkanUploadPlanning.h"
#include "VulkanDescriptorPlanning.h"
#include "VulkanCommandRecording.h"
#include "VulkanVirtualGeometryExecutor.h"
#include "VulkanFrameDiagnostics.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryVisibility.h"

#include "Core/Logger/Logger.h"
#include "Window/WindowSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryPagePublication.h"
#include "Resource/ShaderCollection.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineTextureFile.h"
#include "File Formats/TerraEngineShaderFile.h"
#include "Scene/SceneManager.h"
#include "UI/UIRenderer.h"

#include <vector>
#include <set>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <map>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <filesystem>
#include <string_view>
#include <chrono>
#include <compare>

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
// struct QueueFamilyIndices groups related state and operations so callers have one documented contract.
struct QueueFamilyIndices {
	std::optional<uint32_t> graphicsFamily;
	std::optional<uint32_t> presentFamily;

	// isComplete is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool isComplete() {
		return graphicsFamily.has_value() && presentFamily.has_value();
	}
};

// Swap chain capabilities returned by physical devices.
// struct SwapChainSupportDetails groups related state and operations so callers have one documented contract.
struct SwapChainSupportDetails {
	VkSurfaceCapabilitiesKHR capabilities{};
	std::vector<VkSurfaceFormatKHR> formats;
	std::vector<VkPresentModeKHR> presentModes;
	// A failed surface query must never be mistaken for an empty but usable
	// capability set by candidate construction.
	bool valid = false;
};

// GPU buffer wrapper that keeps the VMA allocation alongside the VkBuffer.
// struct VulkanBuffer groups related state and operations so callers have one documented contract.
struct VulkanBuffer {
	VkBuffer buffer;
	VmaAllocation allocation;
	VkDeviceSize size;
};

// Conventional graphics shaders read this one 64-byte record through global
// set-0/binding-9. meshDataIndex selects its bindless buffer and textureIndex
// selects one work-packet record, avoiding any change to the frozen mesh push
// constants while retaining immutable animation-generation identity.
struct alignas(16) VulkanGpuAnimationGraphicsRecord {
	std::uint32_t planGeneration = 0U;
	std::uint32_t rendererGeneration = 0U;
	std::uint32_t workPacketIndex = 0U;
	std::uint32_t generationRecordIndex = 0U;
	std::uint32_t paletteMatricesBindlessIndex = std::numeric_limits<std::uint32_t>::max();
	std::uint32_t morphOutputsBindlessIndex = std::numeric_limits<std::uint32_t>::max();
	std::uint64_t paletteOffsetBytes = 0U;
	// This remains the byte offset frozen in GpuAnimationWorkPacket, rather
	// than a float index, so native/HLSL addressing cannot drift.
	std::uint64_t morphOutputOffsetBytes = 0U;
	std::uint32_t jointCount = 0U;
	std::uint32_t morphCount = 0U;
	std::uint32_t paletteEntryCount = 0U;
	std::uint32_t flags = 0U;
	std::uint32_t stageMask = 0U;
	std::uint32_t validMarker = 0U;
};
static_assert(sizeof(VulkanGpuAnimationGraphicsRecord) == 64U,
	"Animation graphics bridge records must remain 64-byte shader ABI values.");

// GPU image wrapper for textures and attachments.
// struct VulkanImage groups related state and operations so callers have one documented contract.
struct VulkanImage {
	VkImage image;
	VmaAllocation allocation;
	VkImageView view;
	VkSampler sampler;
	VkDeviceSize size = 0;
	std::uint32_t sourceMipCount = 1u;
	std::uint32_t residentMipCount = 1u;
	std::uint32_t residentMipOffset = 0u;
};

// Mesh buffers cached on the GPU.
// struct VulkanMesh groups related state and operations so callers have one documented contract.
struct VulkanMesh {
	VulkanBuffer meshShaderVertexBuffer;
	VulkanBuffer meshletVertexRemapBuffer;
	// Binding 6 always receives this buffer. Non-skinned meshes own one
	// zero-filled skinning record so statically used default shader layouts
	// never observe an unwritten storage-buffer descriptor.
	VulkanBuffer meshSkinningBuffer;
	// Target-major float4 position/normal/tangent deltas. Binding 7 always
	// receives this buffer, including its one-record zero fallback for meshes
	// without morph data, so reflected default layouts never bind VK_NULL_HANDLE.
	VulkanBuffer meshMorphDeltaBuffer;
	VulkanBuffer meshDecodeInfoBuffer;
	VulkanBuffer indexBuffer;
	VulkanBuffer meshletBuffer;
	VulkanBuffer meshletSectionBuffer;
	VulkanBuffer meshletBoundsBuffer;
	uint32_t indexCount;
	VkIndexType indexType;
	uint32_t meshletCount;
	bool hasMeshletBounds = false;
	uint32_t bindlessIndex = 0xFFFFFFFFu;
	std::unordered_map<VkDescriptorSetLayout, VkDescriptorSet> descriptorSetsByLayout;
};

// Pipeline pairing with its layout to avoid redundant lookups.
// struct VulkanPipeline groups related state and operations so callers have one documented contract.
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
// struct alignas groups related state and operations so callers have one documented contract.
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
	uint32_t ShadowBehavior;
};

// struct PipelineKey groups related state and operations so callers have one documented contract.
struct PipelineKey {
	TerraEngine::UUID shaderCollectionId;
	uint32_t materialFlags;
	std::uint64_t collectionGeneration = 0u;
	std::uint64_t shaderModuleGeneration = 0u;
	std::uint32_t geometryAbiVersion = 0u;
	std::uint32_t geometryPolicy = 0u;
	// Only the default conventional surface pipeline can exchange its ordinary
	// fragment stage for the Vulkan-only identity-output variant.
	bool usesConventionalVisibilityIdentity = false;

	bool operator==(const PipelineKey& other) const {
		return shaderCollectionId == other.shaderCollectionId
			&& materialFlags == other.materialFlags
			&& collectionGeneration == other.collectionGeneration
			&& shaderModuleGeneration == other.shaderModuleGeneration
			&& geometryAbiVersion == other.geometryAbiVersion
			&& geometryPolicy == other.geometryPolicy
			&& usesConventionalVisibilityIdentity == other.usesConventionalVisibilityIdentity;
	}
};

// struct PipelineKeyHash groups related state and operations so callers have one documented contract.
struct PipelineKeyHash {
	std::size_t operator()(const PipelineKey& k) const {
		std::size_t h1 = std::hash<TerraEngine::UUID>{}(k.shaderCollectionId);
		std::size_t h2 = std::hash<uint32_t>{}(k.materialFlags);
		std::size_t hash = h1 ^ (h2 << 1);
		hash ^= std::hash<std::uint64_t>{}(k.collectionGeneration) << 2u;
		hash ^= std::hash<std::uint64_t>{}(k.shaderModuleGeneration) << 3u;
		hash ^= std::hash<std::uint32_t>{}(k.geometryAbiVersion) << 4u;
		hash ^= std::hash<std::uint32_t>{}(k.geometryPolicy) << 5u;
		hash ^= std::hash<bool>{}(k.usesConventionalVisibilityIdentity) << 6u;
		return hash;
	}
};

// struct VulkanContext groups related state and operations so callers have one documented contract.
struct VulkanContext::Impl {
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
	// Validation is requested by project/export settings before instance
	// creation. Missing SDK layers only disable this flag at startup; they must
	// never keep the engine from booting.
	bool validationLayersRequested = false;
	bool validationLayersEnabled = false;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	// Cached once during physical-device selection so per-frame animation
	// publication can reject oversized storage-buffer descriptors without a
	// driver query on the render hot path.
	VkDeviceSize maximumStorageBufferRange = 0U;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	VkQueue presentQueue = VK_NULL_HANDLE;
	// Every public operation that reads or mutates live Vulkan ownership holds
	// this recursive gate. Renderer methods call one another while preserving the
	// same protected boundary, so recursion is intentional; external resource
	// invalidation and editor callbacks wait until the complete transition ends.
	mutable std::recursive_mutex deviceStateMutex;
	// Vulkan queue handles are externally synchronized objects. Asset uploads
	// can submit from worker threads while the coordinator submits/presents an
	// editor frame, so every queue operation and device-wide idle boundary must
	// share this mutex instead of using unrelated per-feature locks.
	std::mutex queueAccessMutex;
	// Vulkan descriptor sets and their pools require external host
	// synchronization. Project-open asset workers can publish textures while the
	// render thread updates depth/Hi-Z bindings, so serialize the whole mutation
	// transaction rather than only individual vkUpdateDescriptorSets calls.
	std::recursive_mutex descriptorHostAccessMutex;
	// These strings are copied into recovery snapshots when the backend first
	// fails. Keeping them CPU-side means diagnostics can still name the GPU
	// after Vulkan handles have become unsafe to query.
	std::string selectedPhysicalDeviceName;
	std::string selectedDriverVersionString;
	std::string selectedApiVersionString;

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

	// A conventional identity attachment stores integer producer data beside the
	// primary color output. It is not sampled or read back in this slice; owning
	// it separately keeps the future readback path independent from presentation.
	struct ConventionalVisibilityIdentityAttachment
	{
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
	};
	std::vector<ConventionalVisibilityIdentityAttachment> presentationVisibilityIdentities;

	std::vector<VkCommandPool> commandPools;
	std::vector<VkCommandBuffer> commandBuffers;
	// Editor UI recording uses pools that are independent from scene rendering.
	// Project-hub frames have no camera and therefore never enter the renderer's
	// BeginFrame cleanup, so sharing its pools would retain one UI command buffer
	// on every frame until a scene render eventually occurred.
	std::vector<VkCommandPool> editorCommandPools;
	std::vector<VkCommandBuffer> editorCommandBuffers;
	VkCommandPool immediateCommandPool = VK_NULL_HANDLE;
	std::mutex immediateCommandMutex;

	VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
	// ImGui owns and frees viewport texture descriptors independently from mesh
	// resources. A dedicated pool prevents editor resize churn from contending
	// with asset-loading descriptor allocations on worker/coordinator paths.
	VkDescriptorPool editorDescriptorPool = VK_NULL_HANDLE;

	std::vector<VkSemaphore> imageAvailableSemaphores;
	// renderFinishedSemaphores are frame-owned offscreen dependencies consumed
	// by the editor UI submit. They are never used directly for presentation.
	std::vector<VkSemaphore> renderFinishedSemaphores;
	// uiSemaphores are present-ready semaphores owned by swapchain image index,
	// so this collection always matches swapchainImages rather than frame slots.
	std::vector<VkSemaphore> uiSemaphores;
	// Each queued present owns a maintenance fence until the presentation engine
	// releases its wait semaphore. Signaled fences move into the reusable pool;
	// pending fences move with a retired swapchain and gate its destruction.
	std::vector<VkFence> presentationCompletionFences;
	std::vector<VkFence> availablePresentationCompletionFences;
	std::vector<VkFence> inFlightFences;
	// PresentationResourceBundle is movable ownership used during recreation.
	// Candidate construction can fail without destroying the last active bundle.
	struct PresentationResourceBundle
	{
		VkSwapchainKHR swapchain = VK_NULL_HANDLE;
		std::vector<VkImage> images;
		VkFormat imageFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
		std::vector<VkImageView> imageViews;
		VkRenderPass renderPass = VK_NULL_HANDLE;
		std::vector<VkFramebuffer> framebuffers;
		VkImage depthImage = VK_NULL_HANDLE;
		VmaAllocation depthAllocation = VK_NULL_HANDLE;
		VkImageView depthView = VK_NULL_HANDLE;
		std::vector<ConventionalVisibilityIdentityAttachment> visibilityIdentities;
		std::vector<VkSemaphore> acquireByFrame;
		std::vector<VkSemaphore> editorOffscreenReadyByFrame;
		std::vector<VkSemaphore> presentReadyByImage;
		// Pending fences prove completion of every queued present. Reusable signaled
		// fences are retained too so candidate publication remains allocation-bounded.
		std::vector<VkFence> presentCompletionFences;
		std::vector<VkFence> availablePresentCompletionFences;
		// This proof is valid only when every present queued for this bundle carried
		// a maintenance fence. A preparation failure makes retirement fail closed.
		bool presentCompletionCoverageValid = false;
		std::vector<VkFence> frameFences;
	};
	struct RetiredPresentationResourceBundle
	{
		PresentationResourceBundle resources;
		std::uint64_t generation = 0u;
	};
	TerraEngine::Renderer::Vulkan::PresentationLifecycle presentationLifecycle;
	TerraEngine::Renderer::Vulkan::PresentationRecreationCoordinator presentationRecreation;
	TerraEngine::Renderer::Vulkan::EditorFrameCompletionTracker editorCompletionTracker{3u};
	// The installed-editor harness waits for this one-time marker instead of
	// inferring readiness from resize callbacks or an acquire result.
	std::uint64_t editorPresentationReadyLoggedGeneration = 0u;
	std::optional<TerraEngine::Renderer::Vulkan::AcquiredPresentationImage> acquiredPresentationImage;
	std::uint32_t surfaceMinimumImageCount = 0;
	std::uint32_t requestedSwapchainImageCount = 0;
	std::deque<RetiredPresentationResourceBundle> retiredPresentationBundles;
	static constexpr std::size_t MAX_RETIRED_PRESENTATION_BUNDLES = 2u;
	struct PendingUnextendedAcquireProof
	{
		VkFence fence = VK_NULL_HANDLE;
		std::uint64_t generation = 0u;
		std::uint32_t imageIndex = 0u;
	};
	// Older Vulkan implementations expose no present fence. A later completed
	// acquire of the same image is the portable proof that its prior present ended.
	TerraEngine::Renderer::Vulkan::UnextendedPresentationRetirementTracker unextendedRetirementTracker;
	std::vector<PendingUnextendedAcquireProof> pendingUnextendedAcquireProofs;
	// The extension is optional. Without it, retirement is intentionally
	// bounded while the acquire-history fallback waits for explicit proof.
	bool presentationFencesEnabled = false;
	// Device-side swapchain maintenance must use the same KHR or EXT lineage as
	// the optional surface-maintenance instance extension enabled at startup.
	const char* surfaceMaintenanceExtensionName = nullptr;
	// The extension can be available while one generation loses coverage because
	// allocating or resetting its next fence failed. Track that distinction so a
	// partially fenced generation is never destroyed as though all presents were proven.
	bool presentationCompletionCoverageValid = false;

	VmaAllocator allocator = VK_NULL_HANDLE;
	WindowSystem* windowSystem = nullptr;

	uint32_t currentFrame = 0;
	bool framebufferResized = false;
	bool ClaimFramebufferResizeRebuild()
	{
		// Poll retirement fences at the protected frame boundary. A failed old-
		// swapchain handoff needs every retired bundle gone; a full backlog needs
		// only one free slot before publishing one new retry revision.
		CollectCompletedPresentationResourceBundles();
		if (presentationRecreation.IsWaitingForRetirementProof())
		{
			const bool proofReady = presentationRecreation.OldSwapchainWasConsumed()
				? retiredPresentationBundles.empty()
				: retiredPresentationBundles.size() < MAX_RETIRED_PRESENTATION_BUNDLES;
			if (proofReady) {
				const bool retryPublished = presentationRecreation.RequestRetryAfterRetirement();
				(void)retryPublished;
			}
		}
		// The coordinator coalesces duplicate callbacks by revision. Retain this
		// method as the production boundary used by editor presentation callers.
		return presentationRecreation.Claim().has_value();
	}
	void RecordFramebufferResize(int width, int height)
	{
		const auto safeWidth = static_cast<std::uint32_t>(std::max(width, 0));
		const auto safeHeight = static_cast<std::uint32_t>(std::max(height, 0));
		presentationRecreation.RequestFramebufferExtent({ safeWidth, safeHeight });
		framebufferResized = presentationRecreation.IsRecoveryPending();
	}
	void RequestOutOfDateRecovery()
	{
		presentationRecreation.RequestOutOfDate();
		framebufferResized = true;
	}
	bool RequestSuboptimalRecoveryIfNeeded()
	{
		const bool requested = presentationRecreation.RequestSuboptimalIfExtentChanged();
		framebufferResized = framebufferResized || requested;
		return requested;
	}
	void PublishPresentationRecreationSuccess()
	{
		presentationRecreation.PublishSuccess({ swapchainExtent.width, swapchainExtent.height });
		bool needsUnextendedProof = false;
		for (const RetiredPresentationResourceBundle& retired : retiredPresentationBundles) {
			if (!retired.resources.presentCompletionCoverageValid) {
				needsUnextendedProof = true;
				break;
			}
		}
		// A newer chain supersedes any unreachable anchor from its predecessor. The
		// first present of this generation becomes the release anchor for all old bundles.
		unextendedRetirementTracker.BeginGeneration(
			presentationLifecycle.Generation(), needsUnextendedProof);
		framebufferResized = false;
	}
	static constexpr int MAX_FRAMES_IN_FLIGHT = 3;
	bool initialized = false;
	bool editorMode = false;

	VkPhysicalDeviceVulkan12Features vulkan12Features{};
	VkPhysicalDeviceVulkan13Features vulkan13Features{};
	VkPhysicalDeviceMeshShaderFeaturesEXT meshShaderFeatures{};
	VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrsFeatures{};
	bool meshShaderSupported = false;
	bool taskShaderSupported = false;
	bool vrsAttachmentRuntimeEnabled = false;

	// UsesVrsAttachment is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool UsesVrsAttachment() const
	{
		// VRS attachments are only safe when the runtime profile, device feature
		// query, and attachment image all agree. Editor scene/game views render
		// through offscreen ImGui textures, so they intentionally keep VRS off to
		// avoid mixing shading-rate tiles with docked viewport presentation.
		return vrsAttachmentRuntimeEnabled
			&& !editorMode
			&& vrsFeatures.attachmentFragmentShadingRate
			&& vrsFeatures.pipelineFragmentShadingRate
			&& vrs.view != VK_NULL_HANDLE;
	}

	// Bindless Resources
	static constexpr uint32_t MAX_BINDLESS_TEXTURES = 1024;
	static constexpr uint32_t MAX_BINDLESS_BUFFERS = 1024;
	static constexpr uint32_t MAX_BINDLESS_MATERIALS = 1024;
	static constexpr uint32_t INVALID_BINDLESS_BUFFER_INDEX = std::numeric_limits<std::uint32_t>::max();
	static constexpr uint32_t MAX_BINDLESS_DRAWS = 4096;
	static constexpr uint32_t MAX_GLOBAL_LIGHTS = 1024;
	static constexpr uint32_t MAX_LIGHT_CLUSTERS = kLightingClusterDimX * kLightingClusterDimY * kLightingClusterDimZ;
	static constexpr uint32_t MAX_LIGHT_INDEX_ENTRIES = MAX_LIGHT_CLUSTERS * MAX_GLOBAL_LIGHTS;
	// Each queued view gets an independent slice of the mutable per-pass
	// buffers. The fixed cap bounds VRAM use while covering editor scene/game
	// rendering plus several future offscreen camera targets.
	static constexpr uint32_t MAX_RENDER_SNAPSHOTS_PER_FRAME = 8u;
	// VkImage extents use 32-bit dimensions, so no valid 2D image can require
	// more than 32 pyramid levels. Reserving that fixed stride prevents a Scene
	// View with one mip count from rebuilding descriptor storage still referenced
	// by an unsubmitted Game View command buffer (or vice versa).
	static constexpr uint32_t MAX_HIZ_DESCRIPTOR_MIP_LEVELS = std::numeric_limits<uint32_t>::digits;
	static constexpr uint32_t MAX_SHADOW_RESOURCE_SLOTS = kMaxShadowResourceSlots;
	static constexpr uint32_t SHADOW_MAP_RESOLUTION = 1024u;
	static constexpr uint32_t POINT_SHADOW_MAP_RESOLUTION = 1024u;
	static constexpr uint32_t LTC_LUT_RESOLUTION = 64u;
	static constexpr VkFormat AMBIENT_OCCLUSION_FORMAT = VK_FORMAT_R32_SFLOAT;

	// struct ShadowResourceRequest groups related state and operations so callers have one documented contract.
	struct ShadowResourceRequest
	{
		std::uint32_t directionalCount = 0u;
		std::uint32_t spotCount = 0u;
		std::uint32_t areaCount = 0u;
		std::uint32_t pointCount = 0u;

		// Total2DCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		std::uint32_t Total2DCount() const
		{
			return directionalCount + spotCount + areaCount;
		}

		// TotalCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		std::uint32_t TotalCount() const
		{
			return Total2DCount() + pointCount;
		}

		bool operator==(const ShadowResourceRequest&) const = default;
	};

	// struct ShadowStorageRegion groups related state and operations so callers have one documented contract.
	struct ShadowStorageRegion
	{
		// A region describes one live GPU allocation slot or managed sub-region.
		// The VulkanImage arrays own the actual images; this metadata explains
		// which logical assignment is allowed to use that memory this frame.
		ShadowResourceKind resourceKind = ShadowResourceKind::Unsupported;
		std::uint32_t typedSlot = MAX_SHADOW_RESOURCE_SLOTS;
		std::uint32_t atlasId = 0u;
		std::uint32_t x = 0u;
		std::uint32_t y = 0u;
		std::uint32_t width = SHADOW_MAP_RESOLUTION;
		std::uint32_t height = SHADOW_MAP_RESOLUTION;
		ShadowQualityTier qualityTier = ShadowQualityTier::FullResolution;
		std::uint64_t estimatedBytes = 0u;
		bool live = false;
	};

	// ShadowCacheEntryState names the supported states explicitly so serialization and switches stay readable.
	enum class ShadowCacheEntryState : std::uint32_t
	{
		Invalid = 0u,
		Assigned = 1u,
		Cached = 2u,
		Refreshed = 3u,
		PendingRelease = 4u
	};

	// struct ShadowCacheEntry groups related state and operations so callers have one documented contract.
	struct ShadowCacheEntry
	{
		// Cache entries are intentionally conservative: missing or changed
		// signatures invalidate reuse and force a redraw instead of sampling a
		// stale shadow. GPU image lifetime remains owned by ShadowStorageRegion.
		TerraEngine::UUID lightId = TerraEngine::EMPTY_UUID;
		ShadowResourceKind resourceKind = ShadowResourceKind::Unsupported;
		std::uint32_t storageRegionId = MAX_SHADOW_RESOURCE_SLOTS;
		std::uint64_t lightSignature = 0u;
		std::uint64_t casterSignature = 0u;
		std::uint64_t viewSignature = 0u;
		std::uint64_t lastTouchedFrameSerial = 0u;
		ShadowCacheEntryState state = ShadowCacheEntryState::Invalid;
		bool valid = false;
	};

	VkDescriptorSetLayout globalDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool globalDescriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> globalDescriptorSets;
	VkDescriptorSetLayout hiZDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool hiZDescriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> hiZDescriptorSets;
	VkDescriptorSetLayout aoDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorPool aoDescriptorPool = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> aoDescriptorSets;
	VkSampler fallbackPointShadowSampler = VK_NULL_HANDLE;
	
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
				// GPU virtual geometry packet buffers are owned per frame slot
				// and per view. The public frame snapshot stores value records;
				// this private resource record owns the Vulkan handles and the
				// capacities needed to decide whether a later frame can reuse or
				// must retire an allocation.
				struct VirtualGeometryGpuPacketFrameResources
				{
					struct BindlessBufferPublication
					{
						VkBuffer buffer = VK_NULL_HANDLE;
						VkDeviceSize range = 0u;
					};

					std::uint64_t resourceSerial = 0u;
					std::uint64_t frameId = 0u;
					std::uint32_t viewId = 0u;
					RenderViewType viewType = RenderViewType::Game;
					VulkanBuffer packetBuffer{};
					VulkanBuffer traversalInstanceBuffer{};
					VulkanBuffer traversalRequestBuffer{};
					VulkanBuffer traversalCounterBuffer{};
					VulkanBuffer batchBuffer{};
					VulkanBuffer compactedPacketBuffer{};
					VulkanBuffer generatedSubmissionBuffer{};
					VulkanBuffer counterReadbackBuffer{};
					VulkanBuffer zeroWorkIndirectBuffer{};
					// Visibility compute uses frame-owned scratch records. These buffers
					// are intentionally separate from generated draw arguments so a
					// visibility failure cannot publish indirect execution authority.
					VulkanBuffer visibilityTriangleBuffer{};
					VulkanBuffer visibilityDepthBuffer{};
					VulkanBuffer visibilityRecordBuffer{};
					VulkanBuffer resolvedVisibilityBuffer{};
					VulkanBuffer shadingBinBuffer{};
					VulkanBuffer pagePayloadBuffer{};
					// This private set matches the compaction shader's four storage
					// bindings. It is not the global bindless set used by graphics.
					VkDescriptorSet executorDescriptorSet = VK_NULL_HANDLE;
					VkDescriptorSet visibilityComputeRasterDescriptorSet = VK_NULL_HANDLE;
					VkDescriptorSet visibilityResolveDescriptorSet = VK_NULL_HANDLE;
					VkDescriptorSet visibilityShadingBinsDescriptorSet = VK_NULL_HANDLE;
					BindlessBufferPublication packetPublication{};
					BindlessBufferPublication batchPublication{};
					BindlessBufferPublication compactedPacketPublication{};
					BindlessBufferPublication generatedSubmissionPublication{};
					BindlessBufferPublication counterReadbackPublication{};
					BindlessBufferPublication pagePayloadPublication{};
					std::uint32_t packetBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::uint32_t batchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::uint32_t compactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::uint32_t generatedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::uint32_t counterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::uint32_t pagePayloadBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
					std::size_t packetCapacity = 0u;
					std::size_t traversalInstanceCapacity = 0u;
					std::size_t traversalRequestCapacity = 0u;
					std::size_t batchCapacity = 0u;
					std::size_t compactedPacketCapacity = 0u;
					std::size_t generatedSubmissionCapacity = 0u;
					bool generatedSubmissionSupportsIndirect = false;
					std::size_t counterReadbackCapacity = 0u;
					std::size_t visibilityTriangleCapacity = 0u;
					std::size_t visibilityDepthCapacity = 0u;
					std::size_t visibilityRecordCapacity = 0u;
					std::size_t resolvedVisibilityCapacity = 0u;
					std::size_t shadingBinCapacity = 0u;
					std::size_t pagePayloadCapacity = 0u;
					// Resource Residency needs asset-owned byte counts even
					// though page-local virtual geometry streams are packed into
					// one per-view Vulkan buffer. This map records the exact
					// byte slices uploaded for each mesh in this frame resource.
					std::unordered_map<TerraEngine::UUID, std::uint64_t> pagePayloadBytesByMesh;
					// Repacking page-local meshlets is CPU-heavy. Each frame slot keeps
					// the signature of the bytes already stored in its Vulkan buffer so
					// an unchanged camera cut can reuse them on the next slot rotation.
					std::uint64_t pagePayloadReuseSignature = 0u;
					bool pagePayloadCacheValid = false;
					std::uint64_t retireAfterFrameId = 0u;
					bool live = false;
				};
					std::array<std::vector<VirtualGeometryGpuPacketFrameResources>, MAX_FRAMES_IN_FLIGHT> virtualGeometryGpuPacketResourcesByFrame{};
					std::deque<VirtualGeometryGpuPacketFrameResources> retiredVirtualGeometryGpuPacketResources{};
					std::array<std::vector<TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan>, MAX_FRAMES_IN_FLIGHT> pendingVirtualGeometryPageRetirementsByFrame{};
					std::uint64_t nextVirtualGeometryGpuPacketResourceSerial = 1u;
				// Decoded static pages use persistent buffers rather than the
				// per-view packet mirrors above. A candidate is queued on the
				// submitting frame slot and becomes publishable only after
				// BeginFrame observes that slot's fence.
				struct PersistentVirtualGeometryPageResource
				{
					TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
					std::uint32_t pageId = 0xffffffffu;
					std::uint64_t publicationGeneration = 0u;
					std::uint64_t allocationId = 0u;
					std::uint32_t bindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
					VulkanBuffer buffer{};
				};
				std::vector<PersistentVirtualGeometryPageResource> persistentVirtualGeometryPages;
				std::array<std::vector<TerraEngine::Resource::VirtualGeometryPagePublicationCandidate>,
					MAX_FRAMES_IN_FLIGHT> pendingVirtualGeometryPublicationsByFrame{};
				std::uint64_t nextVirtualGeometryPersistentPageAllocationId = 1u;
				std::uint32_t activeVirtualGeometryPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				std::uint32_t activeVirtualGeometryBatchBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				std::uint32_t activeVirtualGeometryCompactedPacketBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				std::uint32_t activeVirtualGeometryGeneratedSubmissionBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				std::uint32_t activeVirtualGeometryCounterReadbackBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				std::uint32_t activeVirtualGeometryPagePayloadBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
				// This plan records whether the GPU-driven executor has every live
				// prerequisite. CPU-authored mirrors remain authoritative until it does.
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorPlan latestVirtualGeometryExecutorPlan{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryFailureReceipt latestVirtualGeometryExecutorFailure{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorResources latestVirtualGeometryExecutorResources{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryExecutorPipelines virtualGeometryExecutorPipelines{};
				VkDescriptorSetLayout virtualGeometryExecutorDescriptorSetLayout = VK_NULL_HANDLE;
				std::string virtualGeometryExecutorUnavailableReason;
				// ExpandedProduction decoding is deliberately isolated from the packet
				// executor. Each slot owns every buffer the one-request compute ABI may
				// touch, and publication stays disabled until its fence-delayed parity
				// receipt matches the CPU reference.
				struct VirtualGeometryProductionDecodeFrameResources
				{
					VulkanBuffer requestBuffer{};
					VulkanBuffer encodedBuffer{};
					VulkanBuffer decodedBuffer{};
					VulkanBuffer resultReadbackBuffer{};
					VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
					TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodePlan plan{};
					std::uint64_t preparedFrameId = 0u;
					bool completionPending = false;
				};
				std::array<VirtualGeometryProductionDecodeFrameResources, MAX_FRAMES_IN_FLIGHT> virtualGeometryProductionDecodeResourcesByFrame{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryProductionDecodePipelines virtualGeometryProductionDecodePipelines{};
				VkDescriptorSetLayout virtualGeometryProductionDecodeDescriptorSetLayout = VK_NULL_HANDLE;
				std::string virtualGeometryProductionDecodeUnavailableReason;
				bool completedVirtualGeometryProductionDecodeEvidence = false;
				std::uint64_t publishedVirtualGeometryProductionDecodeGeneration = 0u;
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityPlan latestVirtualGeometryVisibilityPlan{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityResources latestVirtualGeometryVisibilityResources{};
				TerraEngine::Renderer::Vulkan::VulkanVirtualGeometryVisibilityPipelines virtualGeometryVisibilityPipelines{};
				VkDescriptorSetLayout virtualGeometryVisibilityDescriptorSetLayout = VK_NULL_HANDLE;
				// Composite pipelines are render-pass compatible objects; Scene/Game,
				// capture, and swapchain passes may therefore own distinct handles
				// while sharing one visibility pipeline layout.
				std::unordered_map<VkRenderPass, VkPipeline>
					virtualGeometryCompositePipelines;
				std::string virtualGeometryVisibilityUnavailableReason;
				static constexpr uint32_t kHiZViewCount = 2u;
				static constexpr uint32_t kHiZGameViewIndex = 0u;
				static constexpr uint32_t kHiZSceneViewIndex = 1u;
				// Each view/frame slot owns two Hi-Z pyramids: one stable read
				// image sampled by task shaders, and one write image warmed by
				// the post-depth compute pass. Scene View and Game View must not
				// share these images because their command buffers are recorded
				// separately but submitted together at the end of the editor frame.
				std::array<std::array<std::array<VulkanHiZ, 2>, MAX_FRAMES_IN_FLIGHT>, kHiZViewCount> hiZByViewFrame{};
				std::array<std::array<uint32_t, MAX_FRAMES_IN_FLIGHT>, kHiZViewCount> hiZReadIndexByViewFrame{};
				std::array<std::array<bool, MAX_FRAMES_IN_FLIGHT>, kHiZViewCount> hiZWritePendingSwapByViewFrame{};
				// RenderFrame discovers the true Scene/Game target sizes, but
				// Vulkan objects may only be recreated at a frame-slot fence
				// boundary. BeginFrame consumes these desired extents before any
				// command buffer binds the global Hi-Z descriptors.
				std::array<glm::uvec2, kHiZViewCount> desiredHiZExtentByView{};
				// Validation reads this frozen copy for the whole CPU frame.
				// GenerateHiZPyramid updates the write history while recording,
				// so using a per-view snapshot avoids same-frame scene/game
				// outputs accidentally validating against another view's pyramid.
				std::array<std::array<VulkanHiZHistoryMetadata, MAX_FRAMES_IN_FLIGHT>, kHiZViewCount> readableHiZHistoryByViewFrame{};
				VulkanVRS vrs;

				uint32_t HiZViewIndexForView(VulkanContext::ViewType view) const
				{
					return (view == VulkanContext::ViewType::Scene) ? kHiZSceneViewIndex : kHiZGameViewIndex;
				}

				VulkanHiZ& HiZForFrame(uint32_t viewIndex, uint32_t frameIndex)
				{
					const uint32_t viewSlot = std::min(viewIndex, kHiZViewCount - 1u);
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					return hiZByViewFrame[viewSlot][slot][std::min(hiZReadIndexByViewFrame[viewSlot][slot], 1u)];
				}

				const VulkanHiZ& HiZForFrame(uint32_t viewIndex, uint32_t frameIndex) const
				{
					const uint32_t viewSlot = std::min(viewIndex, kHiZViewCount - 1u);
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					return hiZByViewFrame[viewSlot][slot][std::min(hiZReadIndexByViewFrame[viewSlot][slot], 1u)];
				}

				VulkanHiZ& WritableHiZForFrame(uint32_t viewIndex, uint32_t frameIndex)
				{
					const uint32_t viewSlot = std::min(viewIndex, kHiZViewCount - 1u);
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					return hiZByViewFrame[viewSlot][slot][std::min(hiZReadIndexByViewFrame[viewSlot][slot] ^ 1u, 1u)];
				}

				void MarkHiZWritePending(uint32_t viewIndex, uint32_t frameIndex)
				{
					const uint32_t viewSlot = std::min(viewIndex, kHiZViewCount - 1u);
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					hiZWritePendingSwapByViewFrame[viewSlot][slot] = true;
				}

				void ConfirmHiZWritesSubmitted(uint32_t frameIndex)
				{
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					for (uint32_t viewSlot = 0u; viewSlot < kHiZViewCount; ++viewSlot)
					{
						if (!hiZWritePendingSwapByViewFrame[viewSlot][slot])
						{
							continue;
						}
						// Submission preserves command-buffer order, so the recorded
						// UNDEFINED-to-GENERAL transition now owns the physical layout.
						VulkanHiZ& submittedHiZ = WritableHiZForFrame(viewSlot, slot);
						submittedHiZ.layoutInitialized = true;
						submittedHiZ.layoutTransitionRecorded = false;
					}
				}

				void DiscardRecordedHiZWrites(uint32_t frameIndex)
				{
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					for (uint32_t viewSlot = 0u; viewSlot < kHiZViewCount; ++viewSlot)
					{
						VulkanHiZ& discardedHiZ = WritableHiZForFrame(viewSlot, slot);
						discardedHiZ.layoutTransitionRecorded = false;
						hiZWritePendingSwapByViewFrame[viewSlot][slot] = false;
					}
				}

				bool PromoteCompletedHiZWrite(uint32_t viewIndex, uint32_t frameIndex)
				{
					const uint32_t viewSlot = std::min(viewIndex, kHiZViewCount - 1u);
					const uint32_t slot = std::min(frameIndex, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT - 1));
					if (!hiZWritePendingSwapByViewFrame[viewSlot][slot])
					{
						return false;
					}
					hiZReadIndexByViewFrame[viewSlot][slot] ^= 1u;
					hiZWritePendingSwapByViewFrame[viewSlot][slot] = false;
					return true;
				}

				VulkanHiZ& CurrentHiZ(uint32_t viewIndex)
				{
					return HiZForFrame(viewIndex, currentFrame);
				}

				const VulkanHiZ& CurrentHiZ(uint32_t viewIndex) const
				{
					return HiZForFrame(viewIndex, currentFrame);
				}
				
	// Shadow resources are tracked by live typed demand. The fixed arrays cap
	// descriptor usage, while activeShadowResourceRequest decides how many
	// images and framebuffers each light family actually owns this frame.
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> directionalShadowDepths{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> directionalShadowColors{};
	std::array<VkFramebuffer, MAX_SHADOW_RESOURCE_SLOTS> directionalShadowFramebuffers{};
	std::array<uint32_t, MAX_SHADOW_RESOURCE_SLOTS> directionalShadowTextureIndices{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> spotShadowDepths{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> spotShadowColors{};
	std::array<VkFramebuffer, MAX_SHADOW_RESOURCE_SLOTS> spotShadowFramebuffers{};
	std::array<uint32_t, MAX_SHADOW_RESOURCE_SLOTS> spotShadowTextureIndices{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> areaShadowDepths{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> areaShadowColors{};
	std::array<VkFramebuffer, MAX_SHADOW_RESOURCE_SLOTS> areaShadowFramebuffers{};
	std::array<uint32_t, MAX_SHADOW_RESOURCE_SLOTS> areaShadowTextureIndices{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> pointShadowCubeDepths{};
	std::array<VulkanImage, MAX_SHADOW_RESOURCE_SLOTS> pointShadowCubeColors{};
	std::array<std::array<VkImageView, 6>, MAX_SHADOW_RESOURCE_SLOTS> pointShadowDepthFaceViewsBySlot{};
	std::array<std::array<VkImageView, 6>, MAX_SHADOW_RESOURCE_SLOTS> pointShadowColorFaceViewsBySlot{};
	std::array<std::array<VkFramebuffer, 6>, MAX_SHADOW_RESOURCE_SLOTS> pointShadowFaceFramebuffersBySlot{};
	ShadowResourceRequest activeShadowResourceRequest{};
	std::array<ShadowStorageRegion, MAX_SHADOW_RESOURCE_SLOTS> shadowStorageRegions{};
	std::unordered_map<TerraEngine::UUID, ShadowCacheEntry> shadowCacheEntries{};
	VulkanImage ltcAmplitudeTexture;
	VulkanImage ltcInverseTexture;
	uint32_t ltcAmplitudeTextureIndex = 0u;
	uint32_t ltcInverseTextureIndex = 0u;
	VulkanImage environmentIrradianceTexture;
	VulkanImage prefilteredEnvironmentTexture;
	VulkanImage brdfIntegrationTexture;
	uint32_t environmentIrradianceTextureIndex = 0u;
	uint32_t prefilteredEnvironmentTextureIndex = 0u;
	uint32_t brdfIntegrationTextureIndex = 0u;
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
	VkPipeline gtaoComputePipeline = VK_NULL_HANDLE;
	VkPipeline gtaoBlurComputePipeline = VK_NULL_HANDLE;
	VkPipelineLayout hiZPipelineLayout = VK_NULL_HANDLE;
	VkPipelineLayout aoPipelineLayout = VK_NULL_HANDLE;
	VkPipelineLayout vrsPipelineLayout = VK_NULL_HANDLE;
	static constexpr uint32_t kStatsViewCount = 2u;
	std::array<VulkanImage, MAX_FRAMES_IN_FLIGHT> ambientOcclusionRawByFrame{};
	std::array<VulkanImage, MAX_FRAMES_IN_FLIGHT> ambientOcclusionBlurredByFrame{};
	std::array<VulkanImage, MAX_FRAMES_IN_FLIGHT> ambientOcclusionHistoryByFrame{};
	uint32_t ambientOcclusionWidth = 0u;
	uint32_t ambientOcclusionHeight = 0u;
	std::array<bool, MAX_FRAMES_IN_FLIGHT> ambientOcclusionHistoryValidByFrame{};

	// struct GpuLightAnalysisViewSnapshot groups related state and operations so callers have one documented contract.
	struct GpuLightAnalysisViewSnapshot
	{
		std::unordered_map<TerraEngine::UUID, GPULightAnalysisRecord> recordsByLightId;
		std::uint32_t visibleCandidateCount = 0u;
		bool valid = false;
	};

	// struct TemporalLightSelectionState groups related state and operations so callers have one documented contract.
	struct TemporalLightSelectionState
	{
		float lastCompositeScore = 0.0f;
		float lastGpuVisibilityScore = 0.0f;
		float lastScreenInfluence = 0.0f;
		std::uint64_t lastSeenFrameSerial = 0u;
		bool retainedLastFrame = false;
		bool shadowedLastFrame = false;
	};

	// struct ViewLightingTemporalState groups related state and operations so callers have one documented contract.
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

	// struct ViewDepthHistoryState groups related state and operations so callers have one documented contract.
	struct ViewDepthHistoryState
	{
		// Hi-Z is built from a depth buffer rendered earlier. The matching
		// view-projection lets resize and camera-change frames fail open instead
		// of hiding visible meshlets with stale depth.
		glm::mat4 lastViewProjectionMatrix = glm::mat4(1.0f);
		// GTAO samples this owned depth copy instead of borrowing the live scene
		// attachment. Each frame slot has its own image so the GPU never reads
		// and overwrites the same history resource while frames overlap.
		std::array<VulkanImage, MAX_FRAMES_IN_FLIGHT> ambientOcclusionDepthByFrame{};
		std::array<glm::mat4, MAX_FRAMES_IN_FLIGHT> ambientOcclusionDepthViewProjectionByFrame{};
		std::array<bool, MAX_FRAMES_IN_FLIGHT> ambientOcclusionDepthValidByFrame{};
		bool valid = false;
	};

	// struct LightBudgetApplicationResult groups related state and operations so callers have one documented contract.
	struct LightBudgetApplicationResult
	{
		std::vector<FrameLightCandidate> uploadedCandidates;
		std::vector<FrameLightCandidate> retainedLights;
		FrameBudgetDiagnosticsSnapshot diagnostics;
	};

	static constexpr uint32_t kMaxStatsFrameSlots = 8u;
	static constexpr uint32_t kStatsSlotCount = kStatsViewCount * kMaxStatsFrameSlots;
	std::array<RenderStatsSnapshot, kStatsViewCount> latestStatsByView{};
	std::array<RenderStatsSnapshot, kStatsSlotCount> pendingStatsBySlot{};
	std::array<FrameBudgetDiagnosticsSnapshot, kStatsViewCount> latestBudgetDiagnosticsByView{};
	std::array<FrameBudgetDiagnosticsSnapshot, kStatsSlotCount> pendingBudgetDiagnosticsBySlot{};
	std::array<LightingDiagnosticsSnapshot, kStatsViewCount> latestLightingDiagnosticsByView{};
	struct GpuTimestampViewSample
	{
		bool valid = false;
		float milliseconds = 0.0f;
		std::uint64_t frameId = 0u;
		std::uint32_t latencyFrames = 0u;
		std::string reason;
	};
	// Virtual-geometry timestamp state is separate from the broad per-view GPU
	// timer. A stage stays unavailable until an executor owns an isolated live
	// Vulkan command; prepared CPU packet mirrors never enter this state as work.
	struct VirtualGeometryGpuTimestampStageSample
	{
		TerraEngine::Renderer::VirtualGeometryBackendStage stage =
			TerraEngine::Renderer::VirtualGeometryBackendStage::Traversal;
		TerraEngine::Renderer::VirtualGeometryBackendRoute route =
			TerraEngine::Renderer::VirtualGeometryBackendRoute::None;
		TerraEngine::Renderer::BackendMeasurementAvailability availability =
			TerraEngine::Renderer::BackendMeasurementAvailability::Unavailable;
		std::uint64_t frameNumber = 0u;
		std::uint32_t viewId = 0u;
		std::uint64_t rendererGeneration = 0u;
		std::uint64_t geometryGeneration = 0u;
		std::uint64_t residencyGeneration = 0u;
		std::uint64_t commandGeneration = 0u;
		std::uint32_t firstQuery = 0u;
		bool timestampWritesRecorded = false;
		bool published = false;
		std::string reason;
	};
	static constexpr uint32_t kGpuTimestampQueriesPerView = 2u;
	static constexpr uint32_t kGpuTimestampQueriesPerFrame = kStatsViewCount * kGpuTimestampQueriesPerView;
	static constexpr uint32_t kVirtualGeometryTimestampQueriesPerStage = 2u;
	static constexpr uint32_t kVirtualGeometryTimestampQueriesPerFrame =
		kStatsViewCount
		* static_cast<uint32_t>(TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount)
		* kVirtualGeometryTimestampQueriesPerStage;
	static constexpr uint32_t kTotalGpuTimestampQueriesPerFrame =
		kGpuTimestampQueriesPerFrame + kVirtualGeometryTimestampQueriesPerFrame;
	VkQueryPool gpuTimestampQueryPool = VK_NULL_HANDLE;
	float gpuTimestampPeriodNanoseconds = 0.0f;
	bool gpuTimestampQueriesSupported = false;
	std::uint64_t gpuTimestampQueryPoolGeneration = 0u;
	std::array<std::array<bool, kStatsViewCount>, MAX_FRAMES_IN_FLIGHT> pendingGpuTimestampWritesByFrame{};
	std::array<std::array<std::uint64_t, kStatsViewCount>, MAX_FRAMES_IN_FLIGHT> pendingGpuTimestampFrameIdsByFrame{};
	std::array<std::array<GpuTimestampViewSample, kStatsViewCount>, MAX_FRAMES_IN_FLIGHT> completedGpuTimestampSamplesByFrame{};
	std::array<std::array<std::array<VirtualGeometryGpuTimestampStageSample, TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount>, kStatsViewCount>, MAX_FRAMES_IN_FLIGHT> virtualGeometryGpuTimestampSamplesByFrame{};
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt> completedVirtualGeometryExecutionReceipts;
	std::vector<TerraEngine::Renderer::VirtualGeometryBackendMeasurement> completedVirtualGeometryMeasurements;
	// Capability flags are promoted only after a fence-delayed timestamp receipt,
	// never merely because a pipeline or descriptor set was created.
	std::array<bool, TerraEngine::Renderer::Vulkan::kVulkanVirtualGeometryStageCount> completedVirtualGeometryStageEvidence{};
	std::array<std::vector<FrameLightCandidate>, kStatsSlotCount> pendingLightAnalysisCandidatesBySlot{};
	std::array<GpuLightAnalysisViewSnapshot, kStatsViewCount> latestGpuLightAnalysisByView{};
	std::array<ViewLightingTemporalState, kStatsViewCount> lightingTemporalStateByView{};
	std::array<ViewDepthHistoryState, kStatsViewCount> depthHistoryByView{};
	std::array<ClusterValidationSnapshot, MAX_FRAMES_IN_FLIGHT> pendingClusterValidationByFrame{};
	RenderSnapshotHandle activeRenderSnapshot{};
	RenderSnapshotDiagnosticsSnapshot pendingRenderSnapshotDiagnostics{};
	RenderSnapshotDiagnosticsSnapshot latestRenderSnapshotDiagnostics{};
	TerraEngine::Renderer::BackendFrameBreakdown pendingBackendFrameBreakdown{};
	TerraEngine::Renderer::BackendFrameBreakdown latestBackendFrameBreakdown{};
	TerraEngine::Renderer::RendererRecoverySnapshot latestRendererRecoverySnapshot{};
	// NVIDIA diagnostic checkpoints are optional. When available they identify
	// the last GPU stage reached before a device loss; other vendors continue
	// through the same renderer path with this capability disabled.
	bool diagnosticCheckpointsEnabled = false;
	std::uint64_t backendFrameSerial = 0u;
	std::uint64_t rendererRecoveryFrameSerial = 0u;
	std::uint64_t renderSessionGeneration = 1u;
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
		void CreateEnvironmentLightingResources();
		void DestroyEnvironmentLightingResources();
		void CreateAmbientOcclusionResources();
		void DestroyAmbientOcclusionResources();
		bool EnsureShadowRenderResources(const ShadowResourceRequest& request);
		void UploadLightingData(const std::vector<GPULight>& lights);
		void UploadLightAnalysisInputData(const std::vector<GPULight>& lights);
			bool PrepareDepthForCompute(VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, bool updateGlobalDepthDescriptor);
			void RestoreDepthForAttachment(VkCommandBuffer cmd, VkImage sourceDepthImage);
			void StoreAmbientOcclusionDepthHistory(VkCommandBuffer cmd, VkImage sourceDepthImage, uint32_t viewIndex, const glm::mat4& viewProjection);
			bool GenerateHiZPyramid(VulkanHiZ& hiZ, VkCommandBuffer cmd, VkImage sourceDepthImage, VkImageView depthView, VkExtent2D sourceDepthExtent, uint32_t snapshotSlotIndex, const VulkanHiZHistoryMetadata& completedHistory);
			bool GenerateAmbientOcclusion(VkCommandBuffer cmd, const FrameCullingDataGPU& frameData, uint32_t snapshotSlotIndex, VkImageView sampledDepthView);
		void GenerateVRSImage(VkCommandBuffer cmd, VkImageView colorView);
	
		void PublishVirtualGeometryGpuPacketResources(
			const FrameRenderData& frameData,
			RenderViewType viewType,
			std::uint32_t viewId,
			std::uint64_t frameId,
			std::uint32_t statsSlotIndex,
			std::uint32_t lightDataOffset,
			std::uint32_t clusterDataOffset,
			std::uint32_t lightIndexOffset);
		void DestroyVirtualGeometryGpuPacketResources(VirtualGeometryGpuPacketFrameResources& resources);
		bool EnsureVulkanVirtualGeometryExecutorPipeline();
		void DestroyVulkanVirtualGeometryExecutorPipeline();
		bool EnsureVulkanVirtualGeometryProductionDecodePipeline();
		void DestroyVulkanVirtualGeometryProductionDecodePipeline();
		void PrepareVulkanVirtualGeometryProductionDecode(const FrameRenderData& frameData);
		void CompleteVulkanVirtualGeometryProductionDecode(std::uint32_t frameIndex);
	bool EnsureVulkanVirtualGeometryVisibilityPipelines();
	VkPipeline EnsureVulkanVirtualGeometryCompositePipeline(
		VkRenderPass renderPass);
	void DestroyVulkanVirtualGeometryVisibilityPipelines();
		void ResetMeshletCounters(uint32_t statsSlotIndex);
	
	// GetSnapshotLightDataOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t GetSnapshotLightDataOffset(uint32_t slotIndex) const { return slotIndex * MAX_GLOBAL_LIGHTS; }
	uint32_t GetSnapshotLightAnalysisInputOffset(uint32_t slotIndex) const { return slotIndex * MAX_GLOBAL_LIGHTS; }
	uint32_t GetSnapshotClusterDataOffset(uint32_t slotIndex) const { return slotIndex * MAX_LIGHT_CLUSTERS; }
	// GetSnapshotLightIndexOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t GetSnapshotLightIndexOffset(uint32_t slotIndex) const { return slotIndex * MAX_LIGHT_INDEX_ENTRIES; }
	// GetSnapshotClusterDepthOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t GetSnapshotClusterDepthOffset(uint32_t slotIndex) const { return slotIndex * kLightingClusterDimX * kLightingClusterDimY; }
	// GetSnapshotIndirectDrawOffset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint32_t GetSnapshotIndirectDrawOffset(uint32_t slotIndex) const { return slotIndex * MAX_BINDLESS_DRAWS; }
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
	LightingDiagnosticsSnapshot GetLatestLightingDiagnostics(RenderViewType viewType) const;
	GpuLightAnalysisViewSnapshot GetLatestGpuLightAnalysis(RenderViewType viewType) const;
	void RecordPendingClusterValidationSnapshot(uint32_t frameIndex, const ClusterValidationSnapshot& snapshot);
	ClusterValidationSnapshot ReadCompletedClusterValidationSnapshot(uint32_t frameIndex) const;
	void WriteClusterValidationCapture(const ClusterValidationSnapshot& snapshot, const FrameBudgetDiagnosticsSnapshot& diagnostics) const;

	uint32_t AllocateTextureIndex();
	void UpdateGlobalTexture(uint32_t index, const VulkanImage& image);

	uint32_t AllocateBufferIndex();
	void UpdateGlobalBuffer(uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
	void UpdateGlobalBufferForFrame(uint32_t frameIndex, uint32_t index, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range);
	void UpdateGlobalHiZ(uint32_t frameIndex);
	void WaitForDescriptorMutationIdle();

	uint32_t AllocateMaterialIndex();

	// Editor / Offscreen Resources
	// struct PushConstants groups related state and operations so callers have one documented contract.
	struct PushConstants {
		alignas(16) glm::mat4 viewProj;
		alignas(16) glm::mat4 model;
		uint32_t materialIndex;
		uint32_t meshDataIndex;
		uint32_t textureIndex;
		uint32_t vrsActive;
		uint32_t taskCullingEnabled;
		uint32_t statsSlotIndex;
		float cullingEpsilon;
		uint32_t lightDataOffset;
		uint32_t clusterDataOffset;
		uint32_t lightIndexOffset;
		uint32_t indirectDrawOffset;
		uint32_t virtualMeshletOffset;
		uint32_t virtualMeshletCount;
		uint32_t selectedVirtualLod;
		uint32_t virtualGeometryFlags;
		uint32_t selectedVirtualClusterOffset;
		uint32_t selectedVirtualClusterCount;
		uint32_t selectedVirtualPageNeedOffset;
		uint32_t selectedVirtualPageNeedCount;
		uint32_t virtualGeometryPacketBufferIndex;
		uint32_t virtualGeometryBatchBufferIndex;
		// Page payload identity is carried in push constants so task, mesh, and
		// fragment stages all agree which virtual-geometry page source produced
		// the currently emitted meshlets.
		uint32_t virtualGeometryPagePayloadBufferIndex;
		uint32_t virtualGeometryPayloadSource;
		uint32_t virtualGeometryEmittedSource;
		uint32_t virtualGeometryPagePayloadByteOffset;
		uint32_t virtualGeometryPagePayloadByteSize;
		uint32_t virtualGeometryPacketDescriptorIndex;
		// Editor-only tint is part of the default mesh push-constant contract so
		// every default shader stage observes the same byte layout.
		alignas(16) glm::vec4 editorTint = glm::vec4(0.0f);
	};
	static_assert(sizeof(PushConstants) == 256u, "Default mesh push constants must stay byte-compatible with Default.task/mesh/frag.hlsl.");

	// struct OffscreenResources groups related state and operations so callers have one documented contract.
	struct OffscreenResources {
		VkImage image = VK_NULL_HANDLE;
		VmaAllocation allocation = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkImage depthImage = VK_NULL_HANDLE;
		VmaAllocation depthAllocation = VK_NULL_HANDLE;
		VkImageView depthView = VK_NULL_HANDLE;
		// Every Scene, Game, and dedicated capture target owns the typed identity
		// image required by the conventional default fragment variant.
		ConventionalVisibilityIdentityAttachment visibilityIdentity;
		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		// Each offscreen target owns its real framebuffer extent. MCP capture
		// targets can differ from the editor swapchain, so readback must consult
		// this field instead of assuming every target is swapchain-sized.
		VkExtent2D extent{0u, 0u};
		// Vulkan may recycle the same numeric VkImageView/VkSampler handles after a
		// resize. The editor uses this monotonic identity to refresh its ImGui
		// descriptor even when comparing the raw handles would report no change.
		std::uint64_t generation = 0u;
		// Published only after this target records its conventional pass. Async
		// identity copies reject a frame/session mismatch before mapping pixels.
		std::uint64_t identityFrameId = 0u;
		std::uint64_t identityRendererGeneration = 0u;
	};
	OffscreenResources sceneView;
	OffscreenResources gameView;
	OffscreenResources captureView;
	struct RetiredOffscreenResourceSet
	{
		OffscreenResources resources;
		std::optional<VulkanContext::ViewType> editorView;
		std::uint64_t replacementGeneration = 0;
		std::uint64_t retireAfterSerial = 0;
		bool editorDescriptorAcknowledged = false;
	};
	std::deque<RetiredOffscreenResourceSet> retiredOffscreenResources;

	VkDescriptorSet offscreenDescriptorSet = VK_NULL_HANDLE;
	VkRenderPass offscreenRenderPass = VK_NULL_HANDLE;
	// Editor scene/game views store linear lighting before ImGui presents them.
	// A higher precision format prevents dark point-light falloff from being
	// quantized into visible bands while still allowing a conservative fallback.
	VkFormat offscreenColorFormat = VK_FORMAT_UNDEFINED;

	std::unordered_map<TerraEngine::UUID, VulkanMesh> meshCache;
	std::unordered_map<TerraEngine::UUID, VulkanImage> textureCache;
	// Pipelines keyed by RenderPass then PipelineKey (ShaderID + Flags)
	std::unordered_map<VkRenderPass, std::unordered_map<PipelineKey, VulkanPipeline, PipelineKeyHash>> pipelineCache;

	VulkanImage defaultTexture;
	VulkanImage defaultNormalTexture;
	VulkanImage defaultMaterialMaskTexture;
	std::vector<std::vector<VkCommandPool>> workerCommandPools; // Per-frame command pools for worker threads
	std::vector<std::vector<std::vector<VkCommandBuffer>>> workerCommandBuffers; // Per-frame, per-worker secondary command buffers
	// Track additional primary command buffers allocated when rendering multiple
	// cameras per frame so they can be freed after the frame fence completes.
	std::vector<std::vector<VkCommandBuffer>> extraPrimaryCommandBuffers;
	std::vector<std::vector<VkCommandBuffer>> pendingPrimaryCommandBuffers;
	// Tracks whether the per-frame pending primary list has already been
	// submitted to the graphics queue. This is separate from editorRenderSubmitted,
	// which only describes the UI wait-semaphore chain.
	std::vector<std::uint8_t> pendingPrimaryCommandBuffersSubmitted;
	std::vector<std::vector<VulkanBuffer>> retiredBuffersByFrame;
	// Animation resources are frame-slot owned, never animator owned. A single
	// descriptor set/resource table services every compatible packet in one
	// immutable generation, which prevents crowd scenes from allocating Vulkan
	// descriptors or submitting queues per animated instance.
	struct VulkanAnimationPoseHistoryIdentity
	{
		std::uint64_t stableInstanceId = 0U;
		std::uint64_t localPoseOffsetBytes = 0U;
		std::uint32_t jointCount = 0U;
		std::uint32_t skeletonGeneration = 0U;
		std::uint32_t programGeneration = 0U;

		auto operator<=>(const VulkanAnimationPoseHistoryIdentity&) const = default;
	};
	struct VulkanAnimationFrameResources
	{
		std::array<VulkanBuffer, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> buffers{};
		std::array<VkDeviceSize, TerraEngine::Animation::kGpuAnimationDescriptorBindingCount> capacities{};
		TerraEngine::Renderer::Vulkan::VulkanAnimationDescriptorSetPlan descriptorPlan{};
		std::uint32_t planGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint64_t frameId = 0U;
		std::uint64_t submissionSerial = 0U;
		std::uint32_t recordedDispatchCount = 0U;
		// These set-0/binding-9 slots are owned by the frame resource, not an
		// animator. Conventional graphics shaders receive them through copied
		// push constants and never need the compute-only set-1 descriptors.
		std::uint32_t generationRecordsBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t workPacketsBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t paletteMatricesBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t morphOutputsBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t graphicsBridgeBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		VulkanBuffer graphicsBridgeBuffer{};
		VkDeviceSize graphicsBridgeCapacityBytes = 0U;
		// Hold-last is safe only when this completed frame slot still contains the
		// same animator/layout sequence. A changed packet order or cooked generation
		// forces the local-pose buffer back to its stateless bind-pose sentinel.
		std::vector<VulkanAnimationPoseHistoryIdentity> poseHistoryIdentities;
		bool live = false;
		bool completionPending = false;
	};
	// CPU fallback palettes are graphics inputs only. Keeping them separate from
	// compute resources prevents a CPU animator from fabricating GPU dispatches
	// or overwriting a Hybrid/GPU palette in the same frame.
	struct VulkanCpuAnimationFrameResources
	{
		VulkanBuffer paletteBuffer{};
		VulkanBuffer morphWeightBuffer{};
		VulkanBuffer graphicsBridgeBuffer{};
		VkDeviceSize paletteCapacityBytes = 0U;
		VkDeviceSize morphWeightCapacityBytes = 0U;
		VkDeviceSize graphicsBridgeCapacityBytes = 0U;
		std::uint32_t paletteBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t morphWeightBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t graphicsBridgeBindlessIndex = INVALID_BINDLESS_BUFFER_INDEX;
		std::uint64_t frameId = 0U;
		std::uint64_t rendererGeneration = 0U;
		bool live = false;
	};
	VkDescriptorSetLayout animationDescriptorSetLayout = VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> animationDescriptorSets;
	// Animation compute shares the existing global descriptor set at set 0 and
	// keeps its frame-owned storage buffers at set 1. The pending plan is an
	// immutable render-system publication; it is retained only until the first
	// primary command buffer records it for the current frame slot.
	TerraEngine::Renderer::Vulkan::VulkanAnimationComputePipelines animationComputePipelines{};
	TerraEngine::Renderer::Vulkan::VulkanAnimationCommandPlan pendingAnimationCommandPlan{};
	std::shared_ptr<const TerraEngine::Animation::GpuAnimationPlan> pendingAnimationImmutablePlan;
	std::uint64_t pendingAnimationFrameId = 0U;
	bool animationComputeRecordedForCurrentFrame = false;
	bool animationComputeSubmissionMarkedForCurrentFrame = false;
	std::uint32_t recordedAnimationDispatchCountForCurrentFrame = 0U;
	struct VulkanAnimationCompletion
	{
		std::uint64_t frameId = 0U;
		std::uint32_t planGeneration = 0U;
		std::uint32_t rendererGeneration = 0U;
		std::uint64_t submissionSerial = 0U;
		std::uint32_t recordedDispatchCount = 0U;
	};
	std::vector<VulkanAnimationFrameResources> animationFrameResources;
	std::vector<VulkanCpuAnimationFrameResources> cpuAnimationFrameResources;
	std::deque<VulkanAnimationCompletion> completedAnimationGenerations;
	std::shared_ptr<const TerraEngine::Renderer::CpuAnimationDeformationPlan>
		pendingCpuAnimationPlan;
	TerraEngine::Renderer::AnimationGpuBackendExecutionSnapshot
		latestAnimationGpuBackendExecution{};
	std::uint64_t nextAnimationSubmissionSerial = 1U;
	// Graphics sees exactly one current-frame animation bridge buffer. The
	// inactive sentinel preserves dormant/static paths without stale indexing.
	std::uint32_t activeAnimationGraphicsBufferIndex = INVALID_BINDLESS_BUFFER_INDEX;
	std::uint32_t activeCpuAnimationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	// Animated virtual geometry owns a second compute envelope after animation
	// evaluation. Canonical demanded-page bytes and output arenas are
	// frame-fence owned; no
	// material shader or animator owns Vulkan resources in this path.
	struct VulkanDeformationFrameResources
	{
		std::array<VulkanBuffer,
			TerraEngine::Renderer::Vulkan::
				kVulkanDeformationDescriptorBindingCount> buffers{};
		std::array<VkDeviceSize,
			TerraEngine::Renderer::Vulkan::
				kVulkanDeformationDescriptorBindingCount> capacities{};
		TerraEngine::Renderer::Vulkan::
			VulkanDeformationDescriptorSetPlan descriptorPlan{};
		struct GraphicsGroup
		{
			std::uint64_t stableInstanceId = 0u;
			TerraEngine::UUID meshId = TerraEngine::EMPTY_UUID;
			// The bridge stores each owner/mesh slice contiguously. Draw push
			// constants carry this word offset so shaders never scan other owners.
			std::uint32_t headerWordOffset = 0u;
		};
		std::vector<GraphicsGroup> graphicsGroups;
		std::vector<TerraEngine::Renderer::DeformedPageKey> workKeys;
		VulkanBuffer graphicsBridgeBuffer{};
		VkDeviceSize graphicsBridgeCapacityBytes = 0u;
		std::uint32_t currentVerticesBindlessIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t boundsBindlessIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t refinementBindlessIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
		std::uint32_t graphicsBridgeBindlessIndex =
			INVALID_BINDLESS_BUFFER_INDEX;
		std::uint64_t frameId = 0u;
		std::uint64_t rendererGeneration = 0u;
		bool live = false;
	};
	VkDescriptorSetLayout deformationDescriptorSetLayout =
		VK_NULL_HANDLE;
	std::vector<VkDescriptorSet> deformationDescriptorSets;
	std::vector<VulkanDeformationFrameResources>
		deformationFrameResources;
	TerraEngine::Renderer::Vulkan::
		VulkanDeformationComputePipelines deformationComputePipelines{};
	TerraEngine::Renderer::Vulkan::
		VulkanDeformationCommandPlan pendingDeformationCommandPlan{};
	std::shared_ptr<const TerraEngine::Renderer::GpuDeformationPlan>
		pendingDeformationImmutablePlan;
	bool deformationComputeRecordedForCurrentFrame = false;
	std::uint32_t recordedDeformationDispatchCountForCurrentFrame = 0u;
	std::uint32_t activeDeformationGraphicsBufferIndex =
		INVALID_BINDLESS_BUFFER_INDEX;
	std::vector<std::vector<size_t>> workerBufferUsage; // Tracks next available secondary buffer index per worker per frame
	std::atomic_bool swapchainImageAcquired = false;
	// A descriptor-safety flush may submit a runtime prefix and consume the
	// acquire semaphore before EndFrame. Image ownership remains active, while
	// this flag prevents the final submit from waiting the binary semaphore twice.
	std::atomic_bool swapchainAcquireWaitConsumed = false;
	std::atomic_bool skipFrame = false;
	// Vulkan device loss invalidates the live device and its queues. Swapchain
	// recreation cannot repair that state, so latch it once and make frame
	// entry points return quietly instead of retrying failed queue work.
	std::atomic_bool deviceLost = false;
	std::atomic_bool deviceLostLogged = false;
	std::atomic_bool editorRenderSubmitted = false;
	uint32_t acquiredImageIndex = 0;

	// Each record owns every Vulkan object needed by one asynchronous copy. GPU
	// resources remain alive after a client timeout until its fence proves that
	// the queue has stopped using them.
	struct VulkanReadbackRequest {
		TerraEngine::Renderer::RenderCaptureResult completion;
		RenderViewType viewType = RenderViewType::Game;
		VkImage sourceImage = VK_NULL_HANDLE;
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VmaAllocation stagingAllocation = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		// The live target can follow the dock size while automation requires an
		// exact output size. Preserve both dimensions until encoding completes.
		std::uint32_t requestedWidth = 0;
		std::uint32_t requestedHeight = 0;
		VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
		std::chrono::steady_clock::time_point submittedAt{};
		std::chrono::milliseconds timeout = std::chrono::milliseconds(2000);
		bool completionPublished = false;
		// Preserve which offscreen image owns this readback. Source kind remains
		// GameView for next-frame captures even though their pixels come from the
		// dedicated target rather than a potentially hidden dock target.
		bool dedicatedRenderTarget = false;
	};
	std::deque<VulkanReadbackRequest> inFlightReadbacks;
	std::deque<TerraEngine::Renderer::RenderCaptureResult> completedReadbacks;
	// Identity copies are kept separate from image capture so their uint4 ABI and
	// generation checks can fail closed without manufacturing color metadata.
	struct VulkanConventionalVisibilityIdentityReadbackRequest
	{
		TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult completion;
		VkImage sourceImage = VK_NULL_HANDLE;
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VmaAllocation stagingAllocation = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkCommandPool commandPool = VK_NULL_HANDLE;
	};
	std::deque<VulkanConventionalVisibilityIdentityReadbackRequest> inFlightConventionalVisibilityIdentityReadbacks;
	std::deque<TerraEngine::Renderer::ConventionalVisibilityIdentityReadbackResult> completedConventionalVisibilityIdentityReadbacks;

	bool InitVulkan(WindowSystem& windowSystem);
	void Cleanup(bool skipDeviceIdle = false);
	void ProcessReadbackRequests();
	void DestroyReadbackRequestResources(VulkanReadbackRequest& request);
	void CancelReadbackRequests(std::string_view errorCategory, std::string_view message, bool publishResults);
	void ProcessConventionalVisibilityIdentityReadbacks();
	void DestroyConventionalVisibilityIdentityReadbackRequestResources(
		VulkanConventionalVisibilityIdentityReadbackRequest& request);
	[[nodiscard]] bool HasInFlightReadbackForImage(VkImage image) const;

	void InitEditorMode();
	void CreateOffscreenResources(OffscreenResources& res, VkExtent2D requestedExtent = {});
	void DestroyOffscreenResourceSet(OffscreenResources& resources);
	void DrainRetiredOffscreenResources();
	void RecreateOffscreenResources(); // Handle resize
	void ClearResources();
	bool MarkDeviceLost(VkResult result, std::string_view operation);
	void RecordDiagnosticCheckpoint(VkCommandBuffer commandBuffer, const char* marker) const;
	std::string CollectDiagnosticCheckpointSummary();
	void RecordInitializationFailure(std::string_view result, std::string_view operation);
	VkResult SubmitGraphicsQueue(std::uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence);
	VkResult PresentSwapchain(const VkPresentInfoKHR* presentInfo);
	VkResult WaitGraphicsQueueIdle();
	VkResult WaitDeviceIdle();

	// Grid Renderer
	VkPipeline sceneViewGridPipeline = VK_NULL_HANDLE;
	VkPipelineLayout sceneViewGridPipelineLayout = VK_NULL_HANDLE;
	VkPipeline skyBackgroundPipeline = VK_NULL_HANDLE;
	VkPipelineLayout skyBackgroundPipelineLayout = VK_NULL_HANDLE;
	
	// struct GridPushConstants groups related state and operations so callers have one documented contract.
	struct GridPushConstants {
		glm::mat4 view;
		glm::mat4 proj;
		glm::mat4 inverseView;
		glm::mat4 inverseProj;
	};

	void CreateSceneViewGridPipeline(VkRenderPass renderPass);
	void RenderSceneViewGrid(VkCommandBuffer cmd, const FrameRenderData& frameData, VkExtent2D renderExtent);
	void CreateSkyBackgroundPipeline(VkRenderPass renderPass);
	void RenderSkyBackground(VkCommandBuffer cmd, const FrameRenderData& frameData, VkExtent2D renderExtent);

	// UI Rendering
	VkPipeline staticUiPipeline = VK_NULL_HANDLE;
	VkPipelineLayout staticUiPipelineLayout = VK_NULL_HANDLE;
	VkPipeline instancedUiPipeline = VK_NULL_HANDLE;
	VkPipelineLayout instancedUiPipelineLayout = VK_NULL_HANDLE;
	VkPipeline sdfTextPipeline = VK_NULL_HANDLE;
	VkPipelineLayout sdfTextPipelineLayout = VK_NULL_HANDLE;
	VkDescriptorSetLayout uiFontDescriptorSetLayout = VK_NULL_HANDLE;
	VkDescriptorSet uiFontDescriptorSet = VK_NULL_HANDLE;
	VulkanImage uiFontAtlasImage{};
	int uiFontAtlasWidth = 0;
	int uiFontAtlasHeight = 0;
	float uiFontAtlasPixelHeight = 48.0f;
	std::vector<unsigned char> uiFontAtlasSourceBytes;
	std::filesystem::path uiFontAtlasSourcePath;
	bool uiFontAtlasSourceLoadAttempted = false;
	bool uiFontAtlasMissingWarned = false;
	UIRenderer uiTextRenderer;

	// Re-usable UI buffers (resized dynamically)
	// Runtime UI vertices are rewritten by the CPU. Keep one allocation per
	// frame-fence slot so a later CPU frame cannot overwrite memory still being
	// consumed by an earlier GPU submission.
	std::vector<VulkanBuffer> uiStaticVertexBuffers;
	VulkanBuffer uiInstanceBuffer{};
	std::vector<VulkanBuffer> uiSdfVertexBuffers;

	void CreateUIPipelines(VkRenderPass renderPass);
	void RenderUI(VkCommandBuffer cmd, const UIRenderBatch& batch);

	bool CreateInstance();
	void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo);
	bool SetupDebugMessenger();
	bool PickPhysicalDevice();
	bool CreateLogicalDevice();

	bool CreateSwapChain(WindowSystem& windowSystem, VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE);
	// Becomes true before vkCreateSwapchainKHR receives a non-null old swapchain.
	// That call is an ownership boundary: failure must not restore/acquire it.
	bool oldSwapchainConsumedByCreateAttempt = false;
	bool CreateImageViews();
	VkFormat FindDepthFormat();
	VkFormat FindSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features);
	bool CreateDepthResources();
	bool CreateRenderPass();
	bool CreateFramebuffers();
	bool CreateCommandPool();
	bool CreateSyncObjects();
	void InitializeFrameTracking();
	void DestroyFrameSyncObjects();
	void DestroyPresentationImageSyncObjects();
	PresentationResourceBundle TakePresentationResourceBundle();
	void RestorePresentationResourceBundle(PresentationResourceBundle&& bundle);
	void DestroyPresentationResourceBundle(PresentationResourceBundle& bundle);
	void RetirePresentationResourceBundle(PresentationResourceBundle&& bundle, std::uint64_t generation);
	void CollectCompletedPresentationResourceBundles();
	bool PreparePresentationCompletionFence(std::uint32_t imageIndex,
		VkPresentInfoKHR& presentInfo, VkSwapchainPresentFenceInfoKHR& completionInfo);
	void ResolveUnenqueuedPresentationFence(bool fencePrepared, VkResult presentResult);
	VkFence PrepareUnextendedAcquireProof();
	void ResolveUnextendedAcquireProof(VkFence proofFence, VkResult acquireResult, std::uint32_t imageIndex);
	void CollectCompletedUnextendedAcquireProofs();
	void RecordUnextendedPresent(std::uint32_t imageIndex, VkResult presentResult);
	bool CreateGpuTimestampQueries();
	void DestroyGpuTimestampQueries();
	void ResolveCompletedGpuTimestamps(uint32_t frameIndex);
	void MarkGpuTimestampBegin(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view, std::uint64_t frameId);
	void MarkGpuTimestampEnd(VkCommandBuffer cmd, uint32_t frameIndex, VulkanContext::ViewType view);
	void BeginVirtualGeometryTimestampFrame(
		uint32_t frameIndex,
		VulkanContext::ViewType view,
		std::uint64_t frameNumber,
		std::uint32_t viewId,
		std::uint64_t rendererGeneration);
	void MarkVirtualGeometryTimestampBegin(
		VkCommandBuffer cmd,
		uint32_t frameIndex,
		VulkanContext::ViewType view,
		TerraEngine::Renderer::VirtualGeometryBackendStage stage,
		TerraEngine::Renderer::VirtualGeometryBackendRoute route,
		std::uint64_t geometryGeneration,
		std::uint64_t residencyGeneration,
		std::uint64_t commandGeneration);
	void MarkVirtualGeometryTimestampEnd(
		VkCommandBuffer cmd,
		uint32_t frameIndex,
		VulkanContext::ViewType view,
		TerraEngine::Renderer::VirtualGeometryBackendStage stage);
	void ResolveCompletedVirtualGeometryTimestamps(uint32_t frameIndex);

	bool CreateDescriptorPool();

	void CreateDefaultTexture();
	VulkanBuffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, const char* name = nullptr);
	void DestroyBufferImmediate(VulkanBuffer& buffer);
	void RetireBuffer(VulkanBuffer& buffer);
	void PublishVirtualGeometryDecodedPageResources(const FrameRenderData& frameData);
	bool PublishVulkanAnimationResources(const TerraEngine::Animation::GpuAnimationPlan& plan, std::uint32_t rendererGeneration);
	bool PublishVulkanCpuAnimationResources(
		const TerraEngine::Renderer::CpuAnimationDeformationPlan& plan);
	bool EnsureVulkanAnimationDescriptorSets();
	bool EnsureVulkanAnimationComputePipelines();
	void DestroyVulkanAnimationComputePipelines();
	bool EnsureVulkanDeformationDescriptorSets();
	bool EnsureVulkanDeformationComputePipelines();
	void DestroyVulkanDeformationComputePipelines();
	bool PublishVulkanDeformationResources(
		const TerraEngine::Renderer::GpuDeformationPlan& plan);
	void RetireVulkanDeformationFrameResources(
		VulkanDeformationFrameResources& resources);
	void MarkVulkanAnimationGenerationSubmitted(std::uint32_t rendererGeneration);
	void CompleteVulkanAnimationFrameResources(std::uint32_t frameIndex);
	void RetireVulkanAnimationFrameResources(VulkanAnimationFrameResources& resources);
	void RetireVulkanCpuAnimationFrameResources(
		VulkanCpuAnimationFrameResources& resources);
	void CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
	VkCommandBuffer BeginSingleTimeCommands();
	bool EndSingleTimeCommands(VkCommandBuffer commandBuffer, std::string_view operation = "immediate command");
	bool TransitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout, uint32_t mipLevels = VK_REMAINING_MIP_LEVELS, uint32_t arrayLayers = 1u);
	bool CopyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
	VkImageView CreateImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags, uint32_t mipLevels = 1u);

	VkFormat MapTextureFormat(uint32_t vkFormat);
	void EnsureTexture(TerraEngine::UUID textureId);
	std::vector<VkDescriptorSet> EnsureMaterialDescriptorSets(TerraEngine::UUID shaderCollectionId, TerraEngine::UUID materialId);
	void InvalidateMaterial(TerraEngine::UUID materialId);
	void InvalidateMesh(TerraEngine::UUID meshId);
	void InvalidateTexture(TerraEngine::UUID textureId, bool reloadAfterInvalidate = true);
	void InvalidateResource(TerraEngine::UUID id, bool reloadTexturesAfterInvalidate = true);
	void EnsureMesh(TerraEngine::UUID meshId);
	std::optional<PipelineKey> BuildPipelineKey(
		TerraEngine::UUID shaderCollectionId,
		uint32_t materialFlags,
		bool usesConventionalVisibilityIdentity = false) const;
	void EnsurePipeline(TerraEngine::UUID shaderCollectionId, VkRenderPass renderPass,
		uint32_t materialFlags, bool usesConventionalVisibilityIdentity = false);
	bool UsesConventionalVisibilityAttachments(VkRenderPass pass) const;
	bool CreateConventionalVisibilityIdentityAttachment(
		VkExtent2D extent, ConventionalVisibilityIdentityAttachment& attachment);
	void DestroyConventionalVisibilityIdentityAttachment(
		ConventionalVisibilityIdentityAttachment& attachment);
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
