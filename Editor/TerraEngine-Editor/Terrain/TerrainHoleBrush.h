// TerrainHoleBrush.h -- Deterministic detached terrain-hole cell editing.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	// Hole bits describe canonical terrain cells, never vertices. A set bit means
	// every consumer must treat that cell as absent from geometry and collision.
	enum class TerrainHoleBrushMode : std::uint8_t { PaintHole, EraseHole };

	struct TerrainHoleBrushSettings
	{
		TerrainHoleBrushMode mode = TerrainHoleBrushMode::PaintHole;
		float radiusCells = 2.0F;
	};

	struct TerrainHoleBrushStamp
	{
		float centerCellX = 0.0F;
		float centerCellZ = 0.0F;
	};

	class TerrainHoleBrush
	{
	public:
		[[nodiscard]] static bool IsHole(std::span<const std::uint8_t> canonicalMask,
			std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t z) noexcept;
		// Applies a circular brush stamp to a copied bit mask. Invalid spans leave
		// output unchanged, making malformed worker input safe to reject upstream.
		[[nodiscard]] static bool ApplyStamp(std::span<const std::uint8_t> sourceMask,
			std::span<std::uint8_t> outMask, std::uint32_t width, std::uint32_t height,
			const TerrainHoleBrushSettings& settings, const TerrainHoleBrushStamp& stamp) noexcept;
	};
}
