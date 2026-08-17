// TerrainSelectionQuery.h -- Exact bounded masks for procedural terrain work.

#pragma once

#include "Terrain/TerrainAreaCodec.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	// The storage rectangle may include a one-sample halo around evaluationBounds.
	// Supplying that halo lets slope selection agree across authored area edges.
	struct TerrainSelectionGrid
	{
		TerrainSampleCoordinate storageOrigin{};
		std::uint32_t storageWidth = 0u;
		std::uint32_t storageHeight = 0u;
		TerrainSampleBounds evaluationBounds{};
		std::span<const std::int32_t> heightMm;
		std::span<const std::uint8_t> paintedMask;
		std::span<const TerrainWeightSample> weights;
	};

	struct TerrainSelectionCriteria
	{
		std::optional<TerrainSampleBounds> worldBounds;
		std::optional<std::int32_t> minimumHeightMm;
		std::optional<std::int32_t> maximumHeightMm;
		// Slope is compared as exact integer rise/run in per-mille units. For
		// example, 1000 means a one-metre rise over one metre of sample spacing.
		std::optional<std::uint32_t> minimumSlopePermille;
		std::optional<std::uint32_t> maximumSlopePermille;
		std::uint32_t sampleSpacingMm = 1000u;
		std::optional<std::uint8_t> minimumPaintedMask;
		std::optional<std::uint16_t> layerStableId;
		std::uint8_t minimumLayerWeight = 1u;
		std::size_t maximumEvaluatedSamples = kTerrainAreaSampleCount;
	};

	struct TerrainSelectionResult
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
		// The result is row-major over evaluationBounds, never the optional halo.
		std::vector<std::uint8_t> mask;
		std::size_t selectedSampleCount = 0u;
		std::optional<TerrainSampleBounds> selectedBounds;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok; }
	};

	// Every configured criterion is combined with logical AND. The function is
	// read-only and allocation is rejected before work if the declared budget is
	// too small, making it safe to call from a worker job.
	TERRAENGINE_API TerrainSelectionResult EvaluateTerrainSelection(
		const TerrainSelectionGrid& grid,
		const TerrainSelectionCriteria& criteria);
}
