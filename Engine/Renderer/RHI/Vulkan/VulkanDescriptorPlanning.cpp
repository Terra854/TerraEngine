#include "VulkanDescriptorPlanning.h"

namespace TerraEngine::Renderer::Vulkan {

	DescriptorBindingPlan BuildDescriptorBindingPlan(const RenderFrameSnapshot& snapshot, std::uint32_t viewId)
	{
		DescriptorBindingPlan plan;
		plan.frameId = snapshot.frameId;
		plan.viewId = viewId;
		plan.records.reserve(snapshot.materials.size() + snapshot.textures.size());
		for (const RenderFrameResourceRecord& material : snapshot.materials)
		{
			plan.records.push_back("material:" + TerraEngine::ToHexString(material.resourceId));
		}
		for (const RenderFrameResourceRecord& texture : snapshot.textures)
		{
			plan.records.push_back("texture:" + TerraEngine::ToHexString(texture.resourceId));
		}
		plan.fallbackReason = plan.coordinatorOwnedAllocator ? "Descriptor allocator mutation remains coordinator-owned." : "";
		return plan;
	}

}
