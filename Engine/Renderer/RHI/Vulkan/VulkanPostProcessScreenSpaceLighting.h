// Persistent native compute ownership for current-frame screen-space lighting.
#pragma once
#include "VulkanPostProcessScreenSpaceRaster.h"
#include "Renderer/GraphicsSettings/PostProcessEffectSettings.h"

namespace TerraEngine::Renderer { struct RenderFramePublication; }
namespace TerraEngine::Renderer::Vulkan
{
	struct ScreenSpaceLightingPass;
	enum class ScreenSpaceLightingStage : std::uint32_t { GTAO = 1, Reflections = 2, Indirect = 3, Composite = 4, DepthHierarchy = 5 };
	// Trusted native marker only: must not throw/allocate or mutate live scene
	// state. Parent owns timestamp queries, accepted submission and availability.
	using ScreenSpaceLightingStageMarker = void (*)(void*, ScreenSpaceLightingStage, bool);
	struct ScreenSpaceLightingImageState
	{
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags access = 0;
	};
	enum class ScreenSpaceOriginalAo { Unknown, LegacyScreenAoPresent, LegacyScreenAoOmitted };
	struct ScreenSpaceGeometryRecordedUse
	{
		// The protected recorder supplies this only AFTER recording the genuine
		// matching geometry pass on this command buffer. This is recording proof,
		// not a GPU completion or producer stamp manufactured by preparation.
		VkCommandBuffer command = VK_NULL_HANDLE;
		std::uint64_t frameId = 0;
		ScreenSpaceInputViewOwner owner;
		std::array<ScreenSpaceInputImage, 7> colors{};
		ScreenSpaceInputImage depth;
		std::array<ScreenSpaceLightingImageState, 7> colorStates{};
		ScreenSpaceLightingImageState depthState;
		VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(0);
		bool variableRateShading = true;
		ScreenSpaceOriginalAo originalAo = ScreenSpaceOriginalAo::Unknown;
	};
	struct ScreenSpaceHiZRecordedUse
	{
		VkCommandBuffer command = VK_NULL_HANDLE;
		std::uint64_t frameId = 0;
		ScreenSpaceInputImage image;
		ScreenSpaceLightingImageState state;
	};
	struct ScreenSpaceLightingCreateInfo
	{
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VmaAllocator allocator = VK_NULL_HANDLE;
		ScreenSpaceInputDemand demand;
		std::shared_ptr<const ScreenSpaceRasterResources> raster;
		ScreenSpaceInputImage output; // Distinct same-extent/format HDR storage target.
		std::optional<ScreenSpaceInputImage> hiZ; // Optional genuine standard-Z MAX pyramid.
		VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
		bool variableRateShading = false;
		// Optional persistent acceleration for active SSR/SSGI. Failure retains
		// direct SAME-FRAME depth tracing; AO-only/disabled paths allocate none.
		bool prepareSameFrameHiZ = true;
	};
	struct ScreenSpaceLightingFrameInput
	{
		std::shared_ptr<const RenderFramePublication> publication;
		std::uint32_t presentationIndex = 0;
		ScreenSpaceGeometryRecordedUse geometry;
		std::optional<ScreenSpaceHiZRecordedUse> hiZ;
		ScreenSpaceLightingImageState outputState;
	};
	struct ScreenSpaceLightingStyle
	{
		// Fixed projection-independent world-unit style, copied by the protected
		// owner. C# quality/intensity continue to come from the sealed settings.
		float aoRadius = 0.5f, aoBias = 0.01f;
		float aoDepthSigma = 0.1f, aoNormalPower = 8.0f;
		float traceDistance = 50.0f, traceThickness = 0.1f;
	};
	struct ScreenSpaceLightingRecordedUse
	{
		// Parent retains/adopts this proof only on accepted queue submission.
		std::uint64_t frameId = 0, settingsRevision = 0, passGeneration = 0;
		ScreenSpaceInputViewOwner owner;
		VkImage outputImage = VK_NULL_HANDLE;
		ScreenSpaceLightingImageState outputState;
		std::uint32_t activeMask = 0; // SSR1, SSGI2, enhanced AO4.
		// Actual recorded mip dispatches, not requested settings or completion.
		bool usedOwnedHiZ = false;
		std::uint32_t hiZMipLevels = 0, ownedHiZDispatches = 0;
	};
	enum class ScreenSpaceLightingRecordResult { Rejected, Skipped, Recorded };

	// Persistent target-generation resources only; preparation accepts allocated
	// images with stamp0 and never certifies current geometry. Demanded scratch,
	// descriptors/pipelines and matrix UBO are created once, never per frame.
	// All-disabled returns null with an empty reason before native allocation.
	std::shared_ptr<ScreenSpaceLightingPass> PrepareScreenSpaceLightingPass(
		const ScreenSpaceLightingCreateInfo& info, std::string& reason);

	// Outside render passes, on the SAME serialized compute-capable graphics
	// queue as geometry. Exact same-command/current-publication receipts and
	// sample1/VRS-off are mandatory. No Vulkan allocation, asset lookup or CPU
	// waits occur here. Borrowed states are restored; output becomes sampled.
	// Retain pass, raster, output and optional HiZ owners through actual completion.
	ScreenSpaceLightingRecordResult RecordScreenSpaceLightingPass(VkCommandBuffer command,
		ScreenSpaceLightingPass& pass, const GraphicsSettings::PostProcessEffectSettings& controls,
		const ScreenSpaceLightingFrameInput& frame, ScreenSpaceLightingRecordedUse& recorded,
		const ScreenSpaceLightingStyle& style = {}, ScreenSpaceLightingStageMarker marker = nullptr,
		void* markerContext = nullptr) noexcept;
}
