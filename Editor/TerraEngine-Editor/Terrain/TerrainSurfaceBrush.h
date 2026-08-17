// TerrainSurfaceBrush.h -- Deterministic detached paint kernels for terrain weights.

#pragma once

#include "Terrain/TerrainAreaCodec.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	// Surface painting always addresses stable layer IDs. Presentation order and
	// temporary material failures therefore cannot rewrite already-authored texels.
	enum class TerrainSurfaceBrushMode : std::uint8_t { Paint, Erase, Fill, Replace, Normalize, LayerMask };

	struct TerrainSurfaceBrushSettings
	{
		TerrainSurfaceBrushMode mode = TerrainSurfaceBrushMode::Paint;
		std::uint16_t layerId = 0u;
		std::uint16_t replacementLayerId = 0u;
		std::uint8_t strength = 32u;
		bool useLayerMask = false;
		std::uint32_t displacementSeed = 0u;
	};

	struct TerrainSurfaceBrushResult
	{
		bool changed = false;
		// A non-zero id means an incoming fifth contributor displaced it. The UI
		// surfaces this rather than silently hiding a painted material.
		std::uint16_t displacedLayerId = 0u;
		std::int32_t deterministicDisplacementMm = 0;
	};

	class TerrainSurfaceBrush
	{
	public:
		// Applies one bounded sample edit to detached data. layerMask is optional
		// and uses 0..255 coverage, matching the height-brush mask convention.
		[[nodiscard]] static TerrainSurfaceBrushResult ApplySample(
			TerraEngine::Terrain::TerrainWeightSample& sample,
			const TerrainSurfaceBrushSettings& settings,
			std::uint8_t layerMask = 255u) noexcept;
		// Fill/replace/normalize operate in index order so previews remain bitwise
		// reproducible regardless of worker scheduling.
		[[nodiscard]] static TerrainSurfaceBrushResult ApplyArea(
			std::span<TerraEngine::Terrain::TerrainWeightSample> samples,
			const TerrainSurfaceBrushSettings& settings,
			std::span<const std::uint8_t> layerMask = {}) noexcept;
		[[nodiscard]] static std::int32_t PreviewDisplacementMm(
			std::uint32_t seed, std::int64_t sampleX, std::int64_t sampleZ,
			std::int32_t amplitudeMm) noexcept;
	};
}
