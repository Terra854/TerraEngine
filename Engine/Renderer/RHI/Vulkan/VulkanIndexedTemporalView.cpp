#include "VulkanContextImpl.h"
#include "VulkanPostProcessPasses.h"
#include "VulkanPostProcessTemporalView.h"

bool VulkanContext::Impl::PrepareIndexedTemporalVariants(
	const TerraEngine::Renderer::RenderFramePublication& publication, std::uint32_t viewIndex,
	VkRenderPass rasterPass, const VulkanRasterPipelineConfiguration& indexed,
	TerraEngine::Renderer::Vulkan::PostProcessPassResources& passes, std::string& reason)
{
	using namespace TerraEngine::Renderer;
	using namespace TerraEngine::Renderer::Vulkan;
	if(!passes.temporalView || indexed.samples != VK_SAMPLE_COUNT_1_BIT)
	{
		reason = "Temporal motion requires the current indexed single-sample view owner.";
		return false;
	}
	auto& resources = ResourceManager::Instance();
	const auto defaultId = resources.GetDefaultShaderCollectionID();
	const auto variants = pipelineCache.find(rasterPass);
	const auto resolver = MakeRenderViewCommandResolver(publication, viewIndex);
	for(std::uint32_t column = 0u; column < resolver.Count(); ++column)
	{
		RenderItemRecord item;
		if(!resolver.Resolve(column, item)) { reason = "The temporal geometry publication changed."; return false; }
		const auto material = resources.AcquireMaterialSnapshot(item.materialId);
		// The depth-equal auxiliary shader does not run alpha tests. Selecting
		// it for opaque Default surfaces makes the coverage contract exact.
		if(!material || material->header.alphaMode != static_cast<std::uint32_t>(AlphaMode::Opaque))
		{
			reason = "Initial indexed temporal motion supports opaque Default surfaces.";
			return false;
		}
		const auto key = BuildPipelineKey(defaultId, material->header.flags, true, indexed);
		if(!key || variants == pipelineCache.end()) { reason = "The indexed surface pipeline is absent."; return false; }
		const auto found = variants->second.find(*key);
		if(found == variants->second.end() || found->second.setLayouts.size() != 2u)
		{
			reason = "The indexed surface does not expose its exact global and geometry layouts.";
			return false;
		}
		const auto& surface = found->second;
		if(!PrepareTemporalMotionVariant(*passes.temporalView, {surface.setLayouts[0], surface.setLayouts[1]},
			surface.rasterCullMode, surface.rasterFrontFace, VK_NULL_HANDLE, reason)) return false;
	}
	return true;
}
