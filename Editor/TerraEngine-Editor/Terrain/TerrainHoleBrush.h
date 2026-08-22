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
		static constexpr float kMinimumRadiusCells = 1.0F;
		// Every entry point uses this value so rendering, automation, and the edit
		// kernel cannot disagree about the supported minimum brush footprint.
		[[nodiscard]] static constexpr float EffectiveRadiusCells(float requestedRadius) noexcept
		{
			return requestedRadius >= kMinimumRadiusCells ? requestedRadius : kMinimumRadiusCells;
		}
		// Picking identifies the containing integer cell, while the circular kernel
		// measures distance from cell centres. This conversion guarantees that even
		// a sub-cell brush radius still covers the cell directly under the pointer.
		[[nodiscard]] static constexpr TerrainHoleBrushStamp CenteredOnCell(
			std::int64_t cellX, std::int64_t cellZ) noexcept
		{
			return {
				static_cast<float>(cellX) + 0.5F,
				static_cast<float>(cellZ) + 0.5F};
		}
		[[nodiscard]] static bool IsHole(std::span<const std::uint8_t> canonicalMask,
			std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t z) noexcept;
		// Applies a circular brush stamp to a copied bit mask. Invalid spans leave
		// output unchanged, making malformed worker input safe to reject upstream.
		[[nodiscard]] static bool ApplyStamp(std::span<const std::uint8_t> sourceMask,
			std::span<std::uint8_t> outMask, std::uint32_t width, std::uint32_t height,
			const TerrainHoleBrushSettings& settings, const TerrainHoleBrushStamp& stamp) noexcept;
	};
}
