// VulkanPostProcessLensHdr.h - independent exposed-HDR dirt and flare draws.
#pragma once
#include "VulkanPostProcessTargets.h"
#include "Utilities/UUID.h"
#include <array>
#include <memory>
#include <string>

struct Texture;
namespace TerraEngine::Renderer::GraphicsSettings { struct LensDirtControls; struct LensFlareControls; }
namespace TerraEngine::Renderer::Vulkan
{
	struct BloomPass;
	struct LensHdrPass;
	struct LensHdrBloomRadiance
	{
		// Must be the actual exposed BLOOM-ONLY upsample output. A composited
		// scene or copied scene color is not a valid bright-source substitute.
		PostProcessTargetImage image;
		std::shared_ptr<const BloomPass> producer;
	};
	struct LensHdrDirtTexture
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		PostProcessTargetImage image;
		std::shared_ptr<const Texture> sourceOwner;
		// A dedicated retained native image owner, not an evictable raw cache view.
		std::shared_ptr<const void> imageOwner;
	};
	struct LensHdrImageState
	{
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		VkAccessFlags access = 0;
	};
	struct LensHdrRecordContext
	{
		std::uint64_t sceneFrameId = 0, producedRadianceFrameId = 0;
		const BloomPass* radianceProducer = nullptr;
		VkImage producedRadianceImage = VK_NULL_HANDLE;
		LensHdrImageState scene, radiance, dirt, output;
	};
	struct LensHdrRecordedUse
	{
		std::uint64_t sceneFrameId = 0;
		VkImage radianceImage = VK_NULL_HANDLE, outputImage = VK_NULL_HANDLE;
		LensHdrImageState outputState;
	};
	enum class LensHdrRecordResult { Rejected, Skipped, Recorded };
	struct LensDirtStyle
	{
		std::array<float, 2> textureScale{1.0f, 1.0f}, textureOffset{};
	};
	struct LensFlareStyle
	{
		float ghostSpacing = 0.3f, haloRadius = 0.3f, haloWidth = 0.08f;
		std::array<float, 3> tint{1.0f, 1.0f, 1.0f};
		std::uint32_t ghostCount = 4;
	};

	// Both stages use real bright-source/down/up radiance, even when ordinary
	// bloom composition is disabled. Preparation performs no GPU commands. The
	// supplied render pass/framebuffer must own exactly this destination, one
	// matching single-sample color attachment, initial COLOR_ATTACHMENT and final
	// SHADER_READ_ONLY. Parent retirement protects every borrowed source/target.
	std::shared_ptr<LensHdrPass> PrepareLensDirtPass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& scene,
		LensHdrBloomRadiance radiance, LensHdrDirtTexture dirt,
		const PostProcessTargetImage& output, VkRenderPass renderPass, VkFramebuffer framebuffer, std::string& reason);
	std::shared_ptr<LensHdrPass> PrepareLensFlarePass(VkPhysicalDevice physical,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& scene,
		LensHdrBloomRadiance radiance, const PostProcessTargetImage& output,
		VkRenderPass renderPass, VkFramebuffer framebuffer, std::string& reason);

	// Copied live controls only. Skipped means zero commands and unchanged
	// output; the graph continues from the original scene input. Parent stamps
	// upstream radiance only after its actual production commands are recorded,
	// then admits this whole primary on real submit. No accepted-state mutation.
	[[nodiscard]] LensHdrRecordResult RecordLensDirtPass(VkCommandBuffer command, LensHdrPass& pass,
		const GraphicsSettings::LensDirtControls& controls, const LensHdrRecordContext& context,
		LensHdrRecordedUse& recorded, const LensDirtStyle& style = {}) noexcept;
	[[nodiscard]] LensHdrRecordResult RecordLensFlarePass(VkCommandBuffer command, LensHdrPass& pass,
		const GraphicsSettings::LensFlareControls& controls, const LensHdrRecordContext& context,
		LensHdrRecordedUse& recorded, const LensFlareStyle& style = {}) noexcept;
}
