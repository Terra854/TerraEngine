// RenderWorkerTaskGroup.h - scheduler-facing renderer job grouping.
//
// Render jobs share the engine scheduler with scripts and gameplay work. A
// named task group keeps render work diagnosable and cancellable when a frame,
// viewport, or resource revision becomes stale.

#pragma once

#include "RenderFrameSnapshot.h"
#include "Renderer/RHI/RHIDefinitions.h"
#include "Core/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Renderer {

	struct RenderWorkerTaskGroup
	{
		std::string groupId;
		std::string label;
		std::uint32_t priority = 0u;
		std::vector<std::string> dependencies;
		std::size_t requestedWorkItems = 0u;
		std::size_t workerCount = 0u;
		std::size_t serialCount = 0u;
		bool cancelled = false;
		std::string cancellationReason;
	};

	TERRAENGINE_API RenderWorkerTaskGroup BuildRenderWorkerTaskGroup(
		const RenderFrameSnapshot& snapshot,
		const BackendCapabilityProfile& capabilities,
		std::string label,
		std::size_t requestedWorkItems);
	TERRAENGINE_API void CancelRenderWorkerTaskGroup(RenderWorkerTaskGroup& group, std::string reason);
	TERRAENGINE_API bool ShouldRunRenderGroupOnWorkers(const RenderWorkerTaskGroup& group);

}
