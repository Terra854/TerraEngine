#pragma once
#include "Engine/Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"
namespace TerraEngine::Editor
{
	// Builds one voxel or impostor transition candidate. This offline helper does
	// not mutate an asset; publication occurs only after the candidate is checked.
	[[nodiscard]] TerraEngine::Renderer::VirtualGeometryAggregateRepresentation BuildVirtualGeometryVoxelPrototype(std::uint64_t id, bool impostor);
} // namespace TerraEngine::Editor
