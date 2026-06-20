#include "RenderWorkerTaskGroup.h"

#include <algorithm>

namespace TerraEngine::Renderer {

	RenderWorkerTaskGroup BuildRenderWorkerTaskGroup(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities,
		std::string label,
		std::size_t requestedWorkItems)
	{
		RenderWorkerTaskGroup group;
		group.groupId = "render-frame-" + std::to_string(snapshot.frameId) + "." + label;
		group.label = std::move(label);
		group.requestedWorkItems = requestedWorkItems;

		const bool canUseWorkers = capabilities.supportsParallelRecording
			&& capabilities.maxRecordingWorkers > 1u
			&& requestedWorkItems >= capabilities.workerSchedulingThreshold;
		if (canUseWorkers)
		{
			group.workerCount = std::min<std::size_t>(capabilities.maxRecordingWorkers, requestedWorkItems);
		}
		else
		{
			group.serialCount = requestedWorkItems > 0u ? 1u : 0u;
		}
		return group;
	}

	void CancelRenderWorkerTaskGroup(RenderWorkerTaskGroup& group, std::string reason)
	{
		group.cancelled = true;
		group.cancellationReason = std::move(reason);
		group.workerCount = 0u;
		group.serialCount = group.requestedWorkItems > 0u ? 1u : 0u;
	}

	bool ShouldRunRenderGroupOnWorkers(const RenderWorkerTaskGroup& group)
	{
		return !group.cancelled && group.workerCount > 0u;
	}

}
