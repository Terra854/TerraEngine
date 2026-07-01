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

	VirtualGeometryPageUploadPublication PlanVirtualGeometryPageUploadPublication(const GeometryPageRuntime& page)
	{
		VirtualGeometryPageUploadPublication publication;
		publication.pageId = page.pageId;
		if (page.state == GeometryPageState::Resident || page.state == GeometryPageState::Pinned)
		{
			publication.accepted = true;
			publication.publishedState = page.state;
			publication.reason = "virtual geometry page is already published";
			return publication;
		}
		if (page.state != GeometryPageState::PendingUpload)
		{
			publication.reason = "virtual geometry page is not waiting for protected GPU upload";
			return publication;
		}
		if (page.byteSize == 0u)
		{
			publication.reason = "virtual geometry page upload has no byte payload";
			return publication;
		}

		// The actual Vulkan buffer creation and descriptor publication stay in
		// the coordinator-owned backend path. This pure hook lets workers and
		// tests agree on the state that may be published once upload succeeds.
		publication.accepted = true;
		publication.publishedState = (page.fallbackPage || page.pinnedFallback)
			? GeometryPageState::Pinned
			: GeometryPageState::Resident;
		publication.reason = publication.publishedState == GeometryPageState::Pinned
			? "virtual geometry fallback page uploaded and pinned"
			: "virtual geometry page uploaded";
		return publication;
	}

	VirtualGeometryPageRetirementPlan PlanVirtualGeometryPageRetirement(const GeometryPageRuntime& page, std::uint64_t frameId, std::uint64_t safeFrameLag)
	{
		VirtualGeometryPageRetirementPlan plan;
		plan.pageId = page.pageId;
		if (page.fallbackPage || page.pinnedFallback || page.state == GeometryPageState::Pinned)
		{
			plan.reason = "virtual geometry fallback page is protected from retirement";
			return plan;
		}
		if (page.state != GeometryPageState::Evicting)
		{
			plan.reason = "virtual geometry page is not marked evicting";
			return plan;
		}

		// Page retirement waits for completed frames so in-flight command
		// buffers never read buffers after the backend releases them.
		plan.canRetire = true;
		plan.retireAfterFrameId = frameId + safeFrameLag;
		plan.reason = "virtual geometry page queued for deferred retirement";
		return plan;
	}

}
