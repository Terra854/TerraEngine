// One frame's exclusive scratch lease and its borrowing component collections.
#pragma once
#include "Renderer/Graph/RenderFrameScratch.h"
#include <memory_resource>
#include <vector>

namespace TerraEngine::Renderer::Frame {
	// The owner must outlive its lease, and both must outlive the PMR vectors.
	// The concrete entry types remain private to RenderSystem. This small holder
	// does not own resources, routes, views or backend state from other domains.
	template<typename CameraEntry, typename LightEntry>
	struct RenderScratchEpisode final {
		std::shared_ptr<RenderFrameScratch> owner;
		RenderFrameScratch::Lease lease;
		std::pmr::vector<CameraEntry> cameras;
		std::pmr::vector<LightEntry> lights;

		RenderScratchEpisode(std::shared_ptr<RenderFrameScratch> scratch, std::uint64_t frameId)
			: owner(std::move(scratch)), lease(owner->BeginEpisode(frameId)),
			cameras(lease.Resource()), lights(lease.Resource()) {}
		RenderScratchEpisode(const RenderScratchEpisode&) = delete;
		RenderScratchEpisode& operator=(const RenderScratchEpisode&) = delete;
	};
}
