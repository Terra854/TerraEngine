// TerrainThermalErosion.h -- Deterministic talus relaxation for terrain previews.

#pragma once

#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralOperators.h"

namespace TerraEngine::Editor::Terrain
{
	struct TerrainThermalErosionSettings
	{
		std::uint32_t iterations = 1u;
		std::uint32_t maximumIterations = 64u;
		std::size_t maximumWorkingBytes = 64u * ::TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u;
		std::int32_t talusMm = 100;
		std::int32_t transferPerIterationMm = 1;
	};

	TerrainProceduralResult ApplyTerrainThermalErosion(const TerrainProceduralRequest& request, const TerrainThermalErosionSettings& settings);
}
