// TerrainBrushEngine.cpp -- Bounds-safe fixed-order terrain brush kernels.

#include "TerrainBrushEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		float DeterministicNoise(std::uint32_t seed, std::uint32_t x, std::uint32_t z) noexcept
		{
			std::uint32_t value = seed ^ (x * 0x9E3779B9U) ^ (z * 0x85EBCA6BU);
			value ^= value >> 16U; value *= 0x7FEB352DU; value ^= value >> 15U; value *= 0x846CA68BU; value ^= value >> 16U;
			return static_cast<float>(value & 0xFFFFU) / 32767.5F - 1.0F;
		}

		std::int32_t SaturateToI32(float value) noexcept
		{
			const double preciseValue = static_cast<double>(value);
			if(preciseValue <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) return std::numeric_limits<std::int32_t>::min();
			if(preciseValue >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) return std::numeric_limits<std::int32_t>::max();
			return static_cast<std::int32_t>(preciseValue);
		}
	}

	bool TerrainBrushEngine::IsStrokeSampleDue(const TerrainBrushStamp& previous, const TerrainBrushStamp& candidate, float spacingSamples) noexcept
	{
		const float dx = candidate.centerX - previous.centerX;
		const float dz = candidate.centerZ - previous.centerZ;
		return dx * dx + dz * dz >= std::max(0.0F, spacingSamples) * std::max(0.0F, spacingSamples);
	}

	float TerrainBrushEngine::EvaluateFalloff(TerrainBrushFalloff falloff, float normalizedDistance) noexcept
	{
		if(normalizedDistance > 1.0F) return 0.0F;
		const float t = std::clamp(normalizedDistance, 0.0F, 1.0F);
		switch(falloff)
		{
		case TerrainBrushFalloff::Constant: return 1.0F;
		case TerrainBrushFalloff::Linear: return 1.0F - t;
		case TerrainBrushFalloff::SmoothStep: return 1.0F - t * t * (3.0F - 2.0F * t);
		case TerrainBrushFalloff::Gaussian: return std::exp(-4.5F * t * t);
		}
		return 0.0F;
	}

	void TerrainBrushEngine::ApplyStamp(std::span<const std::int32_t> source, std::span<std::int32_t> output,
		std::uint32_t width, std::uint32_t height, const TerrainBrushSettings& settings,
		const TerrainBrushStamp& stamp, std::span<const std::uint8_t> mask)
	{
		const std::size_t required = static_cast<std::size_t>(width) * height;
		if(width == 0U || height == 0U || source.size() < required || output.size() < required || (!mask.empty() && mask.size() < required)) return;
		std::copy_n(source.begin(), required, output.begin());
		const float radius = std::max(0.001F, settings.radiusSamples);
		const float c = std::cos(settings.rotationRadians), s = std::sin(settings.rotationRadians);
		const float signedStrength = settings.strengthMm * std::clamp(settings.pressure, 0.0F, 1.0F);
		for(std::uint32_t z = 0U; z < height; ++z) for(std::uint32_t x = 0U; x < width; ++x)
		{
			const float dx = static_cast<float>(x) - stamp.centerX;
			const float dz = static_cast<float>(z) - stamp.centerZ;
			const float localX = c * dx - s * dz, localZ = s * dx + c * dz;
			const float distance = std::sqrt(localX * localX + localZ * localZ) / radius;
			if(distance > 1.0F) continue;
			const std::size_t index = static_cast<std::size_t>(z) * width + x;
			const float weight = EvaluateFalloff(settings.falloff, distance) * (mask.empty() ? 1.0F : static_cast<float>(mask[index]) / 255.0F);
			const float oldValue = static_cast<float>(source[index]);
			float target = oldValue;
			switch(settings.mode)
			{
			case TerrainBrushMode::Raise: target = oldValue + signedStrength; break;
			case TerrainBrushMode::Lower: target = oldValue - signedStrength; break;
			case TerrainBrushMode::Smooth:
			{
				const std::uint32_t left = x == 0U ? x : x - 1U, right = std::min(x + 1U, width - 1U);
				const std::uint32_t up = z == 0U ? z : z - 1U, down = std::min(z + 1U, height - 1U);
				target = static_cast<float>(source[static_cast<std::size_t>(z) * width + left] + source[static_cast<std::size_t>(z) * width + right]
					+ source[static_cast<std::size_t>(up) * width + x] + source[static_cast<std::size_t>(down) * width + x]) * 0.25F;
				break;
			}
			case TerrainBrushMode::FlattenValue: case TerrainBrushMode::FlattenPicked: target = static_cast<float>(settings.flattenHeightMm); break;
			case TerrainBrushMode::Terrace: target = std::round(oldValue / std::max(1, settings.terraceStepMm)) * std::max(1, settings.terraceStepMm); break;
			case TerrainBrushMode::Ramp: target = static_cast<float>(settings.flattenHeightMm) + localX * signedStrength / radius; break;
			case TerrainBrushMode::Stamp: target = oldValue + signedStrength * (1.0F - distance); break;
			case TerrainBrushMode::Noise: target = oldValue + signedStrength * DeterministicNoise(settings.randomSeed, x, z); break;
			}
			output[index] = SaturateToI32(oldValue + (target - oldValue) * weight);
		}
	}
}
