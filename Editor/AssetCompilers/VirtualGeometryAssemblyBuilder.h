#pragma once
#include "Engine/Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
#include <vector>
namespace TerraEngine::Editor
{
	// Converts stable source-piece identities into deterministic disconnected
	// assembly prototypes. The returned records contain no runtime handles.
	[[nodiscard]] std::vector<TerraEngine::Renderer::VirtualGeometryAggregateRepresentation> BuildVirtualGeometryAssemblyPrototypes(
		std::vector<std::uint64_t> ids);
} // namespace TerraEngine::Editor
