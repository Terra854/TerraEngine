// TerrainHoleBrush.cpp -- Bounds-safe canonical hole-mask brush implementation.

#include "TerrainHoleBrush.h"

#include <algorithm>
#include <cmath>

namespace TerraEngine::Editor::Terrain
{
namespace
{
	std::size_t RequiredBytes(std::uint32_t width, std::uint32_t height) noexcept
	{
		return (static_cast<std::size_t>(width) * height + 7u) / 8u;
	}

	bool ReadBit(std::span<const std::uint8_t> bytes, std::size_t index) noexcept
	{
		return (bytes[index / 8u] & static_cast<std::uint8_t>(1u << (index % 8u))) != 0u;
	}

	void WriteBit(std::span<std::uint8_t> bytes, std::size_t index, bool value) noexcept
	{
		const std::uint8_t mask = static_cast<std::uint8_t>(1u << (index % 8u));
		if(value) bytes[index / 8u] |= mask;
		else bytes[index / 8u] &= static_cast<std::uint8_t>(~mask);
	}
}

	bool TerrainHoleBrush::IsHole(std::span<const std::uint8_t> canonicalMask,
		std::uint32_t width, std::uint32_t height, std::uint32_t x, std::uint32_t z) noexcept
	{
		if(width == 0u || height == 0u || x >= width || z >= height || canonicalMask.size() < RequiredBytes(width, height)) return false;
		return ReadBit(canonicalMask, static_cast<std::size_t>(z) * width + x);
	}

	bool TerrainHoleBrush::ApplyStamp(std::span<const std::uint8_t> sourceMask,
		std::span<std::uint8_t> outMask, std::uint32_t width, std::uint32_t height,
		const TerrainHoleBrushSettings& settings, const TerrainHoleBrushStamp& stamp) noexcept
	{
		const std::size_t required = RequiredBytes(width, height);
		if(width == 0u || height == 0u || sourceMask.size() < required || outMask.size() < required) return false;
		std::copy_n(sourceMask.begin(), required, outMask.begin());
		const float radius = EffectiveRadiusCells(settings.radiusCells);
		bool changed = false;
		for(std::uint32_t z = 0u; z < height; ++z) for(std::uint32_t x = 0u; x < width; ++x)
		{
			const float dx = static_cast<float>(x) + 0.5F - stamp.centerCellX;
			const float dz = static_cast<float>(z) + 0.5F - stamp.centerCellZ;
			if(dx * dx + dz * dz > radius * radius) continue;
			const std::size_t index = static_cast<std::size_t>(z) * width + x;
			const bool target = settings.mode == TerrainHoleBrushMode::PaintHole;
			if(ReadBit(sourceMask, index) == target) continue;
			WriteBit(outMask, index, target);
			changed = true;
		}
		return changed;
	}
}
