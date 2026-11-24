#pragma once

#include "Renderer/Graph/RenderFramePublication.h"
#include <memory>
#include <vector>

namespace TerraEngine::TestSupport
{
	// These are real ordinary ResourceManager publications, not substitute GPU
	// attachments. The fixture retains them until every native readback settles.
	struct ScreenLightingGeometry
	{
		struct Surface
		{
			UUID objectId{}, meshId{}, materialId{};
			std::uint64_t meshGeneration = 0, materialGeneration = 0;
		};
		std::vector<Surface> surfaces;
	};
	ScreenLightingGeometry CreateScreenLightingGeometry();
	// Makes an isolated immutable publication using the same defensive seal as
	// production. It never writes scene storage or invents a recorded draw receipt.
	std::shared_ptr<const Renderer::RenderFramePublication> PublishScreenLightingGeometry(
		const ScreenLightingGeometry& geometry, Renderer::RenderViewPresentationInput input,
		std::uint64_t frame, std::uint64_t session, std::uint64_t backendGeneration);
}
