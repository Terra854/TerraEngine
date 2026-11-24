// VulkanPostProcessLookups.cpp — exact immutable mip-zero grading uploads.
#include "VulkanResourceSource.h"
#include "VulkanPostProcessLookups.h"
#include "Renderer/GraphicsSettings/PostProcessColorSettings.h"
#include <cmath>
#include <stdexcept>
#include <limits>

namespace TerraEngine::Renderer::Vulkan
{
	PostProcessDirtImage::~PostProcessDirtImage()
	{
		// The enclosing lens pass retires after its real accepted GPU borrowers.
		if(image.view && device) vkDestroyImageView(device,image.view,nullptr);
		if(image.image && allocator) vmaDestroyImage(allocator,image.image,image.allocation);
	}
	PostProcessLookupImages::~PostProcessLookupImages()
	{
		// Caller supplies GPU retirement, just as for the enclosing tone pass.
		for(auto& image : images)
		{
			if(image.view && device) vkDestroyImageView(device, image.view, nullptr);
			if(image.image && allocator) vmaDestroyImage(allocator, image.image, image.allocation);
		}
	}
}

namespace
{
	struct LookupUpload
	{
		VkFormat format = VK_FORMAT_UNDEFINED;
		VkExtent2D extent{};
		const std::uint8_t* bytes = nullptr;
		VkDeviceSize size = 0;
	};
	// Integer-sampled, sRGB and compressed assets cannot represent the intended
	// linear RGBA lookup contract. Preserve accepted bits rather than converting
	// or reducing the source precision on this renderer preparation path.
	std::uint32_t LookupPixelBytes(VkFormat format) noexcept
	{
		switch(format)
		{
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_UNORM: return 4;
		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
		case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
		default: return 0;
		}
	}
	bool StageLookupBytes(VmaAllocator allocator, const VulkanBuffer& buffer, const LookupUpload& upload)
	{
		if(!buffer.buffer || !buffer.allocation) return false;
		void* mapped = nullptr;
		if(vmaMapMemory(allocator, buffer.allocation, &mapped) != VK_SUCCESS) return false;
		std::memcpy(mapped, upload.bytes, static_cast<std::size_t>(upload.size));
		// Check flush as well as mapping: non-coherent staging must be visible
		// before submission. The shared WriteMappedBuffer helper ignores this
		// result, so the lookup transaction explicitly proves it here.
		const auto flushed = vmaFlushAllocation(allocator, buffer.allocation, 0, upload.size);
		vmaUnmapMemory(allocator, buffer.allocation);
		return flushed == VK_SUCCESS;
	}
	bool ValidateLookup(const Texture& texture, std::optional<bool> curve, LookupUpload& upload, std::string& reason)
	{
		const auto& description = texture.descriptor;
		const auto width = static_cast<std::uint32_t>(description.width);
		const auto height = static_cast<std::uint32_t>(description.height);
		const auto format = static_cast<VkFormat>(description.vkFormat);
		const auto pixelBytes = LookupPixelBytes(format);
		if(texture.publicationGeneration == 0 || description.mipCount == 0 || texture.levelTable.empty()
			|| description.depth != 1 || description.layers != 1 || description.faces != 1
			|| (texture.header.flags & 0x1fu) != 0 || description.semantic == static_cast<std::uint8_t>(TextureFileSemantic::Color)
			|| pixelBytes == 0 || !width || !height
			|| std::uint64_t(width) > std::numeric_limits<std::uint64_t>::max() / height / pixelBytes)
		{
			reason = "Post-process linear RGBA texture requires a published linear, non-premultiplied 2D RGBA UNORM/float texture with mip zero.";
			return false;
		}
		// Curve/LUT callers retain their exact dimensional contracts. Dirt shares
		// the checked byte/encoding path without inventing a lookup shape.
		if(curve && ((*curve && (height != 1 || width < 2)) || (!*curve && (height < 2 || std::uint64_t(height) * height != width))))
		{
			reason = *curve ? "Grading curve must have at least two RGBA samples in one row."
				: "Grading LUT must use horizontal blue slices with width N squared and height N, N at least two.";
			return false;
		}
		const std::uint64_t expected = std::uint64_t(width) * height * pixelBytes;
		const auto& mip = texture.levelTable.front();
		if(description.blockWidth != 1 || description.blockHeight != 1 || description.blockBytes != pixelBytes
			|| mip.size != expected || mip.offset > texture.transcodedData.size()
			|| mip.size > texture.transcodedData.size() - mip.offset)
		{
			reason = "Post-process linear RGBA mip zero is missing or does not match its complete uncompressed source dimensions.";
			return false;
		}
		const auto* bytes = texture.transcodedData.data() + static_cast<std::size_t>(mip.offset);
		// Non-finite authored lookup values would poison all later color math.
		// Validation never clamps finite HDR or negative values in float assets.
		if(format == VK_FORMAT_R32G32B32A32_SFLOAT)
		{
			for(std::size_t offset = 0; offset < expected; offset += sizeof(float))
			{
				float value; std::memcpy(&value, bytes + offset, sizeof(value));
				if(!std::isfinite(value)) { reason = "Post-process linear RGBA texture contains a non-finite float sample."; return false; }
			}
		}
		else if(format == VK_FORMAT_R16G16B16A16_SFLOAT)
		{
			for(std::size_t offset = 0; offset < expected; offset += sizeof(std::uint16_t))
			{
				std::uint16_t value; std::memcpy(&value, bytes + offset, sizeof(value));
				if((value & 0x7c00u) == 0x7c00u) { reason = "Post-process linear RGBA texture contains a non-finite half-float sample."; return false; }
			}
		}
		upload = {format, {width, height}, bytes, expected}; return true;
	}
}

