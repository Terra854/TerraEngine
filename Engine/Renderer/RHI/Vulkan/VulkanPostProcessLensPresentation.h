// Per-view fullscreen lens styling after AA, before default UI and encoding.
#pragma once
#include "VulkanPostProcessTargets.h"
#include <array>
#include <memory>
#include <string>

namespace TerraEngine::Renderer::Vulkan
{
	struct LensPresentationPass;
	struct LensPresentationControls
	{
		// Each independent switch defaults off. Nonzero authored strengths never
		// enable a stage implicitly; zero amplitudes retain exact shader bypass.
		bool vignetteEnabled = false;
		bool grainEnabled = false;
		bool chromaticEnabled = false;
		bool distortionEnabled = false;
		float vignetteIntensity = 0.35f;
		float vignetteRadius = 0.65f;
		float vignetteSoftness = 0.25f;
		float grainIntensity = 0.06f;
		float grainResponse = 1.0f;
		float chromaticPixels = 2.0f;
		float distortionK1 = 0.05f;
		float distortionK2 = 0.0f;
		std::array<float, 2> opticalCenter{0.5f, 0.5f};
	};
	// Pure validation does not allocate or silently normalize rejected settings.
	bool ValidateLensPresentationControls(const LensPresentationControls& controls) noexcept;

	// Prepare only when the graph explicitly needs presentation lens styling.
	// Source/output are distinct, equal-extent linear Rec.709 images; no encoding
	// or exposure conversion occurs. No images or live settings are owned here.
	// The borrowed pass has one COLOR_ATTACHMENT initial/SHADER_READ final image;
	// the framebuffer must reference destination with that exact compatibility.
	std::shared_ptr<LensPresentationPass> PrepareLensPresentationPass(VkPhysicalDevice physicalDevice,
		PostProcessTargetBundle& targets, const PostProcessTargetImage& source,
		PostProcessTargetImage& destination, VkRenderPass processingPass,
		VkFramebuffer destinationFramebuffer, std::string& reason);

	// Protected render-owner recording only: no allocation, asset lookup, ECS
	// access, implicit enable or mutable per-frame settings retained in the pass.
	// A false result records no commands. Retain all borrowed images and this pass
	// until actual GPU completion. Seed must be copied from this view/frame owner.
	bool RecordLensPresentationPass(VkCommandBuffer command, LensPresentationPass& pass,
		const LensPresentationControls& controls, std::uint32_t frameSeed) noexcept;
}
