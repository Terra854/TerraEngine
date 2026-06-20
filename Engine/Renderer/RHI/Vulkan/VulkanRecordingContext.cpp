#include "VulkanRecordingContext.h"

namespace TerraEngine::Renderer::Vulkan {

	ThreadLocalRecordingContext CreateThreadLocalRecordingContext(std::uint32_t workerId, std::uint64_t frameId)
	{
		ThreadLocalRecordingContext context;
		context.workerId = workerId;
		context.frameId = frameId;
		context.commandPoolRevision = frameId;
		context.scratchArenaRevision = frameId;
		context.diagnosticLabel = "vulkan-worker-" + std::to_string(workerId);
		return context;
	}

	bool CanResetThreadLocalRecordingContext(const ThreadLocalRecordingContext& context, std::uint64_t completedFrameId)
	{
		return !context.inUse && context.frameId <= completedFrameId && context.safeToReset;
	}

	void MarkThreadLocalRecordingContextSubmitted(ThreadLocalRecordingContext& context)
	{
		context.inUse = false;
		context.safeToReset = false;
	}

	void ResetThreadLocalRecordingContext(ThreadLocalRecordingContext& context, std::uint64_t nextFrameId)
	{
		context.frameId = nextFrameId;
		++context.commandPoolRevision;
		++context.scratchArenaRevision;
		context.inUse = false;
		context.safeToReset = true;
	}

}
