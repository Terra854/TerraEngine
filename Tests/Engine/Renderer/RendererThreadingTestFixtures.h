#pragma once

#include "Renderer/Graph/RenderGraph.h"

namespace TerraEngine::Tests {

	TerraEngine::Renderer::RenderFrameSnapshot MakeRendererThreadingSnapshot(
		std::uint64_t frameId,
		std::size_t renderableCount,
		std::size_t lightCount = 0u);
	TerraEngine::Renderer::BackendCapabilityProfile MakeThreadedRendererProfile();
	TerraEngine::Renderer::RenderPassNode MakeRendererPass(
		std::string passId,
		std::uint64_t resourceId,
		TerraEngine::Renderer::RenderAccessMode outputAccess);
	void RandomizePacketCompletionOrder(std::vector<TerraEngine::Renderer::RenderWorkPacket>& packets);
	void InjectResourceDelay(TerraEngine::Renderer::RenderFrameSnapshot& snapshot, const char* reason);

}
