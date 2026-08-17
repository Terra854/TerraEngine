// TerrainProceduralOperators.h -- Bounded, immutable terrain height operators.

#pragma once

#include "Terrain/TerrainSelectionQuery.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	// This packet deliberately contains values rather than an asset-store handle.
	// Worker jobs can therefore prepare a complete replacement without touching
	// the live authored root; callers publish the returned areas transactionally.
	struct TerrainProceduralArea
	{
		::TerraEngine::Terrain::TerrainAreaCoordinate coordinate{};
		::TerraEngine::Terrain::Generation observedGeneration = 0u;
		::TerraEngine::Terrain::ContentHash sourceHash{};
		std::vector<std::int32_t> heightMm;
		// An optional row-major mask copied from TerrainSelectionResult::mask.
		// Zero excludes a sample and every other value selects it.
		std::vector<std::uint8_t> selectionMask;
	};

	struct TerrainProceduralBudget
	{
		std::size_t maximumAreas = 64u;
		std::size_t maximumSamples = 64u * ::TerraEngine::Terrain::kTerrainAreaSampleCount;
		std::size_t maximumWorkingBytes = 64u * ::TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t) * 2u;
	};

	struct TerrainProceduralRequest
	{
		std::vector<TerrainProceduralArea> areas;
		TerrainProceduralBudget budget{};
		const std::atomic_bool* cancellation = nullptr;
	};

	struct TerrainProceduralResult
	{
		::TerraEngine::Terrain::TerrainResultCode code = ::TerraEngine::Terrain::TerrainResultCode::Ok;
		std::string message;
		std::vector<TerrainProceduralArea> areas;
		::TerraEngine::Terrain::ContentHash outputHash{};
		[[nodiscard]] bool Succeeded() const noexcept { return code == ::TerraEngine::Terrain::TerrainResultCode::Ok; }
	};

	struct TerrainHeightRemapSettings { std::int32_t multiplyNumerator = 1; std::int32_t multiplyDenominator = 1; std::int32_t addMm = 0; std::int32_t minimumMm = INT32_MIN; std::int32_t maximumMm = INT32_MAX; };
	struct TerrainTerraceSettings { std::int32_t stepMm = 1000; std::uint8_t blendPercent = 0u; };
	struct TerrainNoiseSettings { std::uint64_t seed = 0u; std::int32_t amplitudeMm = 0; std::uint32_t periodSamples = 32u; };
	struct TerrainStampSettings { ::TerraEngine::Terrain::TerrainSampleCoordinate centre{}; std::uint32_t radiusSamples = 1u; std::int32_t heightDeltaMm = 0; std::uint8_t falloffPercent = 100u; };

	TerrainProceduralResult ApplyTerrainHeightRemap(const TerrainProceduralRequest& request, const TerrainHeightRemapSettings& settings);
	TerrainProceduralResult ApplyTerrainTerrace(const TerrainProceduralRequest& request, const TerrainTerraceSettings& settings);
	TerrainProceduralResult ApplyTerrainSeededNoise(const TerrainProceduralRequest& request, const TerrainNoiseSettings& settings);
	TerrainProceduralResult ApplyTerrainStamp(const TerrainProceduralRequest& request, const TerrainStampSettings& settings);
}
