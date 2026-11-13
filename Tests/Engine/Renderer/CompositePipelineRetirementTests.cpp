// Real cached composite pipelines retire with actual swapchain passes.
#include "TestHarness.h"
#include "Tests/Support/ReviewRemediation/VulkanReviewFixture.h"
#include "Tests/Support/ReviewRemediation/ExistingRendererFixtureSession.h"
// Impl contains validation-gated fields before every native owner. This TU
// must use the Engine's matching validation ABI; otherwise even a read is unsafe.
#if !defined(TERRAENGINE_RUNTIME_MEMORY_SAFETY_VALIDATION) || !defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
#error Native review Impl access requires the matching Engine validation ABI.
#endif
#include "Renderer/RHI/Vulkan/VulkanContextImpl.h"
#include <fstream>
#include <array>
#include <cstring>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests::Review
{
	// The proposed friend only snapshots copied ownership and invokes the actual
	// production pipeline owner. It does not implement a second cache or retire it.
	struct VulkanLifecycleReviewAccess
	{
		static void ForceZeroStartupExtent(VulkanContext& context, bool enabled) {
			std::lock_guard lock(context.impl->deviceStateMutex);
			context.impl->forceZeroStartupExtentForValidation = enabled;
		}
		static bool HasNativeStartupOwners(VulkanContext& context) {
			std::lock_guard lock(context.impl->deviceStateMutex);
			return context.impl->instance || context.impl->device || context.impl->allocator || context.impl->swapchain;
		}
		// Consume the exact cached production pipeline, without installing another
		// pipeline or changing a production resource's usage. The clone is compatible
		// with the editor's color-only presentation pass; its final transfer layout
		// belongs only to this tiny private image, never a swapchain image.
		static nlohmann::json ConsumeActualComposite(VulkanContext& context,
			const std::filesystem::path& artifacts, unsigned cycle)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
				throw TestFailure("Composite consumption cannot submit an already-invalid native cohort.");
			const auto cached = state.virtualGeometryCompositePipelines.find(state.renderPass);
			const bool bgra = state.swapchainImageFormat == VK_FORMAT_B8G8R8A8_UNORM || state.swapchainImageFormat == VK_FORMAT_B8G8R8A8_SRGB;
			const bool rgba = state.swapchainImageFormat == VK_FORMAT_R8G8B8A8_UNORM || state.swapchainImageFormat == VK_FORMAT_R8G8B8A8_SRGB;
			if (!state.editorMode || state.UsesConventionalVisibilityAttachments(state.renderPass) || (!bgra && !rgba)
				|| cached == state.virtualGeometryCompositePipelines.end() || !cached->second
				|| state.currentFrame >= state.globalDescriptorSets.size() || !state.globalDescriptorSets[state.currentFrame]
				|| state.currentFrame >= state.globalMaterialSSBOs.size() || state.globalMaterialSSBOs[state.currentFrame].size < sizeof(MaterialData)
				|| state.currentFrame >= state.globalFrameCullingSSBOs.size() || state.globalFrameCullingSSBOs[state.currentFrame].size < sizeof(FrameCullingDataGPU))
				throw TestFailure("Composite consumption needs the actual color-only editor pass, RGBA8/BGRA8 format and initialized global material/frame owners.");
			VulkanContext::EditorHandles handles; context.GetEditorHandles(handles);
			const auto getInstance = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
			const auto getDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstance(state.instance, "vkGetDeviceProcAddr"));
			VolkDeviceTable vk{};
			vk.vkCreateRenderPass = reinterpret_cast<PFN_vkCreateRenderPass>(getDevice(state.device,"vkCreateRenderPass"));
			vk.vkDestroyRenderPass = reinterpret_cast<PFN_vkDestroyRenderPass>(getDevice(state.device,"vkDestroyRenderPass"));
			vk.vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(getDevice(state.device,"vkCreateImageView"));
			vk.vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(getDevice(state.device,"vkDestroyImageView"));
			vk.vkCreateFramebuffer = reinterpret_cast<PFN_vkCreateFramebuffer>(getDevice(state.device,"vkCreateFramebuffer"));
			vk.vkDestroyFramebuffer = reinterpret_cast<PFN_vkDestroyFramebuffer>(getDevice(state.device,"vkDestroyFramebuffer"));
			vk.vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(getDevice(state.device,"vkCreateDescriptorPool"));
			vk.vkDestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(getDevice(state.device,"vkDestroyDescriptorPool"));
			vk.vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(getDevice(state.device,"vkAllocateDescriptorSets"));
			vk.vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(getDevice(state.device,"vkUpdateDescriptorSets"));
			vk.vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(getDevice(state.device,"vkCreateCommandPool"));
			vk.vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(getDevice(state.device,"vkDestroyCommandPool"));
			vk.vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(getDevice(state.device,"vkAllocateCommandBuffers"));
			vk.vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(getDevice(state.device,"vkBeginCommandBuffer"));
			vk.vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(getDevice(state.device,"vkEndCommandBuffer"));
			vk.vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(getDevice(state.device,"vkCmdPipelineBarrier"));
			vk.vkCmdBeginRenderPass = reinterpret_cast<PFN_vkCmdBeginRenderPass>(getDevice(state.device,"vkCmdBeginRenderPass"));
			vk.vkCmdEndRenderPass = reinterpret_cast<PFN_vkCmdEndRenderPass>(getDevice(state.device,"vkCmdEndRenderPass"));
			vk.vkCmdSetViewport = reinterpret_cast<PFN_vkCmdSetViewport>(getDevice(state.device,"vkCmdSetViewport"));
			vk.vkCmdSetScissor = reinterpret_cast<PFN_vkCmdSetScissor>(getDevice(state.device,"vkCmdSetScissor"));
			vk.vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(getDevice(state.device,"vkCmdBindPipeline"));
			vk.vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(getDevice(state.device,"vkCmdBindDescriptorSets"));
			vk.vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(getDevice(state.device,"vkCmdPushConstants"));
			vk.vkCmdDrawMeshTasksEXT = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(getDevice(state.device,"vkCmdDrawMeshTasksEXT"));
			vk.vkCmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(getDevice(state.device,"vkCmdCopyImageToBuffer"));
			vk.vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(getDevice(state.device,"vkCreateFence"));
			vk.vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(getDevice(state.device,"vkDestroyFence"));
			vk.vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(getDevice(state.device,"vkWaitForFences"));
			if (!vk.vkCmdDrawMeshTasksEXT) throw TestFailure("Actual composite mesh draw entry point is unavailable.");
			const auto check = [](VkResult result) { if (result != VK_SUCCESS) throw TestFailure("Composite consumption prerequisite failed: " + std::to_string(result)); };
			struct OwnedDraw final
			{
				VkDevice device; VmaAllocator allocator; const VolkDeviceTable& vk;
				VkImage image = VK_NULL_HANDLE; VmaAllocation imageMemory = nullptr;
				VkImageView view = VK_NULL_HANDLE; VkRenderPass pass = VK_NULL_HANDLE; VkFramebuffer framebuffer = VK_NULL_HANDLE;
				VkDescriptorPool descriptors = VK_NULL_HANDLE; VkCommandPool commands = VK_NULL_HANDLE;
				std::array<VulkanBuffer,3> buffers{}; bool pending = false;
				~OwnedDraw()
				{
					if (pending) return;
					if (commands) vk.vkDestroyCommandPool(device,commands,nullptr);
					if (descriptors) vk.vkDestroyDescriptorPool(device,descriptors,nullptr);
					if (framebuffer) vk.vkDestroyFramebuffer(device,framebuffer,nullptr);
					if (pass) vk.vkDestroyRenderPass(device,pass,nullptr);
					if (view) vk.vkDestroyImageView(device,view,nullptr);
					if (image) vmaDestroyImage(allocator,image,imageMemory);
					for (auto& buffer : buffers) if (buffer.buffer) vmaDestroyBuffer(allocator,buffer.buffer,buffer.allocation);
				}
			} owned{state.device,state.allocator,vk};
			constexpr std::uint32_t extent = 4;
			VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D;
			image.format = state.swapchainImageFormat; image.extent = {extent,extent,1}; image.mipLevels = image.arrayLayers = 1;
			image.samples = VK_SAMPLE_COUNT_1_BIT; image.tiling = VK_IMAGE_TILING_OPTIMAL;
			image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			const auto imageProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(getInstance(state.instance,"vkGetPhysicalDeviceImageFormatProperties"));
			VkImageFormatProperties supported{};
			if (!imageProperties || imageProperties(state.physicalDevice,image.format,image.imageType,image.tiling,image.usage,0,&supported) != VK_SUCCESS
				|| !(supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) || supported.maxExtent.width < extent || supported.maxExtent.height < extent)
				throw TestFailure("Actual device does not support the private compatible color/readback image contract.");
			VmaAllocationCreateInfo imageMemory{}; imageMemory.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			check(vmaCreateImage(state.allocator,&image,&imageMemory,&owned.image,&owned.imageMemory,nullptr));
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = owned.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
			check(vk.vkCreateImageView(state.device,&view,nullptr,&owned.view));
			VkAttachmentDescription attachment{}; attachment.format = image.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
			attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
			VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
			subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &reference;
			// Preserve the real editor pass's external dependency too; only the
			// permitted attachment layout/load/store differences belong to the clone.
			VkSubpassDependency dependency{}; dependency.srcSubpass = VK_SUBPASS_EXTERNAL; dependency.dstSubpass = 0;
			dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
			VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pass.attachmentCount = 1; pass.pAttachments = &attachment;
			pass.subpassCount = 1; pass.pSubpasses = &subpass; pass.dependencyCount = 1; pass.pDependencies = &dependency;
			check(vk.vkCreateRenderPass(state.device,&pass,nullptr,&owned.pass));
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; framebuffer.renderPass = owned.pass;
			framebuffer.attachmentCount = 1; framebuffer.pAttachments = &owned.view; framebuffer.width = framebuffer.height = extent; framebuffer.layers = 1;
			check(vk.vkCreateFramebuffer(state.device,&framebuffer,nullptr,&owned.framebuffer));
			// Match only the frozen RasterTriangle input layout. This is controlled
			// input for pipeline compatibility, not fabricated production raster work.
			struct alignas(16) TriangleInput final {
				std::array<std::array<float,4>,16> surface{};
				std::array<std::uint32_t,4> identity{}, offsets{}, shading{}, debug{};
			};
			static_assert(sizeof(TriangleInput) == 320 && offsetof(TriangleInput,identity) == 256);
			TriangleInput triangle;
			for (std::size_t index = 6; index < 9; ++index) triangle.surface[index] = {0,0,1,0};
			for (std::size_t index = 9; index < 12; ++index) triangle.surface[index] = {1,0,0,1};
			triangle.surface[15] = {1,0,0,1}; triangle.identity = {7,0,41,0}; triangle.shading[1] = 1U << 8;
			std::array<std::array<std::uint32_t,4>,extent*extent> visibility;
			for (auto& record : visibility) record = {0,0,0x80000000U,0x56320000U};
			owned.buffers[0] = state.CreateBuffer(sizeof(visibility),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU,"Composite controlled visibility");
			owned.buffers[1] = state.CreateBuffer(sizeof(triangle),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,VMA_MEMORY_USAGE_CPU_TO_GPU,"Composite controlled triangle");
			owned.buffers[2] = state.CreateBuffer(extent*extent*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VMA_MEMORY_USAGE_GPU_TO_CPU,"Composite actual color readback");
			if (!owned.buffers[0].mappedData || !owned.buffers[1].mappedData || !owned.buffers[2].mappedData)
				throw TestFailure("Composite owned upload/readback allocations must be mapped.");
			VkMemoryPropertyFlags readbackProperties = 0;
			vmaGetAllocationMemoryProperties(state.allocator,owned.buffers[2].allocation,&readbackProperties);
			if (!(readbackProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || owned.buffers[2].size < extent*extent*4)
				throw TestFailure("Composite readback must have bounded actual host-visible storage.");
			std::memcpy(owned.buffers[0].mappedData,visibility.data(),sizeof(visibility)); std::memcpy(owned.buffers[1].mappedData,&triangle,sizeof(triangle));
			check(vmaFlushAllocation(state.allocator,owned.buffers[0].allocation,0,sizeof(visibility)));
			check(vmaFlushAllocation(state.allocator,owned.buffers[1].allocation,0,sizeof(triangle)));
			VkDescriptorPoolSize descriptorSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,6};
			VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = pool.poolSizeCount = 1; pool.pPoolSizes = &descriptorSize;
			check(vk.vkCreateDescriptorPool(state.device,&pool,nullptr,&owned.descriptors));
			VkDescriptorSetAllocateInfo sets{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; sets.descriptorPool = owned.descriptors;
			sets.descriptorSetCount = 1; sets.pSetLayouts = &state.virtualGeometryVisibilityDescriptorSetLayout;
			VkDescriptorSet descriptor = VK_NULL_HANDLE; check(vk.vkAllocateDescriptorSets(state.device,&sets,&descriptor));
			std::array<VkDescriptorBufferInfo,6> infos{}; std::array<VkWriteDescriptorSet,6> writes{};
			for (std::size_t index = 0; index < 6; ++index) {
				const auto& buffer = owned.buffers[index == 3 ? 1 : 0]; infos[index] = {buffer.buffer,0,buffer.size};
				writes[index] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[index].dstSet = descriptor;
				writes[index].dstBinding = static_cast<std::uint32_t>(index); writes[index].descriptorCount = 1;
				writes[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[index].pBufferInfo = &infos[index];
			}
			vk.vkUpdateDescriptorSets(state.device,6,writes.data(),0,nullptr);
			const auto families = state.FindQueueFamilies(state.physicalDevice); if (!families.graphicsFamily) throw TestFailure("Actual composite graphics queue unavailable.");
			VkCommandPoolCreateInfo commandPool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; commandPool.queueFamilyIndex = *families.graphicsFamily;
			check(vk.vkCreateCommandPool(state.device,&commandPool,nullptr,&owned.commands));
			nlohmann::json draws = nlohmann::json::array(); bool matches = true;
			for (unsigned variant = 0; variant < 2; ++variant)
			{
				VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocate.commandPool = owned.commands;
				allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocate.commandBufferCount = 1;
				VkCommandBuffer command = VK_NULL_HANDLE; check(vk.vkAllocateCommandBuffers(state.device,&allocate,&command));
				VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
				check(vk.vkBeginCommandBuffer(command,&begin));
				VkMemoryBarrier upload{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; upload.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT; upload.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
				vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&upload,0,nullptr,0,nullptr);
				VkClearValue clear{}; clear.color.float32[2] = clear.color.float32[3] = 1;
				VkRenderPassBeginInfo render{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; render.renderPass = owned.pass; render.framebuffer = owned.framebuffer;
				render.renderArea.extent = {extent,extent}; render.clearValueCount = 1; render.pClearValues = &clear;
				vk.vkCmdBeginRenderPass(command,&render,VK_SUBPASS_CONTENTS_INLINE);
				const VkViewport viewport{0,0,float(extent),float(extent),0,1}; const VkRect2D scissor{{0,0},{extent,extent}};
				vk.vkCmdSetViewport(command,0,1,&viewport); vk.vkCmdSetScissor(command,0,1,&scissor);
				vk.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,cached->second);
				const std::array<VkDescriptorSet,2> descriptorSets{state.globalDescriptorSets[state.currentFrame],descriptor};
				vk.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,state.virtualGeometryVisibilityPipelines.layout,0,2,descriptorSets.data(),0,nullptr);
				const std::array<std::uint32_t,4> constants{extent,extent,41U+variant,0};
				vk.vkCmdPushConstants(command,state.virtualGeometryVisibilityPipelines.layout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,16,constants.data());
				vk.vkCmdDrawMeshTasksEXT(command,1,1,1); vk.vkCmdEndRenderPass(command);
				VkImageMemoryBarrier copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; copy.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; copy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				copy.srcQueueFamilyIndex = copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; copy.image = owned.image; copy.subresourceRange = view.subresourceRange;
				vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&copy);
				VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent = {extent,extent,1};
				vk.vkCmdCopyImageToBuffer(command,owned.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,owned.buffers[2].buffer,1,&region);
				VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
				vk.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
				check(vk.vkEndCommandBuffer(command));
				if (context.GetEmptyFrameValidationReceipt().validationErrorsAfter != 0)
					throw TestFailure("Composite recording validation failed before native submission.");
				VkFence fence = VK_NULL_HANDLE; VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vk.vkCreateFence(state.device,&fenceInfo,nullptr,&fence));
				VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submission.commandBufferCount = 1; submission.pCommandBuffers = &command;
				const auto quarantine = [&](VkResult result, const char* point) noexcept {
					try { std::ofstream receipt(artifacts/("composite-quarantine-"+std::to_string(cycle)+".json"));
						receipt << nlohmann::json{{"point",point},{"result",int(result)},{"cycle",cycle},{"variant",variant},
							{"context",reinterpret_cast<std::uintptr_t>(&context)},{"device",reinterpret_cast<std::uintptr_t>(state.device)},
							{"queue",reinterpret_cast<std::uintptr_t>(state.graphicsQueue)},{"command",reinterpret_cast<std::uintptr_t>(command)},
							{"fence",reinterpret_cast<std::uintptr_t>(fence)},{"completionProven",false}}.dump(2); receipt.flush(); } catch (...) {}
					std::_Exit(2);
				};
				owned.pending = true; VkResult result = VK_ERROR_UNKNOWN;
				// The actual queue gate also excludes asynchronous upload/editor calls.
				try { result = state.SubmitGraphicsQueue(1,&submission,fence); }
				catch (...) { quarantine(VK_ERROR_UNKNOWN,"CompositeSubmitException"); }
				if (result != VK_SUCCESS) quarantine(result,"CompositeSubmitUncertain");
				result = vk.vkWaitForFences(state.device,1,&fence,VK_TRUE,10000000000ULL);
				if (result != VK_SUCCESS) quarantine(result,"CompositeCompletionUnproven");
				owned.pending = false; vk.vkDestroyFence(state.device,fence,nullptr);
				check(vmaInvalidateAllocation(state.allocator,owned.buffers[2].allocation,0,extent*extent*4));
				std::array<std::uint8_t,extent*extent*4> pixels{}; std::memcpy(pixels.data(),owned.buffers[2].mappedData,pixels.size());
				const auto file = "composite-cycle-"+std::to_string(cycle)+(variant ? "-rejected-blue.bin" : "-accepted-red.bin");
				std::ofstream raw(artifacts/file,std::ios::binary); raw.write(reinterpret_cast<const char*>(pixels.data()),pixels.size());
				raw.flush();
				if (!raw) throw TestFailure("Cannot retain actual composite color bytes.");
				bool oracle = true;
				for (std::size_t pixel = 0; pixel < extent*extent; ++pixel) {
					const auto red = pixels[pixel*4+(bgra ? 2 : 0)], blue = pixels[pixel*4+(bgra ? 0 : 2)];
					oracle &= red == (variant ? 0 : 255) && blue == (variant ? 255 : 0) && pixels[pixel*4+1] == 0;
				}
				matches &= oracle; draws.push_back({{"generationMismatchControl",variant!=0},{"actualFenceCompleted",true},
					{"rawColor",file},{"independentColorOraclePassed",oracle}});
			}
			return {{"scope","ControlledInputInstalledCompositePipelineCompatibility"},{"productionPass",reinterpret_cast<std::uintptr_t>(state.renderPass)},
				{"selectedPipeline",reinterpret_cast<std::uintptr_t>(cached->second)},{"format",int(image.format)},
				{"colorOnlySingleSampleCompatibleClone",true},{"draws",draws},{"allColorOraclesPassed",matches}};
		}
		static std::size_t CompositeCount(VulkanContext& context)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			return state.virtualGeometryCompositePipelines.size();
		}
		static std::size_t RetiredPassCount(VulkanContext& context)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			return state.retiredPresentationBundles.size();
		}
		static bool PrimeActualComposite(VulkanContext& context)
		{
			auto& state = *context.impl; std::lock_guard lock(state.deviceStateMutex);
			return state.renderPass != VK_NULL_HANDLE && state.EnsureVulkanVirtualGeometryCompositePipeline(state.renderPass) != VK_NULL_HANDLE;
		}
	};
}
namespace TerraEngine::Tests
{
	void RegisterCompositePipelineRetirementTests(TestRegistry& registry)
	{
		using namespace Review;
		if (!VulkanReviewFixture::IsNativeReviewEnabled()) return;
		registry.Add("Native.ReviewSimulation.Lifecycle.ZeroExtentStartupReleasesAndRetries", [](TestContext& context) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.ZeroExtentStartupReleasesAndRetries";
			if (!VulkanReviewFixture::IsWatchedChild(name)) {
				const auto child = VulkanReviewFixture::RunWatchedSelf(name, std::chrono::seconds(120));
				Review::RecordNativeReviewChild(name, child);
				context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Deferred native startup child must settle: " + child.output); return;
			}
			ExistingRendererFixtureSession session(name);
			session.CloseRenderer();
			VulkanContext native;
			native.SetValidationLayersRequested(true);
			// Force only the surface-capability result; actual instance/device/
			// allocator creation and rollback still execute in the production owner.
			VulkanLifecycleReviewAccess::ForceZeroStartupExtent(native, true);
			for(unsigned retry = 0; retry < 2; ++retry) {
				native.Initialize(WindowSystem::Instance());
				context.Expect(!native.IsReady() && native.IsInitializationDeferred()
					&& !VulkanLifecycleReviewAccess::HasNativeStartupOwners(native), "Zero extent must release partial startup and remain retryable without live native roots.");
			}
			VulkanLifecycleReviewAccess::ForceZeroStartupExtent(native, false);
			native.Initialize(WindowSystem::Instance());
			context.Expect(native.IsReady() && !native.IsInitializationDeferred() && VulkanLifecycleReviewAccess::HasNativeStartupOwners(native), "A later drawable extent must complete real native initialization.");
			native.Shutdown();
			context.Expect(!VulkanLifecycleReviewAccess::HasNativeStartupOwners(native) && session.ValidationErrors() == 0U, "Restored startup and shutdown must leave no roots or Vulkan validation errors.");
		});
		registry.Add("Native.ReviewSimulation.Lifecycle.CompositePipelinesRetireWithPass", [](TestContext& context) {
			const std::string name = "Native.ReviewSimulation.Lifecycle.CompositePipelinesRetireWithPass";
			if (!VulkanReviewFixture::IsWatchedChild(name))
			{
				const auto child = VulkanReviewFixture::RunWatchedSelf(name, std::chrono::seconds(120));
			Review::RecordNativeReviewChild(name, child);
				context.Expect(child.started && !child.timedOut && child.exitCode == 0, "Actual composite retirement child must settle: " + child.output); return;
			}
			ExistingRendererFixtureSession session(name); auto& native = session.Native();
			const auto initial = native.GetEmptyFrameValidationReceipt();
			context.Expect(VulkanLifecycleReviewAccess::PrimeActualComposite(native), "Installed production composite pipeline must actually create for the active pass.");
			nlohmann::json rows = nlohmann::json::array(); bool bounded = true;
			for (unsigned cycle = 0; cycle < 6; ++cycle)
			{
				auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
				glfwSetWindowSize(window, 96 + int(cycle) * 8, 80 + int(cycle) * 8); glfwPollEvents();
				context.Expect(native.RecreateEmptyFrameValidation(false), "Actual protected swapchain recreation must produce a new generation.");
				// Retired present owners need real subsequent acquire/present proof;
				// a bounded actual empty frame pumps that existing lifecycle path.
				for (unsigned frame = 0; frame < 16 && VulkanLifecycleReviewAccess::RetiredPassCount(native); ++frame)
				{
					Renderer::Vulkan::EmptyFrameValidationRequest request; request.forceEmpty = true;
					context.Expect(native.ArmEmptyFrameValidation(request), "Actual empty frame must admit only before acquire.");
					native.BeginFrame(); native.EndFrame();
				}
				const auto retired = VulkanLifecycleReviewAccess::RetiredPassCount(native);
				const auto beforePrime = VulkanLifecycleReviewAccess::CompositeCount(native);
				// No new pass has been primed yet. Once all old pass owners retire,
				// any remaining composite entry necessarily outlives its owning pass.
				bounded &= retired == 0 && beforePrime == 0;
				const bool primed = VulkanLifecycleReviewAccess::PrimeActualComposite(native);
				const auto afterPrime = VulkanLifecycleReviewAccess::CompositeCount(native);
				bounded &= primed && afterPrime == 1;
				rows.push_back({{"cycle",cycle},{"retiredPassOwners",retired},{"cacheBeforeNewPrime",beforePrime},{"newPrimeCreated",primed},{"cacheAfterPrime",afterPrime}});
			}
			// Keep every original runtime/VRS-compatible owner-retirement control
			// above. The public transition now establishes a real color-only editor
			// pass; controlled consuming draws do not claim runtime raster parity.
			native.EnableEditorMode(); nlohmann::json editorRows = nlohmann::json::array();
			for (unsigned cycle = 0; cycle < 2; ++cycle)
			{
				auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());
				glfwSetWindowSize(window,152 + int(cycle)*8,128 + int(cycle)*8); glfwPollEvents();
				context.Expect(native.RecreateEmptyFrameValidation(false), "Public editor-mode resize must recreate the actual color-only pass.");
				for (unsigned frame = 0; frame < 16 && VulkanLifecycleReviewAccess::RetiredPassCount(native); ++frame)
				{
					Renderer::Vulkan::EmptyFrameValidationRequest request; request.forceEmpty = true;
					context.Expect(native.ArmEmptyFrameValidation(request), "Editor retirement pump must admit before real acquire.");
					native.BeginFrame(); native.EndFrame();
				}
				const auto retired = VulkanLifecycleReviewAccess::RetiredPassCount(native);
				const auto beforePrime = VulkanLifecycleReviewAccess::CompositeCount(native);
				context.Expect(retired == 0 && beforePrime == 0, "The prior runtime/editor pass must retire its cache before the consuming editor control.");
				context.Expect(VulkanLifecycleReviewAccess::PrimeActualComposite(native)
					&& VulkanLifecycleReviewAccess::CompositeCount(native) == 1, "Exactly one actual new editor composite pipeline must be selected.");
				const auto consumption = VulkanLifecycleReviewAccess::ConsumeActualComposite(native,session.Artifacts(),cycle);
				bounded &= consumption.at("allColorOraclesPassed").get<bool>();
				editorRows.push_back({{"cycle",cycle},{"retiredPassOwners",retired},{"cacheBeforeNewPrime",beforePrime},{"actualPipelineConsumption",consumption}});
			}
			const auto final = native.GetEmptyFrameValidationReceipt();
			session.CloseRenderer();
			std::ofstream evidence(session.Artifacts()/"composite-retirement.json");
			evidence << nlohmann::json{{"scope","ActualSwapchainRecreationAndCompositeCache"},{"cycles",rows},
				{"controlledConsumingPipelineMode","ActualPublicEditorMode"},{"editorConsumingCycles",editorRows},
				{"validationErrorsBefore",initial.validationErrorsAfter},{"validationErrorsAfter",final.validationErrorsAfter},{"allLifetimeValidationErrors",session.ValidationErrors()}}.dump(2);
			evidence.flush(); context.Expect(static_cast<bool>(evidence), "Actual composite draw and retirement evidence must persist completely.");
			context.Expect(bounded, "Completed pass retirement must remove its actual composite cache owner before selecting a fresh compatible pipeline.");
			context.Expect(final.validationErrorsAfter == initial.validationErrorsAfter && session.ValidationErrors() == 0, "Actual recreation, pipeline creation and teardown cannot introduce Vulkan validation errors.");
		});
	}
}
