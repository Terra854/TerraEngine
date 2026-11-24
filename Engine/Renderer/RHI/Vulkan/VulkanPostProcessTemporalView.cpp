#include "VulkanPostProcessTemporalView.h"
#include "Resource/ResourceManager.h"

#include <cstring>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		std::vector<std::uint32_t> ReadShaderWords(const char* logicalPath)
		{
			// Read through the same package/project authority as other engine
			// effects. Copy into aligned words for VkShaderModule validation.
			auto& manager = ResourceManager::Instance();
			const auto bytes = manager.ReadFileBytes(manager.ResolveEngineAssetPath(logicalPath));
			if(bytes.empty() || bytes.size() % sizeof(std::uint32_t)) return {};
			std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
			std::memcpy(words.data(), bytes.data(), bytes.size());
			return words;
		}
	}

	std::shared_ptr<TemporalViewResources> PrepareTemporalViewResources(
		VkPhysicalDevice physicalDevice, PostProcessTargetBundle& targets,
		const PostProcessTargetImage& sceneColor, const PostProcessTargetImage& sceneDepth,
		const PostProcessTargetImage& resolvedColor, bool cameraOnlyMotion, std::string& reason)
	{
		try
		{
			auto view = std::make_shared<TemporalViewResources>();
			view->device = targets.device;
			view->motion = PrepareMotionRasterTargets(physicalDevice, targets.device, targets.allocator,
				sceneDepth.view, sceneDepth.format, sceneDepth.extent, cameraOnlyMotion, reason);
			if(!view->motion) return {};
			view->motionLayout = CreatePostProcessMotionBufferLayout(targets.device, reason);
			if(!view->motionLayout) return {};
			view->motionVertexShader = ReadShaderWords(cameraOnlyMotion
				? "Shaders/PostProcess/IndexedMotionCamera.vert.spv" : "Shaders/PostProcess/IndexedMotionTotal.vert.spv");
			view->motionFragmentShader = ReadShaderWords(cameraOnlyMotion
				? "Shaders/PostProcess/IndexedMotionCamera.frag.spv" : "Shaders/PostProcess/IndexedMotionTotal.frag.spv");
			if(!ValidateMotionRasterShaders(view->motionVertexShader, view->motionFragmentShader, cameraOnlyMotion, reason))
				return {};
			TemporalPassInputs inputs{sceneColor, sceneDepth, view->motion->totalMotion, resolvedColor,
				VK_IMAGE_ASPECT_DEPTH_BIT, VK_SAMPLE_COUNT_1_BIT, TemporalViewFrameState::kSlotCount};
			view->resolve = PrepareTemporalPass(physicalDevice, targets, inputs, reason);
			if(!view->resolve) return {};
			view->frameState = std::make_shared<TemporalViewFrameState>();
			reason.clear();
			return view;
		}
		catch(const std::exception&)
		{
			reason = "Could not prepare one complete native motion and reconstruction view.";
			return {};
		}
	}

	const MotionRasterPipeline* PrepareTemporalMotionVariant(TemporalViewResources& view,
		std::array<VkDescriptorSetLayout, 2> sourceLayouts, VkCullModeFlags cullMode,
		VkFrontFace frontFace, VkPipelineCache cache, std::string& reason)
	{
		for(const auto& variant : view.variants)
			if(variant.sourceLayouts == sourceLayouts && variant.cullMode == cullMode && variant.frontFace == frontFace)
				return variant.owner.get();
		// Bind the identical descriptor owners used by the qualified indexed
		// surface. The new set is private to this motion pipeline.
		MotionRasterPipelineRequest request;
		request.targets = view.motion;
		request.setLayouts = {sourceLayouts[0], sourceLayouts[1], view.motionLayout->layout};
		request.vertexWords = view.motionVertexShader;
		request.fragmentWords = view.motionFragmentShader;
		request.cullMode = cullMode;
		request.frontFace = frontFace;
		request.cache = cache;
		auto prepared = PrepareMotionRasterPipeline(view.device, request, reason);
		if(!prepared) return nullptr;
		// The target owns every accepted cached variant until its last real
		// submission completes. Do not evict in-flight pipelines to fit more.
		if(view.variants.size() >= 16u)
		{
			reason = "The indexed motion view exceeds its bounded pipeline variants.";
			return nullptr;
		}
		try { view.variants.push_back({sourceLayouts, cullMode, frontFace, std::move(prepared)}); }
		catch(const std::bad_alloc&) { reason = "Could not retain the indexed motion pipeline."; return nullptr; }
		return view.variants.back().owner.get();
	}

	glm::vec2 TemporalJitter(std::uint32_t phase, VkExtent2D extent) noexcept
	{
		// Eight Halton(2,3) points centered on a pixel. Both the opaque draw and
		// the depth-equal motion pass consume this exact normalized shift.
		static constexpr std::array<glm::vec2, 8> points{{
			{0.0f,-1.0f/6.0f}, {-0.25f,1.0f/6.0f}, {0.25f,-7.0f/18.0f}, {-0.375f,-1.0f/18.0f},
			{0.125f,5.0f/18.0f}, {-0.125f,-5.0f/18.0f}, {0.375f,1.0f/18.0f}, {-0.4375f,7.0f/18.0f}
		}};
		if(!extent.width || !extent.height) return {};
		return points[phase % points.size()] / glm::vec2(extent.width, extent.height);
	}

	glm::mat4 ApplyTemporalJitter(glm::mat4 viewProjection, glm::vec2 jitterUV) noexcept
	{
		// Adding clip.w times the NDC offset works for perspective and
		// orthographic cameras without altering depth or the culling matrix.
		for(int column = 0; column < 4; ++column)
		{
			viewProjection[column].x += viewProjection[column].w * (2.0f * jitterUV.x);
			viewProjection[column].y += viewProjection[column].w * (2.0f * jitterUV.y);
		}
		return viewProjection;
	}
}
