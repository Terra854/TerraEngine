#pragma once
#include "Engine/Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
namespace TerraEngine::Editor
{
	// Builds one typed foliage-card or curve candidate from a stable source id.
	// The immutable result shares storage across repeated instances and is ready
	// for the protected generation publication boundary.
	[[nodiscard]] TerraEngine::Renderer::VirtualGeometryAggregateRepresentation BuildVirtualGeometryFoliageCurveCandidate(std::uint64_t id, bool curve);
} // namespace TerraEngine::Editor
