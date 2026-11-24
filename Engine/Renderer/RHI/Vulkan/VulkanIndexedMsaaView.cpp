#include "VulkanContextImpl.h"
#include "VulkanPostProcessMsaa.h"
#include "VulkanPostProcessPasses.h"

#include <cstring>

namespace
{
	using namespace TerraEngine::Renderer::Vulkan;

	std::vector<std::uint32_t> ReadCoverageShader(const char* name)
	{
		auto& resources = ResourceManager::Instance();
		const auto bytes = resources.ReadFileBytes(resources.ResolveEngineAssetPath(name));
		if(bytes.size() < sizeof(std::uint32_t) || bytes.size() % sizeof(std::uint32_t) != 0u)
			return {};
		// Package reads can return byte alignment. Hand Vulkan an aligned word
		// span and check the SPIR-V signature before pipeline preparation.
		std::vector<std::uint32_t> words(bytes.size() / sizeof(std::uint32_t));
		std::memcpy(words.data(), bytes.data(), bytes.size());
		if(words.front() != 0x07230203u) return {};
		return words;
	}
}

bool VulkanContext::Impl::SupportsIndexedMsaa4(VkFormat colorFormat)
{
	using namespace TerraEngine::Renderer::Vulkan;
	if(deviceLost || UsesVrsAttachment()) return false;
	if(indexedMsaaShaderGeneration != backendResourceGeneration)
	{
		indexedMsaaShaderReady = false;
		try
		{
			indexedMsaaShaderReady = !ReadCoverageShader("Shaders/PostProcess/MsaaIdentityResolve.comp.spv").empty()
				&& !ReadCoverageShader("Shaders/PostProcess/HiZMsaa4Seed.comp.spv").empty();
		}
		catch(const std::exception&) {}
		indexedMsaaShaderGeneration = backendResourceGeneration;
	}
	if(!indexedMsaaShaderReady) return false;
	const auto shape = DescribeIndexedMsaa4Attachments(colorFormat, FindDepthFormat());
	VkImageFormatProperties destination{};
	constexpr auto identityUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	return SupportsIndexedMsaa4Attachments(physicalDevice, {1u, 1u}, shape)
		&& vkGetPhysicalDeviceImageFormatProperties(physicalDevice, VK_FORMAT_R32G32B32A32_UINT, VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL, identityUsage, 0, &destination) == VK_SUCCESS
		&& (destination.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0u;
}

VkRenderPass VulkanContext::Impl::EnsureIndexedMsaaRenderPass(std::string& reason)
{
	using namespace TerraEngine::Renderer::Vulkan;
	if(indexedMsaaRenderPass) return indexedMsaaRenderPass;
	const auto nativeDepthFormat = FindDepthFormat();
	const auto shape = DescribeIndexedMsaa4Attachments(offscreenColorFormat, nativeDepthFormat);
	if(UsesVrsAttachment() || !SupportsIndexedMsaa4Attachments(physicalDevice, {1u, 1u}, shape))
	{
		reason = "The active Vulkan device does not qualify indexed coverage attachments.";
		return VK_NULL_HANDLE;
	}
	VkRenderPass pass = VK_NULL_HANDLE;
	if(CreateIndexedMsaa4RenderPass(device, shape, pass) != VK_SUCCESS)
	{
		reason = "Could not prepare the native four-sample render pass.";
		return VK_NULL_HANDLE;
	}
	try
	{
		indexedRasterPasses.emplace(pass, IndexedRasterPassCompatibility{
			VK_SAMPLE_COUNT_4_BIT, offscreenColorFormat, VK_FORMAT_R32G32B32A32_UINT, nativeDepthFormat, false});
	}
	catch(const std::bad_alloc&)
	{
		vkDestroyRenderPass(device, pass, nullptr);
		reason = "Could not retain the native four-sample pass compatibility.";
		return VK_NULL_HANDLE;
	}
	indexedMsaaRenderPass = pass;
	return pass;
}

bool VulkanContext::Impl::PrepareIndexedMsaaTargets(const OffscreenResources& source,
	TerraEngine::Renderer::Vulkan::PostProcessPassResources& passes, std::string& reason)
{
	using namespace TerraEngine::Renderer::Vulkan;
	try
	{
		const auto nativeDepthFormat = FindDepthFormat();
		const auto pass = EnsureIndexedMsaaRenderPass(reason);
		if(!pass) return false;
		const auto identityShader = ReadCoverageShader("Shaders/PostProcess/MsaaIdentityResolve.comp.spv");
		const auto seedShader = ReadCoverageShader("Shaders/PostProcess/HiZMsaa4Seed.comp.spv");
		IndexedMsaa4CreateInfo request;
		request.physicalDevice = physicalDevice;
		request.device = device;
		request.allocator = allocator;
		request.extent = source.extent;
		request.resolvedColor = {source.image, source.view, offscreenColorFormat,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_SAMPLE_COUNT_1_BIT, source.extent};
		request.resolvedDepth = {source.depthImage, source.depthView, nativeDepthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			VK_SAMPLE_COUNT_1_BIT, source.extent};
		request.destinationIdentity = {source.visibilityIdentity.image, source.visibilityIdentity.view,
			VK_FORMAT_R32G32B32A32_UINT, source.visibilityIdentity.usage, VK_SAMPLE_COUNT_1_BIT, source.extent};
		request.identityFinalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		request.compatibleRenderPass = pass;
		request.identityShader = identityShader;
		request.hiZSeedShader = seedShader;
		auto prepared = CreateIndexedMsaa4Resources(request, reason);
		if(!prepared) return false;
		passes.coverage = std::move(prepared);
		return true;
	}
	catch(const std::exception&)
	{
		reason = "Failed to prepare the installed native coverage resolve shaders or target owners.";
		return false;
	}
}
