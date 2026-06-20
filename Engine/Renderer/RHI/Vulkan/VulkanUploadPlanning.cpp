#include "VulkanUploadPlanning.h"

namespace TerraEngine::Renderer::Vulkan {

	UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot)
	{
		UploadPreparationPlan plan;
		plan.frameId = snapshot.frameId;
		plan.resourceRevision = snapshot.resourceRevision;
		for (const RenderFrameResourceRecord& mesh : snapshot.meshes)
		{
			if (!mesh.resident)
			{
				plan.payloads.push_back("mesh:" + TerraEngine::ToHexString(mesh.resourceId));
			}
		}
		for (const RenderFrameResourceRecord& texture : snapshot.textures)
		{
			if (!texture.resident)
			{
				plan.payloads.push_back("texture:" + TerraEngine::ToHexString(texture.resourceId));
			}
		}
		return plan;
	}

}
