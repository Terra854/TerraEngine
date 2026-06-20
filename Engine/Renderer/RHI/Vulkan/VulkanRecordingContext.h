// VulkanRecordingContext.h - thread-local recording ownership model.
//
// The public contract stores only stable frame and worker metadata. Actual
// Vulkan command pools and command buffers remain owned inside VulkanContext.

#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <string>

namespace TerraEngine::Renderer::Vulkan {

	struct ThreadLocalRecordingContext
	{
		std::uint32_t workerId = 0u;
		std::uint64_t frameId = 0u;
		std::uint64_t commandPoolRevision = 0u;
		std::uint64_t scratchArenaRevision = 0u;
		bool inUse = false;
		bool safeToReset = true;
		std::string diagnosticLabel;
	};

	TERRAENGINE_API ThreadLocalRecordingContext CreateThreadLocalRecordingContext(std::uint32_t workerId, std::uint64_t frameId);
	TERRAENGINE_API bool CanResetThreadLocalRecordingContext(const ThreadLocalRecordingContext& context, std::uint64_t completedFrameId);
	TERRAENGINE_API void MarkThreadLocalRecordingContextSubmitted(ThreadLocalRecordingContext& context);
	TERRAENGINE_API void ResetThreadLocalRecordingContext(ThreadLocalRecordingContext& context, std::uint64_t nextFrameId);

}
