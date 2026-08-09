#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace TerraEngine::Tests {

	TerraEngine::Renderer::RenderFrameSnapshot MakeRendererThreadingSnapshot(
		std::uint64_t frameId,
		std::size_t renderableCount,
		std::size_t lightCount = 0u);
	// Tests may author a convenient legacy snapshot, then explicitly convert it
	// into the same compact immutable publication consumed by Vulkan production
	// code.  This keeps test inputs concise without preserving a snapshot API at
	// the backend boundary.
	TerraEngine::Renderer::RenderFramePublication BuildRendererThreadingPublication(
		const TerraEngine::Renderer::RenderFrameSnapshot& snapshot);
	TerraEngine::Renderer::BackendCapabilityProfile MakeThreadedRendererProfile();
	TerraEngine::Renderer::RenderPassNode MakeRendererPass(
		std::string passId,
		std::uint64_t resourceId,
		TerraEngine::Renderer::RenderAccessMode outputAccess);
	void RandomizePacketCompletionOrder(std::vector<TerraEngine::Renderer::RenderWorkPacket>& packets);
	void InjectResourceDelay(TerraEngine::Renderer::RenderFrameSnapshot& snapshot, const char* reason);

}
