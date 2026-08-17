// TerrainHydraulicErosion.h -- Deterministic, bounded hydraulic-style erosion.

#pragma once

#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"

namespace TerraEngine::Editor::Terrain
{
	struct TerrainHydraulicErosionSettings
	{
		std::uint32_t iterations = 1u;
		std::uint32_t maximumIterations = 64u;
		std::size_t maximumWorkingBytes = 64u * ::TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u;
		std::int32_t erosionPerIterationMm = 1;
	};

	TerrainProceduralResult ApplyTerrainHydraulicErosion(const TerrainProceduralRequest& request, const TerrainHydraulicErosionSettings& settings);
}
