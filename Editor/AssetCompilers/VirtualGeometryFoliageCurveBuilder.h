#pragma once
#include "Engine/Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
namespace TerraEngine::Editor
{
	// Builds one typed foliage-card or curve prototype from a stable source id.
	// The caller later validates coverage and transition error before publication.
	[[nodiscard]] TerraEngine::Renderer::VirtualGeometryAggregateRepresentation BuildVirtualGeometryFoliageCurvePrototype(std::uint64_t id, bool curve);
} // namespace TerraEngine::Editor
