#pragma once

#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/Graph/RenderFramePublication.h"

namespace TerraEngine::Tests {

	// Renderer workers are allowed to produce local matrices only for an explicit
	// per-view reference frame. Test fixtures use this small canonical frame
	// factory instead of relying on the former implicit global-float origin.
	inline TerraEngine::Renderer::RenderReferenceFrame MakeRendererTestReferenceFrame(
		std::uint64_t frameId,
		std::uint64_t viewId,
		std::uint32_t outputId)
	{
		TerraEngine::Renderer::RenderReferenceFrame frame{};
		frame.frameId = frameId;
		frame.viewId = viewId;
		frame.outputId = outputId;
		frame.originGeneration = 1u;
		frame.spatialGeneration = 1u;
		frame.residencyGeneration = 1u;
		return frame;
	}

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
