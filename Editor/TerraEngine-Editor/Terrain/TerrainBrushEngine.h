// TerrainBrushEngine.h -- Deterministic CPU kernels used by detached previews.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	enum class TerrainBrushMode : std::uint8_t { Raise, Lower, Smooth, FlattenValue, FlattenPicked, Terrace, Ramp, Stamp, Noise };
	enum class TerrainBrushFalloff : std::uint8_t { Constant, Linear, SmoothStep, Gaussian };

	struct TerrainBrushSettings
	{
		TerrainBrushMode mode = TerrainBrushMode::Raise;
		TerrainBrushFalloff falloff = TerrainBrushFalloff::SmoothStep;
		float radiusSamples = 8.0F;
		float strengthMm = 100.0F;
		float spacingSamples = 1.0F;
		float rotationRadians = 0.0F;
		float pressure = 1.0F;
		std::int32_t flattenHeightMm = 0;
		std::int32_t terraceStepMm = 250;
		std::uint32_t randomSeed = 0U;
	};

	struct TerrainBrushStamp
	{
		float centerX = 0.0F;
		float centerZ = 0.0F;
	};

	class TerrainBrushEngine
	{
	public:
		[[nodiscard]] static bool IsStrokeSampleDue(const TerrainBrushStamp& previous, const TerrainBrushStamp& candidate, float spacingSamples) noexcept;
		[[nodiscard]] static float EvaluateFalloff(TerrainBrushFalloff falloff, float normalizedDistance) noexcept;
		// Applies a single stamp to a local heightfield. The optional mask uses
		// 0..255 coverage and never permits the kernel outside its array bounds.
		static void ApplyStamp(
			std::span<const std::int32_t> sourceHeightMm,
			std::span<std::int32_t> outHeightMm,
			std::uint32_t width,
			std::uint32_t height,
			const TerrainBrushSettings& settings,
			const TerrainBrushStamp& stamp,
			std::span<const std::uint8_t> mask = {});
	};
}
