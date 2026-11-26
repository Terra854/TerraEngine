// Actual tone/grade draw ownership. Nothing is allocated on the disabled path.
#include "VulkanPostProcessTone.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include "Resource/ResourceManager.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace TerraEngine::Renderer::Vulkan
{
	struct TonePass
	{
		VkDevice device = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkDescriptorSet descriptor = VK_NULL_HANDLE;
		VkPipelineLayout layout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkRenderPass renderPass = VK_NULL_HANDLE; // borrowed compatible pass
		VkFramebuffer framebuffer = VK_NULL_HANDLE; // borrowed destination view
		PostProcessTargetImage* destination = nullptr;
		ToneLookupTextures lookups;
		~TonePass()
		{
			// The enclosing completed target owner proves GPU quiescence. Destroy
			// pipelines and immutable descriptor borrowers before borrowed images.
			if(!device) return;
			if(pipeline) vkDestroyPipeline(device, pipeline, nullptr);
			if(layout) vkDestroyPipelineLayout(device, layout, nullptr);
			if(descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
			if(descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
		}
	};

	namespace
	{
		struct alignas(16) ToneConstants
		{
			std::array<float, 4> whiteBalance0{1, 0, 0, 1};
			std::array<float, 4> whiteBalance1{0, 1, 0, 1};
			std::array<float, 4> whiteBalance2{0, 0, 1, 0.18f};
			std::array<float, 4> gain{1, 1, 1, 0};
			std::array<float, 4> lift{0, 0, 0, 0};
			std::array<std::uint32_t, 4> options{};
		};
		static_assert(sizeof(ToneConstants) == 96 && alignof(ToneConstants) == 16);
		static_assert(offsetof(ToneConstants, whiteBalance1) == 16 && offsetof(ToneConstants, whiteBalance2) == 32);
		static_assert(offsetof(ToneConstants, gain) == 48 && offsetof(ToneConstants, lift) == 64 && offsetof(ToneConstants, options) == 80);

		using Matrix = std::array<std::array<double, 3>, 3>;
		using Vector = std::array<double, 3>;
		Vector Multiply(const Matrix& matrix, const Vector& value) noexcept
		{
			Vector result{};
			for(std::size_t row = 0; row < 3; ++row)
				for(std::size_t column = 0; column < 3; ++column) result[row] += matrix[row][column] * value[column];
			return result;
		}
		Matrix Multiply(const Matrix& left, const Matrix& right) noexcept
		{
			Matrix result{};
			for(std::size_t row = 0; row < 3; ++row)
				for(std::size_t column = 0; column < 3; ++column)
					for(std::size_t inner = 0; inner < 3; ++inner) result[row][column] += left[row][inner] * right[inner][column];
			return result;
		}
		Matrix Inverse(const Matrix& value) noexcept
		{
			// Only the fixed nonsingular Bradford/RGB matrices below reach here.
			const auto& a = value[0]; const auto& b = value[1]; const auto& c = value[2];
			const double determinant = a[0]*(b[1]*c[2]-b[2]*c[1])-a[1]*(b[0]*c[2]-b[2]*c[0])+a[2]*(b[0]*c[1]-b[1]*c[0]);
			return {{{(b[1]*c[2]-b[2]*c[1])/determinant, (a[2]*c[1]-a[1]*c[2])/determinant, (a[1]*b[2]-a[2]*b[1])/determinant},
				{(b[2]*c[0]-b[0]*c[2])/determinant, (a[0]*c[2]-a[2]*c[0])/determinant, (a[2]*b[0]-a[0]*b[2])/determinant},
				{(b[0]*c[1]-b[1]*c[0])/determinant, (a[1]*c[0]-a[0]*c[1])/determinant, (a[0]*b[1]-a[1]*b[0])/determinant}}};
		}
		std::array<double, 2> PlanckianXy(double kelvin) noexcept
		{
			// Kang 2002 Planckian-locus approximation, valid 1667..25000K. Only
			// validated engine 2000..15000K controls are used. Published equations:
			// https://colour.readthedocs.io/en/develop/_modules/colour/temperature/kang2002.html
			const double t = 1.0 / kelvin;
			const double x = kelvin <= 4000.0
				? -0.2661239e9*t*t*t - 0.2343589e6*t*t + 0.8776956e3*t + 0.179910
				: -3.0258469e9*t*t*t + 2.1070379e6*t*t + 0.2226347e3*t + 0.240390;
			const double y = kelvin <= 2222.0 ? -1.1063814*x*x*x - 1.34811020*x*x + 2.18555832*x - 0.20219683
				: kelvin <= 4000.0 ? -0.9549476*x*x*x - 1.37418593*x*x + 2.09137015*x - 0.16748867
				: 3.0817580*x*x*x - 5.8733867*x*x + 3.75112997*x - 0.37001483;
			return {x, y};
		}
		Matrix WhiteBalance(float kelvin, float tint) noexcept
		{
			// Explicit identity keeps the default signal exact, without matrix noise.
			if(kelvin == 6500.0f && tint == 0.0f) return {{{1,0,0},{0,1,0},{0,0,1}}};
			const auto source = PlanckianXy(kelvin), neutral = PlanckianXy(6500.0);
			// Calibrate the locus offset to Rec.709's actual D65 white point. Tint
			// shifts the inferred illuminant along CIE 1960 v; positive tint removes
			// green from the image. The bounded 0.005 shift keeps every supported
			// illuminant inside physical xy. Bradford then adapts source white to D65.
			const double x = 0.3127 + source[0] - neutral[0];
			const double y = 0.3290 + source[1] - neutral[1];
			const double denominator = -2*x + 12*y + 3;
			const double u = 4*x / denominator, v = 6*y / denominator + double(tint)*0.005;
			const double inverse = 1.0 / (2*u - 8*v + 4);
			const double sx = 3*u*inverse, sy = 2*v*inverse;
			const Vector white{sx/sy, 1.0, (1.0-sx-sy)/sy};
			const Vector d65{0.3127/0.3290, 1.0, (1.0-0.3127-0.3290)/0.3290};
			// Bradford cone transform and linear Rec.709 RGB->XYZ, row-major.
			// https://colour.readthedocs.io/en/latest/generated/colour.adaptation.datasets.cat.CAT_BRADFORD.html
			const Matrix bradford{{{0.8951,0.2664,-0.1614},{-0.7502,1.7135,0.0367},{0.0389,-0.0685,1.0296}}};
			const Matrix rgbToXyz{{{0.4123907993,0.3575843394,0.1804807884},{0.2126390059,0.7151686788,0.0721923154},{0.0193308187,0.1191947798,0.9505321522}}};
			const auto sourceCone = Multiply(bradford, white), targetCone = Multiply(bradford, d65);
			Matrix scale{};
			for(std::size_t channel = 0; channel < 3; ++channel) scale[channel][channel] = targetCone[channel] / sourceCone[channel];
			return Multiply(Inverse(rgbToXyz), Multiply(Inverse(bradford), Multiply(scale, Multiply(bradford, rgbToXyz))));
		}

		float FiniteControl(float value, float fallback, float minimum, float maximum) noexcept
		{
			return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
		}
		ToneConstants MakeConstants(const TonePass& pass, const GraphicsSettings::PostProcessColorSettings& settings) noexcept
		{
			ToneConstants constants;
			// The uniform selects the operator within the existing tone and grade pass.
			constants.options[0] = settings.tone <= GraphicsSettings::ToneMappingMode::GT7 ? static_cast<std::uint32_t>(settings.tone) : 0u;
			if(!settings.grading) return constants;
			const auto matrix = WhiteBalance(FiniteControl(settings.temperature, 6500, 2000, 15000), FiniteControl(settings.tint, 0, -1, 1));
			for(std::size_t column = 0; column < 3; ++column)
			{
				constants.whiteBalance0[column] = static_cast<float>(matrix[0][column]);
				constants.whiteBalance1[column] = static_cast<float>(matrix[1][column]);
				constants.whiteBalance2[column] = static_cast<float>(matrix[2][column]);
			}
			constants.whiteBalance0[3] = FiniteControl(settings.contrast, 1, 0, 3);
			constants.whiteBalance1[3] = FiniteControl(settings.saturation, 1, 0, 3);
			const float gain = FiniteControl(settings.gain, 1, 0, 8), lift = FiniteControl(settings.lift, 0, -1, 1);
			constants.gain = {gain, gain, gain, FiniteControl(settings.curveStrength, 1, 0, 1)};
			constants.lift = {lift, lift, lift, FiniteControl(settings.lutStrength, 1, 0, 1)};
			constants.options[1] = 1u | 2u;
			if(pass.lookups.curve && settings.curve != TerraEngine::EMPTY_UUID) constants.options[1] |= 4u;
			if(pass.lookups.lut && settings.lut != TerraEngine::EMPTY_UUID) constants.options[1] |= 8u;
			constants.options[2] = pass.lookups.curveWidth;
			constants.options[3] = pass.lookups.lutEdge;
			return constants;
		}

		struct ShaderModules
		{
			VkDevice device;
			VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
			~ShaderModules() { if(vertex) vkDestroyShaderModule(device, vertex, nullptr); if(fragment) vkDestroyShaderModule(device, fragment, nullptr); }
		};
		VkShaderModule LoadModule(VkDevice device, const char* path)
		{
			auto& resources = ResourceManager::Instance();
			const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(path));
			if(bytes.empty() || bytes.size() % 4 != 0) return VK_NULL_HANDLE;
			std::vector<std::uint32_t> words(bytes.size()/4);
			std::memcpy(words.data(), bytes.data(), bytes.size());
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = bytes.size(); info.pCode = words.data();
			VkShaderModule module = VK_NULL_HANDLE;
			return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
		}
		bool CreatePipeline(TonePass& pass, bool lookups)
		{
			ShaderModules shaders{pass.device};
			shaders.vertex = LoadModule(pass.device, "Shaders/PostProcess/PostProcessFullscreen.mesh.spv");
			shaders.fragment = LoadModule(pass.device, lookups ? "Shaders/PostProcess/ToneMapLookups.frag.spv" : "Shaders/PostProcess/ToneMap.frag.spv");
			if(!shaders.vertex || !shaders.fragment) return false;
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			for(auto& stage : stages) stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[0].stage=VK_SHADER_STAGE_MESH_BIT_EXT; stages[0].module=shaders.vertex; stages[0].pName="main";
			stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=shaders.fragment; stages[1].pName="PSMain";
			VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=1; viewport.scissorCount=1;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth=1;
			VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
			VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
			VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&attachment;
			const std::array<VkDynamicState, 2> states{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
			VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=states.data();
			VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			info.stageCount=2; info.pStages=stages.data(); info.pViewportState=&viewport;
			info.pRasterizationState=&raster; info.pMultisampleState=&samples; info.pDepthStencilState=&depth; info.pColorBlendState=&blend; info.pDynamicState=&dynamic;
			info.layout=pass.layout; info.renderPass=pass.renderPass;
			return vkCreateGraphicsPipelines(pass.device,VK_NULL_HANDLE,1,&info,nullptr,&pass.pipeline)==VK_SUCCESS;
		}
	}

	std::shared_ptr<TonePass> PrepareTonePass(PostProcessTargetBundle& targets,
		const PostProcessTargetImage& source, PostProcessTargetImage& destination,
		VkRenderPass processingPass, VkFramebuffer destinationFramebuffer,
		ToneLookupTextures lookups, std::string& reason)
	{
		reason.clear();
		if(!targets.device || !targets.sampler || !source.image || !source.view || !destination.image || !destination.view
			|| source.image == destination.image || !processingPass || !destinationFramebuffer
			|| !source.extent.width || !source.extent.height || !destination.extent.width || !destination.extent.height
			|| !(source.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(destination.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
			|| !(destination.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
			|| lookups.curve == destination.view || lookups.lut == destination.view
			|| (lookups.curve && lookups.curveWidth < 2u) || (lookups.lut && lookups.lutEdge < 2u))
		{
			reason="Tone/grading needs distinct sampled source and color destination, a compatible framebuffer, and validated lookup dimensions.";
			return {};
		}
		try
		{
			auto pass=std::make_shared<TonePass>();
			pass->device=targets.device; pass->renderPass=processingPass; pass->framebuffer=destinationFramebuffer; pass->destination=&destination; pass->lookups=lookups;
			const bool withLookups=lookups.curve || lookups.lut;
			const std::uint32_t count=withLookups ? 3u : 1u;
			std::array<VkDescriptorSetLayoutBinding,3> bindings{};
			for(std::uint32_t index=0;index<count;++index) bindings[index]={index,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
			VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; descriptor.bindingCount=count; descriptor.pBindings=bindings.data();
			if(vkCreateDescriptorSetLayout(pass->device,&descriptor,nullptr,&pass->descriptorLayout)!=VK_SUCCESS) throw std::runtime_error("Tone descriptor layout creation failed.");
			VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,count};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
			if(vkCreateDescriptorPool(pass->device,&pool,nullptr,&pass->descriptorPool)!=VK_SUCCESS) throw std::runtime_error("Tone descriptor pool creation failed.");
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocation.descriptorPool=pass->descriptorPool; allocation.descriptorSetCount=1; allocation.pSetLayouts=&pass->descriptorLayout;
			if(vkAllocateDescriptorSets(pass->device,&allocation,&pass->descriptor)!=VK_SUCCESS) throw std::runtime_error("Tone source descriptor allocation failed.");
			const std::array<VkImageView,3> views{source.view,lookups.curve ? lookups.curve : source.view,lookups.lut ? lookups.lut : source.view};
			std::array<VkDescriptorImageInfo,3> images{}; std::array<VkWriteDescriptorSet,3> writes{};
			for(std::uint32_t index=0;index<count;++index)
			{
				images[index]={targets.sampler,views[index],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
				writes[index]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[index].dstSet=pass->descriptor; writes[index].dstBinding=index;
				writes[index].descriptorCount=1; writes[index].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[index].pImageInfo=&images[index];
			}
			vkUpdateDescriptorSets(pass->device,count,writes.data(),0,nullptr);
			VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(ToneConstants)};
			VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&pass->descriptorLayout; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
			if(vkCreatePipelineLayout(pass->device,&layout,nullptr,&pass->layout)!=VK_SUCCESS || !CreatePipeline(*pass,withLookups)) throw std::runtime_error("Installed tone/grading shaders or compatible pipeline creation failed.");
			return pass;
		}
		catch(const std::exception& error) { reason=error.what(); return {}; }
	}

	void RecordTonePass(VkCommandBuffer command, TonePass& pass, const GraphicsSettings::PostProcessColorSettings& settings)
	{
		if(!command || !pass.pipeline || !pass.destination) return;
		auto& destination=*pass.destination;
		// Fully overwrite this output. UNDEFINED avoids trusting layout metadata
		// from commands that were recorded but never admitted to a real queue.
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
		barrier.image=destination.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=pass.renderPass; begin.framebuffer=pass.framebuffer; begin.renderArea.extent=destination.extent;
		vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
		VkViewport viewport{0,0,float(destination.extent.width),float(destination.extent.height),0,1}; VkRect2D scissor{{0,0},destination.extent};
		vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
		const auto constants=MakeConstants(pass,settings);
		vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pass.pipeline);
		vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pass.layout,0,1,&pass.descriptor,0,nullptr);
		vkCmdPushConstants(command,pass.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(constants),&constants);
		vkCmdDrawMeshTasksEXT(command, 1, 1, 1);
		vkCmdEndRenderPass(command);
		// The compatible render pass performs the final layout transition. Make
		// its completed color writes visible to the next fragment/compute reader.
		barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		destination.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}
}