std::shared_ptr<TerraEngine::Renderer::Vulkan::PostProcessLookupImages>
VulkanContext::Impl::PreparePostProcessLookups(
	const TerraEngine::Renderer::GraphicsSettings::PostProcessColorSettings& settings, std::string& reason)
{
	using namespace TerraEngine::Renderer::Vulkan;
	reason.clear();
	// Strength is a live uniform. Prepare every referenced asset while grading
	// is enabled, so changing zero strength cannot expose an unowned descriptor.
	const std::array requested{settings.grading && settings.curve != TerraEngine::EMPTY_UUID,
		settings.grading && settings.lut != TerraEngine::EMPTY_UUID};
	if(!requested[0] && !requested[1]) return {};
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	if(!initialized || deviceLost || !device || !allocator)
	{ reason = "Native grading lookup device is unavailable."; return {}; }
	try
	{
		auto result = std::make_shared<PostProcessLookupImages>();
		result->device = device; result->allocator = allocator;
		const std::array ids{settings.curve, settings.lut};
		std::array<LookupUpload, 2> uploads;
		auto& resources = ResourceManager::Instance();
		constexpr auto usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		for(std::size_t index = 0; index < requested.size(); ++index)
		{
			if(!requested[index]) continue;
			result->sourceOwners[index] = resources.AcquireTextureSnapshot(ids[index]);
			if(!result->sourceOwners[index]) { reason = "Referenced grading lookup is not loaded."; return {}; }
			if(!ValidateLookup(*result->sourceOwners[index], index == 0, uploads[index], reason)) return {};
			VkFormatProperties features{};
			vkGetPhysicalDeviceFormatProperties(physicalDevice, uploads[index].format, &features);
			PostProcessFormatSupport support; support.optimalFeatures = features.optimalTilingFeatures;
			support.imageQueryResult = vkGetPhysicalDeviceImageFormatProperties(physicalDevice, uploads[index].format,
				VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &support.imageProperties);
			if(!QualifiesPostProcessFormat(support, uploads[index].extent, usage)
				|| uploads[index].size > support.imageProperties.maxResourceSize)
			{ reason = "Device cannot linearly sample and upload the complete grading lookup format and dimensions."; return {}; }
		}
		// Reserve all ambiguous ownership receipts before native submission. The
		// failure cleanup below never allocates after a possibly accepted command.
		ambiguousUploadBuffers.reserve(ambiguousUploadBuffers.size() + requested.size());
		ambiguousUploadImages.reserve(ambiguousUploadImages.size() + requested.size());
		std::array<VulkanImage, 2> images{};
		std::array<VulkanBuffer, 2> staging{};
		struct UploadOwner
		{
			Impl& context;
			std::array<VulkanImage, 2>& images;
			std::array<VulkanBuffer, 2>& staging;
			bool mayBePending = false;
			~UploadOwner() noexcept
			{
				for(std::size_t index = 0; index < images.size(); ++index)
				{
					if(mayBePending)
					{
						context.RetainAmbiguousUpload(staging[index]);
						context.RetainAmbiguousUpload(images[index]);
					}
					else
					{
						context.DestroyBufferImmediate(staging[index]);
						if(images[index].view) vkDestroyImageView(context.device, images[index].view, nullptr);
						if(images[index].image) vmaDestroyImage(context.allocator, images[index].image, images[index].allocation);
					}
				}
			}
		} candidate{*this, images, staging};
		for(std::size_t index = 0; index < requested.size(); ++index)
		{
			if(!requested[index]) continue;
			const auto& upload = uploads[index];
			images[index].sourceOwner = result->sourceOwners[index];
			staging[index] = CreateBuffer(upload.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY, "Post-process Lookup Staging");
			if(!staging[index].buffer || !StageLookupBytes(allocator, staging[index], upload))
			{ reason = "Cannot prepare grading lookup staging bytes."; return {}; }
			VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
			info.imageType = VK_IMAGE_TYPE_2D; info.extent = {upload.extent.width, upload.extent.height, 1};
			info.mipLevels = 1; info.arrayLayers = 1; info.format = upload.format;
			info.tiling = VK_IMAGE_TILING_OPTIMAL; info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			info.usage = usage; info.samples = VK_SAMPLE_COUNT_1_BIT; info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			VmaAllocationCreateInfo allocation{}; allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
			if(vmaCreateImage(allocator, &info, &allocation,
				&images[index].image, &images[index].allocation, nullptr) != VK_SUCCESS)
			{ reason = "Cannot allocate immutable grading lookup image."; return {}; }
			images[index].view = CreateImageView(images[index].image, upload.format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
			if(!images[index].view) { reason = "Cannot create grading lookup image view."; return {}; }
		}
		const auto command = BeginSingleTimeCommands();
		if(!command) { reason = "Cannot begin grading lookup upload."; return {}; }
		for(std::size_t index = 0; index < requested.size(); ++index)
		{
			if(!requested[index]) continue;
			VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
			barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.image = images[index].image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
			VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			region.imageExtent = {uploads[index].extent.width, uploads[index].extent.height, 1};
			vkCmdCopyBufferToImage(command, staging[index].buffer, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
			barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
			vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				0, 0, nullptr, 0, nullptr, 1, &barrier);
		}
		candidate.mayBePending = true;
		if(!EndSingleTimeCommands(command, "post-process lookup upload"))
		{ reason = "Grading lookup upload completion is unproven; resources remain retired behind native idle proof."; return {}; }
		candidate.mayBePending = false;
		for(std::size_t index = 0; index < requested.size(); ++index)
		{
			if(!requested[index]) continue;
			if(resources.AcquireTextureSnapshot(ids[index]) != result->sourceOwners[index])
			{ reason = "Grading lookup publication changed during upload; retry the immutable candidate."; return {}; }
			auto& output = result->images[index];
			output.image = images[index].image; output.allocation = images[index].allocation; output.view = images[index].view;
			output.extent = uploads[index].extent; output.format = uploads[index].format;
			output.usage = usage; output.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			images[index] = {};
		}
		if(requested[0]) { result->views.curve = result->images[0].view; result->views.curveWidth = uploads[0].extent.width; }
		if(requested[1]) { result->views.lut = result->images[1].view; result->views.lutEdge = uploads[1].extent.height; }
		return result;
	}
	catch(const std::exception& failure) { reason = std::string("Grading lookup preparation failed: ") + failure.what(); return {}; }
}

std::optional<TerraEngine::Renderer::Vulkan::LensHdrDirtTexture>
VulkanContext::Impl::PreparePostProcessLensDirtTexture(bool active,
	const TerraEngine::UUID& assetId, std::string& reason)
{
	using namespace TerraEngine::Renderer::Vulkan;
	reason.clear();
	// Activity is the selected feature, not its live intensity. Enabled dirt at
	// zero intensity still needs its actual asset prepared for later live changes.
	if(!active) return LensHdrDirtTexture{};
	if(assetId==TerraEngine::EMPTY_UUID) { reason="Enabled lens dirt has no authored texture asset.";return {}; }
	std::lock_guard<std::recursive_mutex> lock(deviceStateMutex);
	if(!initialized||deviceLost||!device||!physicalDevice||!allocator)
	{ reason="Native lens dirt upload device is unavailable.";return {}; }
	try
	{
		auto& resources=ResourceManager::Instance();
		auto source=resources.AcquireTextureSnapshot(assetId);
		if(!source) { reason="Authored lens dirt texture is not loaded.";return {}; }
		// No guessed gamma conversion, decompression, cube layer or premultiply
		// interpretation. The real loader must publish accepted linear RGBA bytes.
		if(source->header.flags!=0||source->descriptor.semantic>static_cast<std::uint8_t>(TextureFileSemantic::Data))
		{ reason="Lens dirt texture has unsupported encoding flags or semantic.";return {}; }
		LookupUpload upload;
		if(!ValidateLookup(*source,std::nullopt,upload,reason)) return {};
		constexpr VkImageUsageFlags usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
		VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(physicalDevice,upload.format,&features);
		PostProcessFormatSupport support;support.optimalFeatures=features.optimalTilingFeatures;
		support.imageQueryResult=vkGetPhysicalDeviceImageFormatProperties(physicalDevice,upload.format,
			VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&support.imageProperties);
		if(!QualifiesPostProcessFormat(support,upload.extent,usage)||upload.size>support.imageProperties.maxResourceSize)
		{ reason="Device cannot upload and linearly sample the complete lens dirt texture.";return {}; }
		auto result=std::make_shared<PostProcessDirtImage>();result->device=device;result->allocator=allocator;result->sourceOwner=source;
		// Reserve failure-retention capacity before any queue submission. A failed
		// completion observation cannot destroy bytes the GPU might still borrow.
		ambiguousUploadBuffers.reserve(ambiguousUploadBuffers.size()+1);
		ambiguousUploadImages.reserve(ambiguousUploadImages.size()+1);
		VulkanImage image{};VulkanBuffer staging{};
		struct UploadOwner
		{
			Impl& context;VulkanImage& image;VulkanBuffer& staging;bool mayBePending=false;
			~UploadOwner() noexcept
			{
				if(mayBePending)
				{
					context.RetainAmbiguousUpload(staging);context.RetainAmbiguousUpload(image);
				}
				else
				{
					context.DestroyBufferImmediate(staging);
					if(image.view)vkDestroyImageView(context.device,image.view,nullptr);
					if(image.image)vmaDestroyImage(context.allocator,image.image,image.allocation);
				}
			}
		} candidate{*this,image,staging};
		image.sourceOwner=source;
		staging=CreateBuffer(upload.size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VMA_MEMORY_USAGE_CPU_ONLY,"Lens Dirt Immutable Staging");
		if(!staging.buffer||!StageLookupBytes(allocator,staging,upload))
		{ reason="Cannot prepare actual lens dirt staging bytes.";return {}; }
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;
		info.extent={upload.extent.width,upload.extent.height,1};info.mipLevels=1;info.arrayLayers=1;info.format=upload.format;
		info.tiling=VK_IMAGE_TILING_OPTIMAL;info.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;info.usage=usage;
		info.samples=VK_SAMPLE_COUNT_1_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
		VmaAllocationCreateInfo allocation{};allocation.usage=VMA_MEMORY_USAGE_GPU_ONLY;
		if(vmaCreateImage(allocator,&info,&allocation,&image.image,&image.allocation,nullptr)!=VK_SUCCESS)
		{ reason="Cannot allocate immutable lens dirt image.";return {}; }
		vmaSetAllocationName(allocator,image.allocation,"Lens Dirt Immutable Texture");
		image.view=CreateImageView(image.image,upload.format,VK_IMAGE_ASPECT_COLOR_BIT,1);
		if(!image.view) { reason="Cannot create immutable lens dirt view.";return {}; }
		const auto command=BeginSingleTimeCommands();
		if(!command) { reason="Cannot begin actual lens dirt upload.";return {}; }
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
		barrier.image=image.image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={upload.extent.width,upload.extent.height,1};
		vkCmdCopyBufferToImage(command,staging.buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
		barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
		vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
		candidate.mayBePending=true;
		if(!EndSingleTimeCommands(command,"lens dirt immutable texture upload"))
		{ reason="Lens dirt upload completion is unproven; native owners remain retained until idle proof.";return {}; }
		candidate.mayBePending=false;
		if(resources.AcquireTextureSnapshot(assetId)!=source)
		{ reason="Lens dirt texture publication changed during upload; retry the candidate.";return {}; }
		auto& output=result->image;output.image=image.image;output.allocation=image.allocation;output.view=image.view;
		output.format=upload.format;output.extent=upload.extent;output.usage=usage;output.layout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		image={};
		LensHdrDirtTexture texture;texture.assetId=assetId;texture.image=output;texture.sourceOwner=std::move(source);texture.imageOwner=std::move(result);
		return texture;
	}
	catch(const std::exception& failure)
	{ reason=std::string("Lens dirt immutable upload failed: ")+failure.what();return {}; }
}
