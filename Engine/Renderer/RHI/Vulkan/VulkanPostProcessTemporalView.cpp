#include "VulkanPostProcessTemporalView.h"
#include "VulkanContextImpl.h"
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
		const PostProcessTargetImage& resolvedColor, bool cameraOnlyMotion, std::string& reason, bool mesh)
	{
		try
		{
			auto view = std::make_shared<TemporalViewResources>();
			view->device = targets.device;
			view->motion = PrepareMotionRasterTargets(physicalDevice, targets.device, targets.allocator,
				sceneDepth.view, sceneDepth.format, sceneDepth.extent, cameraOnlyMotion, reason);
			if(!view->motion) return {};
			view->meshMotion = mesh;
			view->motionLayout = CreatePostProcessMotionBufferLayout(targets.device, reason, mesh);
			if(!view->motionLayout) return {};
			if(mesh) {
				view->motionFragmentShader = ReadShaderWords("Shaders/PostProcess/MeshTemporalMotion.frag.spv");
				if(view->motionFragmentShader.empty()) { reason = "Mesh temporal fragment is missing."; return {}; }
			} else {
				view->compatibilityMeshShader = ReadShaderWords(cameraOnlyMotion
					? "Shaders/PostProcess/IndexedMotionCamera.mesh.spv" : "Shaders/PostProcess/IndexedMotionTotal.mesh.spv");
				view->motionFragmentShader = ReadShaderWords(cameraOnlyMotion
					? "Shaders/PostProcess/IndexedMotionCamera.frag.spv" : "Shaders/PostProcess/IndexedMotionTotal.frag.spv");
				if(view->compatibilityMeshShader.empty() || view->motionFragmentShader.empty()) {
					reason = "Compatibility motion shaders are missing.";
					return {};
				}
			}
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
		request.meshWords = view.compatibilityMeshShader;
		// The push constants select the first meshlet, count and motion row.
		const VkPushConstantRange range{VK_SHADER_STAGE_MESH_BIT_EXT, 0u, 3u * sizeof(std::uint32_t)};
		request.pushConstants = std::span(&range, 1u);
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

	const MotionRasterPipeline* PrepareMeshTemporalMotionVariant(TemporalViewResources& view,
		const VulkanPipeline& surface, std::string& reason)
	{
		if(!view.meshMotion || !surface.sourceCollection || surface.setLayouts.size() != 2u) return nullptr;
		const std::array layouts{surface.setLayouts[0], surface.setLayouts[1]};
		for(const auto& variant : view.variants)
			if(variant.meshCollection == surface.sourceCollection && variant.sourceLayouts == layouts
				&& variant.cullMode == surface.rasterCullMode && variant.frontFace == surface.rasterFrontFace)
				return variant.owner.get();
		MotionRasterPipelineRequest request;
		request.targets = view.motion;
		request.setLayouts = {layouts[0], layouts[1], view.motionLayout->layout};
		request.fragmentWords = view.motionFragmentShader;
		request.cullMode = surface.rasterCullMode; request.frontFace = surface.rasterFrontFace;
		request.pushConstants = surface.pushConstants;
		for(const auto& binding : surface.sourceCollection->stages) {
			if(!binding.shader) continue;
			const auto& shader = *binding.shader;
			const char* entry = shader.stringTable.data() + shader.binary.entryPointOffset;
			if(binding.stage == ShaderStage::Mesh) {
				request.meshWords = shader.meshMotionBytecode; request.meshEntry = entry;
			} else if(binding.stage == ShaderStage::Task && surface.hasTaskStage) {
				request.taskWords = {reinterpret_cast<const std::uint32_t*>(shader.bytecode.data()),
					shader.bytecode.size() / sizeof(std::uint32_t)};
				request.taskEntry = entry;
			}
		}
		if(request.meshWords.empty() || (surface.hasTaskStage && request.taskWords.empty())) return nullptr;
		// Retain the exact collection and pipeline until this view's borrowers retire.
		auto pipeline = PrepareMotionRasterPipeline(view.device, request, reason);
		if(!pipeline || view.variants.size() >= 32u) return nullptr;
		view.variants.push_back({layouts, surface.rasterCullMode, surface.rasterFrontFace,
			std::move(pipeline), surface.sourceCollection});
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
