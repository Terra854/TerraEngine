#include "VulkanUploadPlanning.h"

namespace TerraEngine::Renderer::Vulkan {

	UploadPreparationPlan BuildUploadPreparationPlan(const RenderFrameSnapshot& snapshot, std::size_t payloadBudget)
	{
		UploadPreparationPlan plan;
		plan.frameId = snapshot.frameId;
		plan.resourceRevision = snapshot.resourceRevision;
		plan.cancellationToken = { snapshot.frameId, snapshot.resourceRevision, snapshot.frameId, "upload-plan" };
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
		plan.budget = { RenderWorkBudgetCategory::Upload, payloadBudget, plan.payloads.size(), RenderBudgetOverflowPolicy::Delay, "upload-payloads" };
		plan.overBudget = IsRenderWorkOverBudget(plan.budget);
		while (payloadBudget > 0u && plan.payloads.size() > payloadBudget)
		{
			plan.delayedPayloads.push_back(plan.payloads.back());
			plan.payloads.pop_back();
		}
		return plan;
	}

}
