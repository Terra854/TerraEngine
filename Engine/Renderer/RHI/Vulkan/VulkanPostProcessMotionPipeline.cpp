#include "VulkanPostProcessMotionPipeline.h"
#include <limits>
#include <unordered_map>

namespace TerraEngine::Renderer::Vulkan
{
	namespace
	{
		struct ShaderVariable { std::uint32_t pointerType, storage; };
		// Only the small interface grammar needed by this fixed shader family is
		// inspected. Every instruction is bounds checked before reading operands.
		bool ValidateShader(std::span<const std::uint32_t> words, bool vertex,
			bool camera, std::string& reason)
		{
			auto fail = [&](const char* text) { reason = text; return false; };
			if (words.size() < 5 || reinterpret_cast<std::uintptr_t>(words.data()) % alignof(std::uint32_t)
				|| words.size() > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t)
				|| words[0] != 0x07230203u || words[1] < 0x00010000u || words[1] > 0x00010600u
				|| !words[3] || words[4]) return fail("Motion shader has an invalid SPIR-V header.");
			std::unordered_map<std::uint32_t, std::uint32_t> locations, pointers, vectors, floats;
			std::unordered_map<std::uint32_t, ShaderVariable> variables;
			bool main = false;
			for (std::size_t offset = 5; offset < words.size();)
			{
				const auto count = words[offset] >> 16, opcode = words[offset] & 0xffffu;
				if (!count || count > words.size() - offset) return fail("Motion shader instruction is truncated.");
				const auto* w = words.data() + offset;
				if (opcode == 15) // OpEntryPoint: exactly one expected-stage main.
				{
					if (count < 5 || main || w[1] != (vertex ? 0u : 4u) || w[3] != 0x6e69616du || w[4] != 0u)
						return fail("Motion shader must have one matching main entry point.");
					main = true;
				}
				else if (opcode == 22 && count == 3) floats[w[1]] = w[2]; // OpTypeFloat
				else if (opcode == 23 && count == 4 && w[3] == 4) vectors[w[1]] = w[2];
				else if (opcode == 32 && count == 4) pointers[w[1]] = w[3];
				else if (opcode == 59 && count >= 4)
				{
					if (w[3] == 9) return fail("Motion shaders cannot declare push constants.");
					variables[w[2]] = { w[1], w[3] };
				}
				else if (opcode == 71 && count >= 4)
				{
					if (w[2] == 30 && !locations.emplace(w[1], w[3]).second)
						return fail("Motion shader has duplicate location decorations.");
					// Fragment depth/sample-mask outputs would change source coverage.
					if (!vertex && w[2] == 11 && (w[3] == 22 || w[3] == 20))
						return fail("Motion fragment shader cannot write depth or sample coverage.");
				}
				offset += count;
			}
			if (!main) return fail("Motion shader has no main entry point.");
			std::uint32_t mask = 0;
			for (const auto& [id, variable] : variables)
			{
				const auto location = locations.find(id);
				if (vertex && variable.storage == 1 && location != locations.end())
					return fail("Motion vertex shader must use vertex pulling without attributes.");
				if (vertex || variable.storage != 3) continue;
				if (location == locations.end() || location->second > (camera ? 1u : 0u))
					return fail("Motion fragment output does not match its target count.");
				const auto bit = 1u << location->second;
				if (mask & bit) return fail("Motion fragment output location is duplicated.");
				mask |= bit;
				const auto pointer = pointers.find(variable.pointerType);
				const auto vector = pointer == pointers.end() ? vectors.end() : vectors.find(pointer->second);
				const auto scalar = vector == vectors.end() ? floats.end() : floats.find(vector->second);
				if (scalar == floats.end() || scalar->second != 32)
					return fail("Motion fragment attachments require float4 outputs.");
			}
			return vertex || mask == (camera ? 3u : 1u) || fail("Motion fragment output is missing.");
		}
	}

	bool ValidateMotionRasterShaders(std::span<const std::uint32_t> vertexWords,
		std::span<const std::uint32_t> fragmentWords, bool cameraMotion, std::string& reason)
	{
		reason.clear();
		return ValidateShader(vertexWords, true, cameraMotion, reason)
			&& ValidateShader(fragmentWords, false, cameraMotion, reason);
	}

	MotionRasterPipeline::~MotionRasterPipeline()
	{
		if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
		if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
	}

	std::shared_ptr<MotionRasterPipeline> PrepareMotionRasterPipeline(VkDevice device,
		const MotionRasterPipelineRequest& request, std::string& reason)
	{
		reason.clear();
		if (!device || !request.targets || request.targets->device != device || !request.targets->renderPass
			|| !request.targets->extent.width || !request.targets->extent.height
			|| !request.setLayouts[0] || !request.setLayouts[1] || !request.setLayouts[2]
			|| (request.cullMode & ~VkCullModeFlags(VK_CULL_MODE_FRONT_AND_BACK))
			|| (request.frontFace != VK_FRONT_FACE_CLOCKWISE && request.frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE))
		{
			reason = "Motion pipeline requires exact live targets, three layouts, and valid source raster state.";
			return {};
		}
		if (!ValidateMotionRasterShaders(request.vertexWords, request.fragmentWords, request.targets->usesCameraMotion, reason)) return {};
		auto result = std::make_shared<MotionRasterPipeline>();
		result->device = device;
		result->targets = request.targets;
		VkPipelineLayoutCreateInfo layout{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layout.setLayoutCount = static_cast<std::uint32_t>(request.setLayouts.size());
		layout.pSetLayouts = request.setLayouts.data();
		if (vkCreatePipelineLayout(device, &layout, nullptr, &result->layout) != VK_SUCCESS)
		{
			reason = "Motion pipeline layout creation failed."; return {};
		}
		// Modules are private preparation temporaries; the completed pipeline owns
		// its compiled state, so modules can be released before publication.
		struct Modules
		{
			VkDevice device; std::array<VkShaderModule, 2> handles{};
			~Modules() { for (auto handle : handles) if (handle) vkDestroyShaderModule(device, handle, nullptr); }
		} modules{ device };
		const std::array code{ request.vertexWords, request.fragmentWords };
		std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
		for (std::size_t i = 0; i < code.size(); ++i)
		{
			VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
			info.codeSize = code[i].size_bytes(); info.pCode = code[i].data();
			if (vkCreateShaderModule(device, &info, nullptr, &modules.handles[i]) != VK_SUCCESS)
			{
				reason = "Motion shader module creation failed."; return {};
			}
			stages[i] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
			stages[i].stage = i == 0 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[i].module = modules.handles[i]; stages[i].pName = "main";
		}
		VkPipelineVertexInputStateCreateInfo vertex{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewport.viewportCount = viewport.scissorCount = 1;
		VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = request.cullMode;
		raster.frontFace = request.frontFace; raster.lineWidth = 1.0f;
		VkPipelineMultisampleStateCreateInfo samples{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depth.depthTestEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
		std::array<VkPipelineColorBlendAttachmentState, 2> attachments{};
		for (auto& attachment : attachments) attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
			| VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		blend.attachmentCount = request.targets->usesCameraMotion ? 2u : 1u; blend.pAttachments = attachments.data();
		const std::array dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size()); dynamic.pDynamicStates = dynamicStates.data();
		VkGraphicsPipelineCreateInfo pipeline{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipeline.stageCount = static_cast<std::uint32_t>(stages.size()); pipeline.pStages = stages.data();
		pipeline.pVertexInputState = &vertex; pipeline.pInputAssemblyState = &assembly;
		pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster;
		pipeline.pMultisampleState = &samples; pipeline.pDepthStencilState = &depth;
		pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic;
		pipeline.layout = result->layout; pipeline.renderPass = request.targets->renderPass;
		if (vkCreateGraphicsPipelines(device, request.cache, 1, &pipeline, nullptr, &result->pipeline) != VK_SUCCESS)
		{
			reason = "Motion graphics pipeline creation failed."; return {};
		}
		return result;
	}
}
