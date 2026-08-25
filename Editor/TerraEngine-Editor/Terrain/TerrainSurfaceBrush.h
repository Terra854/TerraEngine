// TerrainSurfaceBrush.h -- Deterministic detached paint kernels for terrain weights.

#pragma once

#include "Terrain/TerrainAreaCodec.h"
#include "Terrain/TerrainLayerStack.h"

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
		// Scene View strokes use the same sample-space convention as height tools.
		// Keeping radius and spacing in samples makes coverage independent of zoom.
		float radiusSamples = 8.0F;
		float spacingSamples = 1.0F;
		bool useLayerMask = false;
		std::uint32_t displacementSeed = 0u;
	};

	struct TerrainSurfaceBrushStamp
	{
		float centerX = 0.0F;
		float centerZ = 0.0F;
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
		// Returns the requested layer only while it remains enabled. Layer-list
		// edits use the first enabled layer as a deterministic fallback so a paint
		// stroke never silently targets the invalid stable ID zero.
		[[nodiscard]] static std::uint16_t ResolveEnabledLayerId(
			const TerraEngine::Terrain::TerrainLayerStack& stack,
			std::uint16_t preferredLayerId) noexcept;
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
		// Applies one circular, smooth-falloff stamp to detached weights. Source and
		// output may alias only through separate buffers, matching the height brush.
		[[nodiscard]] static TerrainSurfaceBrushResult ApplyStamp(
			std::span<const TerraEngine::Terrain::TerrainWeightSample> source,
			std::span<TerraEngine::Terrain::TerrainWeightSample> out,
			std::uint32_t width,
			std::uint32_t height,
			const TerrainSurfaceBrushSettings& settings,
			const TerrainSurfaceBrushStamp& stamp,
			std::span<const std::uint8_t> layerMask = {}) noexcept;
		[[nodiscard]] static bool IsStrokeSampleDue(
			const TerrainSurfaceBrushStamp& previous,
			const TerrainSurfaceBrushStamp& candidate,
			float spacingSamples) noexcept;
		[[nodiscard]] static std::int32_t PreviewDisplacementMm(
			std::uint32_t seed, std::int64_t sampleX, std::int64_t sampleZ,
			std::int32_t amplitudeMm) noexcept;
	};
}
