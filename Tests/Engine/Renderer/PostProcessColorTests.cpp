// Native color regressions execute the bundled shaders and inspect linear float
// attachments. This deliberately bypasses PNG conversion and display transfer.
#include "TestHarness.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		using Review::VulkanReviewFixture;
		using Review::VulkanReviewImage;
		using Pixel = std::array<float, 4>;
		constexpr std::uint32_t kColorExtent = 4u;

		void ColorVkCheck(VkResult result)
		{
			if(result != VK_SUCCESS) throw std::runtime_error("Native color fixture Vulkan failure: " + std::to_string(result));
		}

		std::filesystem::path ColorEvidenceDirectory()
		{
			static std::atomic_uint64_t next{0u};
			const auto parent = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out/validation/post-processing/color-tests";
			std::filesystem::create_directories(parent);
			for(unsigned attempt = 0; attempt != 100u; ++attempt)
			{
				const auto path = parent / (std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
					+ "-" + std::to_string(next.fetch_add(1u)));
				if(std::filesystem::create_directory(path)) return path;
			}
			throw std::runtime_error("Cannot reserve fresh native color evidence.");
		}

		VkShaderModule ColorModule(VulkanReviewFixture& fixture, const std::vector<std::uint32_t>& words)
		{
			fixture.RequireShaderCapabilities(words);
			const auto& native = fixture.GetNative();
			VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			info.codeSize = words.size() * sizeof(std::uint32_t); info.pCode = words.data();
			VkShaderModule module{};
			ColorVkCheck(native.functions->vkCreateShaderModule(native.device, &info, nullptr, &module));
			fixture.RegisterCleanup([module](const auto& owner) { owner.functions->vkDestroyShaderModule(owner.device, module, nullptr); });
			return module;
		}

		struct ColorDrawResult
		{
			VulkanReviewImage image;
			std::vector<Pixel> pixels;
		};

		// The six rows match the production tone/grade push-constant ABI. Keep
		// defaults explicit so disabling one control cannot enable another one.
		struct alignas(16) ColorToneConstants
		{
			Pixel whiteBalance0{1,0,0,1};
			Pixel whiteBalance1{0,1,0,1};
			Pixel whiteBalance2{0,0,1,0.18f};
			Pixel gain{1,1,1,0};
			Pixel lift{0,0,0,0};
			std::array<std::uint32_t,4> options{};
		};
		static_assert(sizeof(ColorToneConstants) == 96u && offsetof(ColorToneConstants, options) == 80u);

		VulkanReviewImage ConstantColor(VulkanReviewFixture& fixture, Pixel color)
		{
			const auto image = fixture.CreateImage(kColorExtent, kColorExtent, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
			std::array<Pixel, kColorExtent * kColorExtent> pixels; pixels.fill(color);
			fixture.UploadImage(image, std::as_bytes(std::span(pixels)));
			return image;
		}

		bool EnterColorChild(TestContext& context, const std::string& name)
		{
			if(VulkanReviewFixture::IsWatchedChild(name)) return true;
			const auto child = VulkanReviewFixture::RunWatchedSelf(name);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Native color child must complete: " + child.output);
			return false;
		}

		struct ExposureState
		{
			float exposureEV = 0, targetEV = 0, meteredLogLuminance = 0;
			std::uint32_t valid = 0;
		};
		struct ExposureResolveConstants
		{
			float minimumLog = -12, maximumLog = 16, minimumEV = -12, maximumEV = 12;
			float lowPercentile = 0.02f, highPercentile = 0.98f, middleGrey = 0.18f, deltaSeconds = 0.25f;
			float brightenRate = 3, darkenRate = 1;
			std::uint32_t automatic = 1, reset = 0;
		};
		static_assert(sizeof(ExposureState) == 16u && sizeof(ExposureResolveConstants) == 48u);
		struct ExposureObservation
		{
			ExposureState state;
			std::array<std::uint32_t,256> histogram{};
			std::vector<Pixel> exposedPixels;
		};

		// Execute the actual histogram and temporal resolve entries. Explicit
		// transfer/compute/host barriers qualify their buffer data before reading;
		// the CPU supplies only initial history and independently checks results.
		ExposureObservation MeterExposure(VulkanReviewFixture& fixture, float luminance, ExposureState previous,
			ExposureResolveConstants constants, const std::vector<std::uint32_t>& histogramWords,
			const std::vector<std::uint32_t>& resolveWords,
			const std::vector<std::uint32_t>* applicationWords = nullptr, float compensationEV = 0.0f)
		{
			const auto source = ConstantColor(fixture, {luminance,luminance,luminance,1});
			const auto histogram = fixture.CreateBuffer(256u * sizeof(std::uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			const auto prior = fixture.CreateBuffer(sizeof(previous), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, std::as_bytes(std::span(&previous,1)));
			const auto next = fixture.CreateBuffer(sizeof(ExposureState), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			VulkanReviewImage exposed;
			if(applicationWords) exposed = fixture.CreateImage(kColorExtent,kColorExtent,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT);
			const auto& native = fixture.GetNative(); const auto& vk = *native.functions;
			std::array<VkDescriptorSetLayoutBinding,5> bindings{};
			for(std::uint32_t index = 0u; index != bindings.size(); ++index)
				bindings[index] = {index, index == 0u ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : index == 4u ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					1u,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
			VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			setInfo.bindingCount = applicationWords ? 5u : 4u; setInfo.pBindings = bindings.data();
			VkDescriptorSetLayout setLayout{}; ColorVkCheck(vk.vkCreateDescriptorSetLayout(native.device,&setInfo,nullptr,&setLayout));
			fixture.RegisterCleanup([setLayout](const auto& owner) { owner.functions->vkDestroyDescriptorSetLayout(owner.device,setLayout,nullptr); });
			const std::array<VkDescriptorPoolSize,3> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1u},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3u},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1u}}};
			VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolInfo.maxSets = 1u; poolInfo.poolSizeCount = applicationWords ? 3u : 2u; poolInfo.pPoolSizes = sizes.data();
			VkDescriptorPool pool{}; ColorVkCheck(vk.vkCreateDescriptorPool(native.device,&poolInfo,nullptr,&pool));
			fixture.RegisterCleanup([pool](const auto& owner) { owner.functions->vkDestroyDescriptorPool(owner.device,pool,nullptr); });
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocation.descriptorPool = pool; allocation.descriptorSetCount = 1u; allocation.pSetLayouts = &setLayout;
			VkDescriptorSet set{}; ColorVkCheck(vk.vkAllocateDescriptorSets(native.device,&allocation,&set));
			VkDescriptorImageInfo image{VK_NULL_HANDLE,source.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkDescriptorImageInfo storageImage{VK_NULL_HANDLE,exposed.view,VK_IMAGE_LAYOUT_GENERAL};
			const std::array<VkDescriptorBufferInfo,3> buffers{{{histogram.handle,0,histogram.bytes},{prior.handle,0,prior.bytes},{next.handle,0,next.bytes}}};
			std::array<VkWriteDescriptorSet,5> writes{};
			for(std::uint32_t index = 0u; index != setInfo.bindingCount; ++index)
			{
				writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[index].dstSet = set; writes[index].dstBinding = index;
				writes[index].descriptorCount = 1u; writes[index].descriptorType = bindings[index].descriptorType;
				if(index == 0u) writes[index].pImageInfo = &image;
				else if(index == 4u) writes[index].pImageInfo = &storageImage;
				else writes[index].pBufferInfo = &buffers[index - 1u];
			}
			vk.vkUpdateDescriptorSets(native.device,setInfo.bindingCount,writes.data(),0u,nullptr);
			VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0u,sizeof(constants)};
			VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layoutInfo.setLayoutCount = 1u; layoutInfo.pSetLayouts = &setLayout; layoutInfo.pushConstantRangeCount = 1u; layoutInfo.pPushConstantRanges = &push;
			VkPipelineLayout layout{}; ColorVkCheck(vk.vkCreatePipelineLayout(native.device,&layoutInfo,nullptr,&layout));
			fixture.RegisterCleanup([layout](const auto& owner) { owner.functions->vkDestroyPipelineLayout(owner.device,layout,nullptr); });
			auto pipeline = [&](const auto& words, const char* entry) {
				VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; info.layout = layout;
				info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
				info.stage.module = ColorModule(fixture,words); info.stage.pName = entry;
				VkPipeline result{}; ColorVkCheck(vk.vkCreateComputePipelines(native.device,VK_NULL_HANDLE,1u,&info,nullptr,&result));
				fixture.RegisterCleanup([result](const auto& owner) { owner.functions->vkDestroyPipeline(owner.device,result,nullptr); });
				return result;
			};
			const auto histogramPipeline = pipeline(histogramWords,"BuildHistogram"), resolvePipeline = pipeline(resolveWords,"ResolveExposure");
			const auto applicationPipeline = applicationWords ? pipeline(*applicationWords,"ApplyExposure") : VK_NULL_HANDLE;
			Review::VulkanReviewBuffer imageReadback;
			const auto completed = fixture.Submit([&](VkCommandBuffer command, const auto& owner) {
				owner.functions->vkCmdFillBuffer(command,histogram.handle,0u,histogram.bytes,0u);
				VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
				owner.functions->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0u,1u,&barrier,0u,nullptr,0u,nullptr);
				owner.functions->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0u,1u,&set,0u,nullptr);
				owner.functions->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,histogramPipeline);
				struct HistogramConstants { std::uint32_t width,height; float minimumLog,maximumLog; } histogramConstants{kColorExtent,kColorExtent,constants.minimumLog,constants.maximumLog};
				owner.functions->vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0u,sizeof(histogramConstants),&histogramConstants);
				owner.functions->vkCmdDispatch(command,1u,1u,1u);
				barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				owner.functions->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0u,1u,&barrier,0u,nullptr,0u,nullptr);
				owner.functions->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,resolvePipeline);
				owner.functions->vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0u,sizeof(constants),&constants);
				owner.functions->vkCmdDispatch(command,1u,1u,1u);
				if(applicationWords)
				{
					barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
					owner.functions->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0u,1u,&barrier,0u,nullptr,0u,nullptr);
					VkImageMemoryBarrier outputBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
					outputBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT; outputBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
					outputBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL; outputBarrier.image = exposed.handle;
					outputBarrier.srcQueueFamilyIndex = outputBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
					outputBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0u,1u,0u,1u};
					owner.functions->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0u,0u,nullptr,0u,nullptr,1u,&outputBarrier);
					struct ApplicationConstants { std::uint32_t width,height; float compensation; std::uint32_t automatic; } application{kColorExtent,kColorExtent,compensationEV,constants.automatic};
					owner.functions->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,applicationPipeline);
					owner.functions->vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0u,sizeof(application),&application);
					owner.functions->vkCmdDispatch(command,1u,1u,1u);
					imageReadback = fixture.CopyImageToReadback(command,exposed,kColorExtent*kColorExtent*sizeof(Pixel));
				}
				barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
				owner.functions->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0u,1u,&barrier,0u,nullptr,0u,nullptr);
			});
			if(!completed.completed) throw std::runtime_error("Exposure history requires actual completed compute work.");
			ExposureObservation result;
			const auto stateBytes = fixture.Read(next), histogramBytes = fixture.Read(histogram);
			std::memcpy(&result.state,stateBytes.data(),sizeof(result.state));
			std::memcpy(result.histogram.data(),histogramBytes.data(),sizeof(result.histogram));
			if(applicationWords)
			{
				const auto bytes = fixture.Read(imageReadback); result.exposedPixels.resize(kColorExtent*kColorExtent);
				std::memcpy(result.exposedPixels.data(),bytes.data(),bytes.size());
			}
			return result;
		}

		// The fixture owns all native resources until a real fence completes. Each
		// stage has private descriptors and a float target, matching the shader ABI
		// without replacing the production shader's equations with a CPU simulation.
		ColorDrawResult DrawColor(VulkanReviewFixture& fixture, const VulkanReviewImage& source,
			const std::vector<std::uint32_t>& vertexWords, const std::vector<std::uint32_t>& fragmentWords,
			std::span<const std::byte> constants, const VulkanReviewImage* bloom = nullptr)
		{
			const auto& native = fixture.GetNative(); const auto& vk = *native.functions;
			ColorDrawResult result;
			result.image = fixture.CreateImage(kColorExtent, kColorExtent, VK_FORMAT_R32G32B32A32_SFLOAT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
			const std::array<VkDescriptorSetLayoutBinding,2> bindings{{
				{0u, bloom ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
				{1u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
			VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
			setInfo.bindingCount = bloom ? 2u : 1u; setInfo.pBindings = bindings.data();
			VkDescriptorSetLayout setLayout{}; ColorVkCheck(vk.vkCreateDescriptorSetLayout(native.device, &setInfo, nullptr, &setLayout));
			fixture.RegisterCleanup([setLayout](const auto& owner) { owner.functions->vkDestroyDescriptorSetLayout(owner.device, setLayout, nullptr); });
			const std::array<VkDescriptorPoolSize,2> poolSizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1u},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1u}}};
			VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
			poolInfo.maxSets = 1u; poolInfo.poolSizeCount = bloom ? 2u : 1u; poolInfo.pPoolSizes = poolSizes.data();
			VkDescriptorPool pool{}; ColorVkCheck(vk.vkCreateDescriptorPool(native.device, &poolInfo, nullptr, &pool));
			fixture.RegisterCleanup([pool](const auto& owner) { owner.functions->vkDestroyDescriptorPool(owner.device, pool, nullptr); });
			VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
			allocation.descriptorPool = pool; allocation.descriptorSetCount = 1u; allocation.pSetLayouts = &setLayout;
			VkDescriptorSet set{}; ColorVkCheck(vk.vkAllocateDescriptorSets(native.device, &allocation, &set));
			VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
			samplerInfo.magFilter = samplerInfo.minFilter = VK_FILTER_NEAREST;
			samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			VkSampler sampler{}; ColorVkCheck(vk.vkCreateSampler(native.device, &samplerInfo, nullptr, &sampler));
			fixture.RegisterCleanup([sampler](const auto& owner) { owner.functions->vkDestroySampler(owner.device, sampler, nullptr); });
			VkDescriptorImageInfo sampled{sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
			write.dstSet = set; write.dstBinding = 0u; write.descriptorCount = 1u;
			write.descriptorType = bindings[0].descriptorType; write.pImageInfo = &sampled;
			vk.vkUpdateDescriptorSets(native.device, 1u, &write, 0u, nullptr);
			if(bloom)
			{
				VkDescriptorImageInfo sampledBloom{sampler,bloom->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
				write.dstBinding = 1u; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &sampledBloom;
				vk.vkUpdateDescriptorSets(native.device,1u,&write,0u,nullptr);
			}
			VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0u, static_cast<std::uint32_t>(constants.size())};
			VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
			layoutInfo.setLayoutCount = 1u; layoutInfo.pSetLayouts = &setLayout;
			layoutInfo.pushConstantRangeCount = 1u; layoutInfo.pPushConstantRanges = &push;
			VkPipelineLayout layout{}; ColorVkCheck(vk.vkCreatePipelineLayout(native.device, &layoutInfo, nullptr, &layout));
			fixture.RegisterCleanup([layout](const auto& owner) { owner.functions->vkDestroyPipelineLayout(owner.device, layout, nullptr); });
			VkAttachmentDescription attachment{}; attachment.format = result.image.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
			VkAttachmentReference color{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1u; subpass.pColorAttachments = &color;
			VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
			passInfo.attachmentCount = 1u; passInfo.pAttachments = &attachment; passInfo.subpassCount = 1u; passInfo.pSubpasses = &subpass;
			VkRenderPass pass{}; ColorVkCheck(vk.vkCreateRenderPass(native.device, &passInfo, nullptr, &pass));
			fixture.RegisterCleanup([pass](const auto& owner) { owner.functions->vkDestroyRenderPass(owner.device, pass, nullptr); });
			VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebufferInfo.renderPass = pass; framebufferInfo.attachmentCount = 1u; framebufferInfo.pAttachments = &result.image.view;
			framebufferInfo.width = framebufferInfo.height = kColorExtent; framebufferInfo.layers = 1u;
			VkFramebuffer framebuffer{}; ColorVkCheck(vk.vkCreateFramebuffer(native.device, &framebufferInfo, nullptr, &framebuffer));
			fixture.RegisterCleanup([framebuffer](const auto& owner) { owner.functions->vkDestroyFramebuffer(owner.device, framebuffer, nullptr); });
			std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
			stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
			stages[0].module = ColorModule(fixture, vertexWords); stages[0].pName = "VSMain";
			stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
			stages[1].module = ColorModule(fixture, fragmentWords); stages[1].pName = "PSMain";
			VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
			VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
			VkViewport viewport{0, 0, float(kColorExtent), float(kColorExtent), 0, 1}; VkRect2D scissor{{0,0},{kColorExtent,kColorExtent}};
			VkPipelineViewportStateCreateInfo viewportInfo{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
			viewportInfo.viewportCount = viewportInfo.scissorCount = 1u; viewportInfo.pViewports = &viewport; viewportInfo.pScissors = &scissor;
			VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
			raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1.0f;
			VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
			VkPipelineColorBlendAttachmentState blendAttachment{}; blendAttachment.colorWriteMask = 15u;
			VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1u; blend.pAttachments = &blendAttachment;
			VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
			pipelineInfo.stageCount = 2u; pipelineInfo.pStages = stages.data(); pipelineInfo.pVertexInputState = &vertices;
			pipelineInfo.pInputAssemblyState = &assembly; pipelineInfo.pViewportState = &viewportInfo; pipelineInfo.pRasterizationState = &raster;
			pipelineInfo.pMultisampleState = &multisample; pipelineInfo.pColorBlendState = &blend; pipelineInfo.layout = layout; pipelineInfo.renderPass = pass;
			VkPipeline pipeline{}; ColorVkCheck(vk.vkCreateGraphicsPipelines(native.device, VK_NULL_HANDLE, 1u, &pipelineInfo, nullptr, &pipeline));
			fixture.RegisterCleanup([pipeline](const auto& owner) { owner.functions->vkDestroyPipeline(owner.device, pipeline, nullptr); });
			Review::VulkanReviewBuffer readback;
			const auto completion = fixture.Submit([&](VkCommandBuffer command, const auto& owner) {
				VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				begin.renderPass = pass; begin.framebuffer = framebuffer; begin.renderArea = scissor; begin.clearValueCount = 1u; begin.pClearValues = &clear;
				owner.functions->vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
				owner.functions->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				owner.functions->vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0u, 1u, &set, 0u, nullptr);
				owner.functions->vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0u, static_cast<std::uint32_t>(constants.size()), constants.data());
				owner.functions->vkCmdDraw(command, 3u, 1u, 0u, 0u); owner.functions->vkCmdEndRenderPass(command);
				readback = fixture.CopyImageToReadback(command, result.image, kColorExtent * kColorExtent * sizeof(Pixel));
				// Readback transitions to transfer source. Restore a sampled layout so
				// a following real shader stage consumes this exact HDR image.
				VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = result.image.handle; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0u,1u,0u,1u};
				owner.functions->vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
					0u, 0u, nullptr, 0u, nullptr, 1u, &barrier);
			});
			if(!completion.completed) throw std::runtime_error("Native color output requires actual fenced GPU completion.");
			const auto bytes = fixture.Read(readback); result.pixels.resize(kColorExtent * kColorExtent);
			std::memcpy(result.pixels.data(), bytes.data(), bytes.size());
			return result;
		}

		void HdrExposureOnce(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context, name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name, VulkanReviewFixture::NativeDeviceOptions());
			const auto shaderRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto vertex = VulkanReviewFixture::CompileShader(shaderRoot / "../Tests/PostProcessFullscreenFixture.vert.hlsl", "vs_6_0", "VSMain", directory / "fullscreen.spv", {}, true);
			const auto exposure = VulkanReviewFixture::CompileShader(shaderRoot / "PostProcessExposure.frag.hlsl", "ps_6_0", "PSMain", directory / "exposure.spv", {}, true);
			const auto source = fixture.CreateImage(kColorExtent, kColorExtent, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT);
			std::array<Pixel, kColorExtent * kColorExtent> input; input.fill({4.0f, 2.0f, 0.5f, 0.75f});
			fixture.UploadImage(source, std::as_bytes(std::span(input)));
			const float oneEV = 1.0f, identityEV = 0.0f;
			const auto exposed = DrawColor(fixture, source, vertex, exposure, std::as_bytes(std::span(&oneEV, 1)));
			const auto composite = DrawColor(fixture, exposed.image, vertex, exposure, std::as_bytes(std::span(&identityEV, 1)));
			const Pixel expected{8.0f, 4.0f, 1.0f, 0.75f};
			for(std::size_t index = 0; index != exposed.pixels.size(); ++index)
				for(std::size_t channel = 0; channel != 4u; ++channel)
					context.Expect(std::abs(exposed.pixels[index][channel] - expected[channel]) <= 0.0001f
						&& std::abs(composite.pixels[index][channel] - expected[channel]) <= 0.0001f,
						"RGBA32F pre-tone output must preserve HDR energy and alpha, with fixed EV applied once across the exposure/composite boundary.");
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledExposureShader_FloatBeforeTone"},
				{"input",input.front()},{"exposed",exposed.pixels.front()},{"identityComposite",composite.pixels.front()},
				{"tolerance",0.0001},{"sourceFormat","RGBA32F"},{"displayTransferApplied",false}}.dump(2);
		}

		void ToneOperators(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context, name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name, VulkanReviewFixture::NativeDeviceOptions());
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto vertex = VulkanReviewFixture::CompileShader(root / "../Tests/PostProcessFullscreenFixture.vert.hlsl", "vs_6_0", "VSMain", directory / "fullscreen.spv", {}, true);
			const auto tone = VulkanReviewFixture::CompileShader(root / "ToneMap.frag.hlsl", "ps_6_0", "PSMain", directory / "tone.spv", {}, true);
			const auto source = ConstantColor(fixture, {4,4,4,0.4f});
			// These independently evaluated reference points distinguish linear
			// output from a shader that prematurely applies a display transfer.
			constexpr std::array<float,4> expected{4.0f, 0.983255814f, 0.973417110f, 1.0f};
			nlohmann::json observations = nlohmann::json::array();
			for(std::uint32_t mode = 0u; mode != expected.size(); ++mode)
			{
				ColorToneConstants constants; constants.options[0] = mode;
				const auto result = DrawColor(fixture, source, vertex, tone, std::as_bytes(std::span(&constants, 1)));
				for(const auto& pixel : result.pixels)
				{
					for(unsigned channel = 0u; channel != 3u; ++channel)
						context.Expect(std::abs(pixel[channel] - expected[mode]) < 0.00001f,
							"Tone operators must produce their declared linear reference values without premature transfer encoding.");
					context.Expect(std::abs(pixel[3] - 0.4f) < 0.000001f, "Tone operators must preserve alpha.");
				}
				observations.push_back({{"mode",mode},{"pixel",result.pixels.front()},{"expectedLinear",expected[mode]}});
			}
			// Independent double-precision evaluations of the published GT7 SDR
			// curve and ICtCp equations. Colored highlights exercise chroma recovery
			// and paper-white desaturation in addition to the neutral response.
			constexpr std::array<std::pair<Pixel, Pixel>, 6> gt7Swatches{{
				{{0.0f, 0.0f, 0.0f, 0.4f}, {0.0f, 0.0f, 0.0f, 0.4f}},
				{{0.18f, 0.18f, 0.18f, 0.4f}, {0.057950f, 0.057950f, 0.057950f, 0.4f}},
				{{0.75f, 0.75f, 0.75f, 0.4f}, {0.300000f, 0.300000f, 0.300000f, 0.4f}},
				{{3.0f, 0.5f, 0.1f, 0.4f}, {1.0f, 0.196586f, 0.032398f, 0.4f}},
				{{0.1f, 4.0f, 1.2f, 0.4f}, {0.220579f, 1.0f, 0.450741f, 0.4f}},
				{{30.0f, 3.0f, 0.1f, 0.4f}, {1.0f, 1.0f, 0.736134f, 0.4f}}
			}};
			ColorToneConstants gt7;
			gt7.options[0] = 3u;
			nlohmann::json swatches = nlohmann::json::array();
			for(const auto& [input, reference] : gt7Swatches)
			{
				const auto result = DrawColor(fixture, ConstantColor(fixture, input), vertex, tone, std::as_bytes(std::span(&gt7, 1)));
				for(const auto& pixel : result.pixels)
					for(unsigned channel = 0; channel != pixel.size(); ++channel)
						context.Expect(std::abs(pixel[channel] - reference[channel]) < (channel == 3 ? 0.000001f : 0.00035f),
							"GT7 HDR swatches must match the SDR reference in linear float and preserve alpha.");
				swatches.push_back({{"inputLinear",input},{"pixel",result.pixels.front()},{"expectedLinear",reference}});
			}
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledToneShader_LinearFloat"},
				{"operators",observations},{"gt7Swatches",swatches}}.dump(2);
		}

		void GradeIndependence(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context, name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name, VulkanReviewFixture::NativeDeviceOptions());
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto vertex = VulkanReviewFixture::CompileShader(root / "../Tests/PostProcessFullscreenFixture.vert.hlsl", "vs_6_0", "VSMain", directory / "fullscreen.spv", {}, true);
			const auto tone = VulkanReviewFixture::CompileShader(root / "ToneMap.frag.hlsl", "ps_6_0", "PSMain", directory / "tone.spv", {}, true);
			const auto source = ConstantColor(fixture, {4.0f,0.5f,0.75f,0.4f});
			ColorToneConstants constants;
			constants.gain = {2.0f,1.0f,0.5f,0.0f}; constants.lift = {0.1f,0.0f,0.2f,0.0f};
			const auto disabled = DrawColor(fixture, source, vertex, tone, std::as_bytes(std::span(&constants, 1)));
			constants.options[1] = 2u;
			const auto enabled = DrawColor(fixture, source, vertex, tone, std::as_bytes(std::span(&constants, 1)));
			const Pixel identity{4.0f,0.5f,0.75f,0.4f}, expected{8.1f,0.5f,0.575f,0.4f};
			for(std::size_t index = 0; index != enabled.pixels.size(); ++index)
				for(unsigned channel = 0u; channel != 4u; ++channel)
					context.Expect(std::abs(disabled.pixels[index][channel] - identity[channel]) < 0.00001f
						&& std::abs(enabled.pixels[index][channel] - expected[channel]) < 0.00001f,
						"Independent grade controls must leave legacy color unchanged when disabled and apply without enabling tone mapping or clipping HDR when enabled.");
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledGradeShader_IndependentControls"},
				{"disabled",disabled.pixels.front()},{"enabled",enabled.pixels.front()},{"expected",expected}}.dump(2);
		}

		void AutomaticExposure(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context,name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name,VulkanReviewFixture::NativeDeviceOptions());
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto histogram = VulkanReviewFixture::CompileShader(root / "ExposureHistogram.comp.hlsl","cs_6_0","BuildHistogram",directory / "histogram.spv");
			const auto resolve = VulkanReviewFixture::CompileShader(root / "ExposureResolve.comp.hlsl","cs_6_0","ResolveExposure",directory / "resolve.spv");
			ExposureResolveConstants constants;
			const ExposureState previous{0,0,0,1};
			const auto brightening = MeterExposure(fixture,0.01f,previous,constants,histogram,resolve);
			const auto darkening = MeterExposure(fixture,4.0f,previous,constants,histogram,resolve);
			context.Expect(brightening.state.valid == 1u && brightening.state.targetEV > 4.0f && brightening.state.targetEV < 4.4f
				&& darkening.state.valid == 1u && darkening.state.targetEV < -4.0f && darkening.state.targetEV > -4.8f,
				"The actual unexposed histogram must meter dark and HDR bright inputs into opposite bounded exposure targets.");
			context.Expect(std::abs(brightening.state.exposureEV - brightening.state.targetEV * (1.0f - std::exp(-3.0f * 0.25f))) < 0.00001f
				&& std::abs(darkening.state.exposureEV - darkening.state.targetEV * (1.0f - std::exp(-1.0f * 0.25f))) < 0.00001f,
				"Independent brighten/darken rates must use elapsed seconds and the correct direction in actual GPU history.");
			std::uint32_t samples = 0u, occupied = 0u;
			for(const auto count : brightening.histogram) { samples += count; occupied += count != 0u; }
			context.Expect(samples == kColorExtent * kColorExtent && occupied == 1u && brightening.histogram[0] == 0u,
				"A partial 16x16 histogram group must count each real finite source pixel once and exclude out-of-bounds lanes.");
			constants.deltaSeconds = 0.125f;
			const auto half = MeterExposure(fixture,0.01f,previous,constants,histogram,resolve);
			const auto halves = MeterExposure(fixture,0.01f,half.state,constants,histogram,resolve);
			context.Expect(std::abs(halves.state.exposureEV - brightening.state.exposureEV) < 0.00001f,
				"Subdividing the same elapsed time must preserve exposure adaptation for a fixed target.");
			constants.reset = 1u; constants.brightenRate = constants.darkenRate = 0.0f;
			const auto reset = MeterExposure(fixture,0.01f,ExposureState{-8,-8,0,1},constants,histogram,resolve);
			context.Expect(reset.state.valid == 1u && reset.state.exposureEV == reset.state.targetEV,
				"A camera/history reset must discard prior adaptation and snap to the first valid meter even at a zero rate.");
			constants.reset = 0u; constants.automatic = 0u;
			const auto disabled = MeterExposure(fixture,0.01f,brightening.state,constants,histogram,resolve);
			context.Expect(disabled.state.valid == 0u && disabled.state.exposureEV == 0.0f,
				"Disabling automatic exposure must invalidate its temporal history instead of retaining another camera's EV.");
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledHistogramAndResolve_ActualGPUHistory"},
				{"brightenEV",brightening.state.exposureEV},{"brightenTarget",brightening.state.targetEV},
				{"darkenEV",darkening.state.exposureEV},{"darkenTarget",darkening.state.targetEV},
				{"twoHalfStepsEV",halves.state.exposureEV},{"resetEV",reset.state.exposureEV},{"histogramSamples",samples}}.dump(2);
		}

		void BloomCompositeIndependence(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context,name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name,VulkanReviewFixture::NativeDeviceOptions());
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto vertex = VulkanReviewFixture::CompileShader(root / "../Tests/PostProcessFullscreenFixture.vert.hlsl","vs_6_0","VSMain",directory / "fullscreen.spv",{},true);
			const auto composite = VulkanReviewFixture::CompileShader(root / "BloomComposite.frag.hlsl","ps_6_0","PSMain",directory / "bloom.spv",{},true);
			const auto source = ConstantColor(fixture,{4,2,1,0.75f});
			const auto bloom = ConstantColor(fixture,{2,1,0.5f,0});
			struct BloomConstants { float intensity; std::uint32_t enabled,width,height; } constants{0.5f,0u,kColorExtent,kColorExtent};
			static_assert(sizeof(BloomConstants) == 16u);
			const auto disabled = DrawColor(fixture,source,vertex,composite,std::as_bytes(std::span(&constants,1)),&bloom);
			constants.enabled = 1u;
			const auto enabled = DrawColor(fixture,source,vertex,composite,std::as_bytes(std::span(&constants,1)),&bloom);
			constants.intensity = 0.0f;
			const auto zero = DrawColor(fixture,source,vertex,composite,std::as_bytes(std::span(&constants,1)),&bloom);
			const Pixel identity{4,2,1,0.75f}, expected{5,2.5f,1.25f,0.75f};
			for(std::size_t index = 0; index != enabled.pixels.size(); ++index)
				for(unsigned channel = 0u; channel != 4u; ++channel)
					context.Expect(std::abs(disabled.pixels[index][channel] - identity[channel]) < 0.00001f
						&& std::abs(zero.pixels[index][channel] - identity[channel]) < 0.00001f
						&& std::abs(enabled.pixels[index][channel] - expected[channel]) < 0.00001f,
						"Bloom must add its independent intensity once in HDR, preserve alpha, and return identity when disabled or zero without implicitly enabling tone or grading.");
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledBloomComposite_IndependentHDRIntensity"},
				{"disabled",disabled.pixels.front()},{"enabled",enabled.pixels.front()},{"zeroIntensity",zero.pixels.front()}}.dump(2);
		}

		void ExposureApplication(TestContext& context, const std::string& name)
		{
			if(!EnterColorChild(context,name)) return;
			const auto directory = ColorEvidenceDirectory();
			VulkanReviewFixture fixture(name,VulkanReviewFixture::NativeDeviceOptions());
			const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine/Shaders/PostProcess";
			const auto histogram = VulkanReviewFixture::CompileShader(root / "ExposureHistogram.comp.hlsl","cs_6_0","BuildHistogram",directory / "histogram.spv");
			const auto resolve = VulkanReviewFixture::CompileShader(root / "ExposureResolve.comp.hlsl","cs_6_0","ResolveExposure",directory / "resolve.spv");
			const std::array<std::string,2> defines{"-D","EXPOSURE_OUTPUT_RGBA32F"};
			const auto apply = VulkanReviewFixture::CompileShader(root / "ExposureApply.comp.hlsl","cs_6_0","ApplyExposure",directory / "apply.spv",defines);
			ExposureResolveConstants constants; constants.reset = 1u;
			const auto automatic = MeterExposure(fixture,0.01f,{},constants,histogram,resolve,&apply,1.0f);
			const float expected = 0.01f * std::exp2(automatic.state.exposureEV + 1.0f);
			for(const auto& pixel : automatic.exposedPixels)
				context.Expect(std::abs(pixel[0]-expected) < 0.00001f && std::abs(pixel[1]-expected) < 0.00001f
					&& std::abs(pixel[2]-expected) < 0.00001f && pixel[3] == 1.0f,
					"The actual storage shader must apply adapted EV plus fixed compensation exactly once and preserve alpha.");
			constants.automatic = 0u;
			const auto hdr = MeterExposure(fixture,100000.0f,automatic.state,constants,histogram,resolve,&apply,-2.0f);
			for(const auto& pixel : hdr.exposedPixels)
				context.Expect(pixel[0] == 25000.0f && pixel[1] == 25000.0f && pixel[2] == 25000.0f && pixel[3] == 1.0f,
					"RGBA32F source energy above half-float range must survive negative fixed EV; disabled automatic exposure must not apply its old history.");
			fixture.Finish(); fixture.WriteEvidence(directory / "native.json");
			std::ofstream(directory / "comparison.json") << nlohmann::json{{"scope","BundledExposureApply_LinearFloat"},
				{"automaticEV",automatic.state.exposureEV},{"fixedEV",1.0},{"actual",automatic.exposedPixels.front()},
				{"expected",expected},{"highHdrAfterNegativeEV",hdr.exposedPixels.front()}}.dump(2);
		}
	}

	void RegisterPostProcessColorTests(TestRegistry& registry)
	{
		if(!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		const std::string name = "Native.ReviewSimulation.PostProcessColor.HdrAndFixedExposureAppliedOnce";
		registry.Add(name, [name](TestContext& context) { HdrExposureOnce(context, name); });
		const std::string tone = "Native.ReviewSimulation.PostProcessColor.ToneOperatorsRemainLinear";
		registry.Add(tone, [tone](TestContext& context) { ToneOperators(context, tone); });
		const std::string grade = "Native.ReviewSimulation.PostProcessColor.GradingIndependentFromToneAndDisabledControls";
		registry.Add(grade, [grade](TestContext& context) { GradeIndependence(context, grade); });
		const std::string automatic = "Native.ReviewSimulation.PostProcessColor.HistogramTemporalAdaptationAndReset";
		registry.Add(automatic,[automatic](TestContext& context) { AutomaticExposure(context,automatic); });
		const std::string bloom = "Native.ReviewSimulation.PostProcessColor.BloomCompositeIndependentHdrIntensity";
		registry.Add(bloom,[bloom](TestContext& context) { BloomCompositeIndependence(context,bloom); });
		const std::string application = "Native.ReviewSimulation.PostProcessColor.AutomaticAndFixedExposureApplyOnce";
		registry.Add(application,[application](TestContext& context) { ExposureApplication(context,application); });
	}
}
