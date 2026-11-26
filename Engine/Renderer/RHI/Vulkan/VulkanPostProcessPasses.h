// VulkanPostProcessPasses.h - actual fixed-exposure and output composition.
#pragma once

#include "VulkanPostProcessTargets.h"
#include "VulkanPostProcessLensHdr.h"
#include "VulkanPostProcessScreenSpaceInputs.h"
#include "VulkanPostProcessHistory.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"

#include <memory>
#include <string>

namespace TerraEngine::Renderer
{
	struct RenderViewPresentationInput;
	struct PostProcessGraphSettings;
	enum class RenderCaptureColorEncoding : std::uint8_t;
}
namespace TerraEngine::Renderer::GraphicsSettings { struct GraphicsSettingsSnapshot; struct GraphicsCameraTarget; }

namespace TerraEngine::Renderer::Vulkan
{
	// Discover the same physical format that offscreen allocation will use.
	// This query is also valid before the first camera target is created.
	VkFormat SelectOffscreenCameraColorFormat(VkPhysicalDevice device) noexcept;
	// The shader's quantization step matches these 8-bit SDR presentation formats.
	constexpr bool SupportsOutputDithering(VkFormat format) noexcept
	{
		return format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB
			|| format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
	}

	struct AutoExposurePass;
	struct BloomPass;
	struct TonePass;
	struct FxaaPass;
	struct SmaaPass;
	struct Cmaa2Pass;
	struct IndexedMsaa4Resources;
	struct LensPresentationPass;
	struct LensHdrPass;
	struct DepthOfFieldPass;
	struct AtmospherePass;
	struct MotionBlurPass;
	struct ScreenSpaceRasterResources;
	struct ScreenSpaceLightingPass;
	struct CustomPostProcessView;
	struct PostProcessCustomViewConfiguration;
	using RetireScreenRaster = void (*)(void*, const std::shared_ptr<ScreenSpaceRasterResources>&) noexcept;
	class TemporalViewFrameState;
	struct TemporalViewResources;
	struct ScaledCameraTargets;
	struct PostProcessLookupImages;
	enum class PostProcessStage : std::uint32_t
	{
		Exposure = 1, Bloom = 2, ToneAndGrade = 3, SpatialAA = 4, Presentation = 5, Temporal = 6,
		DepthOfField = 7, LensRadiance = 8, LensDirt = 9, LensFlare = 10, MotionBlur = 11, MotionDepthHistory = 12,
		ScreenAO = 13, ScreenReflections = 14, ScreenIndirect = 15, ScreenComposite = 16,
		CustomHdr = 17, CustomDisplay = 18, CustomBeforeUI = 19, ScreenDepthHierarchy = 20,
		Atmosphere = 21, OutputDither = 22
	};
	using PostProcessStageMarker = void (*)(void*, PostProcessStage, bool);

	struct PostProcessPassConfiguration
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkRenderPass destinationRenderPass = VK_NULL_HANDLE;
		VkRenderPass cameraRasterPass = VK_NULL_HANDLE;
		VkFormat destinationFormat = VK_FORMAT_UNDEFINED;
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		bool runtimeDestination = false;
		bool usesVrs = false;
		VkExtent2D vrsTexelSize{};
		bool needsGrid = false;
		bool needsPresentationEffects = false;
		bool needsTemporal = false;
		bool meshTemporal = false;
		bool needsScaledCamera = false;
		bool ditherCameraOutput = false;
		GraphicsSettings::PostProcessEffectSettings effects;
		LensHdrDirtTexture dirtTexture;
		std::uint64_t backendResourceGeneration = 0u;
		std::uint64_t viewLifetimeGeneration = 0u;
		ScreenSpaceInputViewOwner screenSpaceOwner;
		RetireScreenRaster retireScreenRaster = nullptr;
		void* screenRasterContext = nullptr;
		std::shared_ptr<const PostProcessCustomViewConfiguration> custom;
		PostProcessTargetImage sceneColorSource;
		PostProcessTargetImage sceneIdentity;
		PostProcessTargetImage sceneDepth;
		std::array<VkImageView, 4> destinationViews{};
		GraphicsSettings::PostProcessColorSettings color{};
		GraphicsSettings::GraphicsAntiAliasingMode antiAliasing = GraphicsSettings::GraphicsAntiAliasingMode::Off;
		std::shared_ptr<PostProcessLookupImages> lookups;
	};

	// This copied input describes real source/destination resources at the
	// protected backend boundary. The same plan builder is used before settings
	// publication and again before recording an already-qualified frame.
	struct PreparedPostProcessViewResources
	{
		std::uint64_t rendererGeneration = 0;
		std::uint64_t backendResourceGeneration = 0;
		VkExtent2D sourceExtent{};
		VkFormat sourceFormat = VK_FORMAT_UNDEFINED;
		std::uint64_t sourceTargetGeneration = 0;
		PostProcessPassConfiguration passes;
	};

	bool SupportsPostProcessPlan(const PostProcessGraphSettings& plan) noexcept;
	// Legacy native fixtures have no authored camera and retain their zero
	// lifetime key. Production camera callers must publish a concrete view.
	PostProcessTargetKey MakePostProcessTargetKey(const RenderViewPresentationInput& presentation,
		std::uint64_t rendererGeneration) noexcept;
	bool MatchesPostProcessCamera(const PostProcessTargetKey& key,
		const GraphicsSettings::GraphicsCameraTarget& camera) noexcept;

	bool BuildPostProcessViewRequest(const RenderViewPresentationInput& presentation,
		const GraphicsSettings::GraphicsSettingsSnapshot& settings,
		const PreparedPostProcessViewResources& resources, PostProcessTargetRequest& request,
		PostProcessPassConfiguration& configuration, std::string& reason);

	// A selected capture must match the exact source target and accepted frame.
	// Older processing bytes remain unavailable after a source target is replaced
	// or a newer legacy/disabled frame is rendered into that same source image.
	struct PostProcessOutputPublication
	{
		VkImage sourceImage = VK_NULL_HANDLE;
		std::uint64_t sourceTargetGeneration = 0;
		std::uint64_t publicationFrameId = 0;
		std::uint64_t settingsRevision = 0;
		VkCommandBuffer command = VK_NULL_HANDLE;
		bool submissionAccepted = false;
		VkImage compositedImage = VK_NULL_HANDLE;
		VkFormat compositedFormat = VK_FORMAT_UNDEFINED;
		VkExtent2D compositedExtent{};
		VkImageLayout compositedLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		RenderCaptureColorEncoding compositedEncoding = static_cast<RenderCaptureColorEncoding>(0);
		bool overlaysIncluded = false;
		bool rawPreview = false;
	};

	inline bool IsPostProcessOutputCurrent(const PostProcessOutputPublication& output,
		VkImage sourceImage, std::uint64_t sourceTargetGeneration, std::uint64_t sourceFrameId) noexcept
	{
		return output.submissionAccepted && sourceImage != VK_NULL_HANDLE && sourceFrameId != 0u
			&& output.sourceImage == sourceImage && output.sourceTargetGeneration == sourceTargetGeneration
			&& output.publicationFrameId == sourceFrameId;
	}

	// The target bundle destroys this owner before destroying borrowed views.
	// Layouts/descriptors are immutable once ready. The target owner records its
	// borrowing commands until their actual completion submission succeeds.
	struct PostProcessPassResources
	{
		VkDevice device = VK_NULL_HANDLE;
		VkRenderPass processingPass = VK_NULL_HANDLE;
		VkRenderPass overlayPass = VK_NULL_HANDLE;
		bool ownsOverlayPass = false;
		std::array<VkFramebuffer, 2> processingFramebuffers{};
		VkFramebuffer overlayFramebuffer = VK_NULL_HANDLE;
		VkPipelineLayout fullscreenLayout = VK_NULL_HANDLE;
		VkPipeline exposurePipeline = VK_NULL_HANDLE;
		const PostProcessTargetImage* exposureSource = nullptr;
		std::uint32_t exposureOutput = 0u;
		VkPipeline compositePipeline = VK_NULL_HANDLE;
		VkPipeline ditherPipeline = VK_NULL_HANDLE;
		VkPipelineLayout gridLayout = VK_NULL_HANDLE;
		VkPipeline gridPipeline = VK_NULL_HANDLE;
		// HUD variants belong to this exact overlay pass; the uint identity target
		// is preserved with a separate disabled blend/write attachment.
		std::uint32_t overlayColorAttachmentCount = 1;
		VkPipeline staticUiPipeline = VK_NULL_HANDLE;
		VkPipeline sdfTextPipeline = VK_NULL_HANDLE;
		std::shared_ptr<AutoExposurePass> autoExposure;
		std::shared_ptr<BloomPass> bloom;
		std::shared_ptr<TonePass> tone;
		// Exactly one spatial variant owns the final display-linear resolve.
		std::shared_ptr<FxaaPass> fxaa;
		std::shared_ptr<SmaaPass> smaa;
		std::shared_ptr<Cmaa2Pass> cmaa2;
		std::shared_ptr<IndexedMsaa4Resources> coverage;
		std::shared_ptr<LensPresentationPass> lensPresentation;
		std::shared_ptr<DepthOfFieldPass> depthOfField;
		std::shared_ptr<AtmospherePass> atmosphere;
		std::shared_ptr<ScreenSpaceRasterResources> screenRaster;
		std::shared_ptr<ScreenSpaceLightingPass> screenLighting;
		RetireScreenRaster retireScreenRaster = nullptr;
		void* screenRasterContext = nullptr;
		std::shared_ptr<const CustomPostProcessView> customView;
		std::array<std::uint32_t, 3> customSources{};
		std::shared_ptr<const PostProcessCustomViewConfiguration> customConfiguration;
		std::shared_ptr<MotionBlurPass> motionBlur;
		std::uint32_t motionBlurSource = 0u, motionBlurOutput = 0u;
		bool temporalReconstruction = false;
		std::shared_ptr<BloomPass> lensRadiance;
		std::shared_ptr<LensHdrPass> lensDirt, lensFlare;
		// History is bound to this exact target and follows its recorded
		// native queue uses and completion epochs through retirement.
		std::shared_ptr<TemporalViewFrameState> temporalFrameState;
		std::shared_ptr<TemporalViewResources> temporalView;
		std::shared_ptr<ScaledCameraTargets> scaledCamera;
		std::shared_ptr<PostProcessLookupImages> lookups;
		std::uint32_t finalProcessingImage = 0u;
		PostProcessOutputPublication output;
		~PostProcessPassResources();
	};

	bool PreparePostProcessPasses(PostProcessTargetBundle& targets,
		const PostProcessPassConfiguration& configuration, std::string& reason);
	bool RecordPostProcessChain(VkCommandBuffer command, PostProcessTargetBundle& targets,
		VkImage source, const GraphicsSettings::PostProcessColorSettings& color, float deltaTimeSeconds,
		PostProcessStageMarker markStage, void* markerContext,
		const GraphicsSettings::PostProcessEffectSettings& effects, std::uint32_t seed,
		const struct TemporalChainRecording* temporal = nullptr,
		const std::shared_ptr<const RenderFramePublication>& publication = {},
		std::uint32_t presentationIndex = 0u);
	void BeginPostProcessComposite(VkCommandBuffer command, PostProcessTargetBundle& targets,
		VkFramebuffer destination, VkExtent2D extent, bool rawSource = false);
	void RecordPostProcessGrid(VkCommandBuffer command, const PostProcessTargetBundle& targets,
		const RenderViewPresentationInput& presentation, VkExtent2D extent);
	void EndPostProcessComposite(VkCommandBuffer command, bool runtimeDestination,
		VkImage destination);
}
