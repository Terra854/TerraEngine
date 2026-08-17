// TerrainSurfaceBrush.cpp -- Stable-ID surface paint and preview helpers.

#include "TerrainSurfaceBrush.h"

#include <algorithm>
#include <array>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
namespace
{
	using TerraEngine::Terrain::TerrainWeightContribution;
	using TerraEngine::Terrain::TerrainWeightSample;

	std::vector<TerrainWeightContribution> Contributions(const TerrainWeightSample& sample)
	{
		std::vector<TerrainWeightContribution> result;
		result.reserve(sample.slots.size());
		for(const auto& slot : sample.slots)
			if(slot.layerId != 0u && slot.weight != 0u) result.push_back({slot.layerId, slot.weight});
		return result;
	}

	std::uint32_t WeightOf(const std::vector<TerrainWeightContribution>& values, std::uint16_t layerId)
	{
		for(const auto& value : values) if(value.layerId == layerId) return value.weight;
		return 0u;
	}

	void SetWeight(std::vector<TerrainWeightContribution>& values, std::uint16_t layerId, std::uint32_t weight)
	{
		auto found = std::find_if(values.begin(), values.end(), [layerId](const auto& value) { return value.layerId == layerId; });
		if(weight == 0u)
		{
			if(found != values.end()) values.erase(found);
			return;
		}
		if(found != values.end()) found->weight = weight;
		else values.push_back({layerId, weight});
	}

	std::uint16_t FindDisplacedLayer(const std::vector<TerrainWeightContribution>& before, const TerrainWeightSample& after, std::uint16_t incoming)
	{
		for(const auto& value : before)
		{
			if(value.layerId == incoming || value.weight == 0u) continue;
			const bool survived = std::any_of(after.slots.begin(), after.slots.end(), [&value](const auto& slot) { return slot.layerId == value.layerId; });
			if(!survived) return value.layerId;
		}
		return 0u;
	}
}

	TerrainSurfaceBrushResult TerrainSurfaceBrush::ApplySample(TerrainWeightSample& sample, const TerrainSurfaceBrushSettings& settings, std::uint8_t layerMask) noexcept
	{
		TerrainSurfaceBrushResult result;
		if(settings.layerId == 0u || (settings.useLayerMask && layerMask == 0u)) return result;
		const TerrainWeightSample beforeSample = sample;
		auto values = Contributions(sample);
		const std::uint32_t maskedStrength = static_cast<std::uint32_t>(settings.strength) * (settings.useLayerMask ? layerMask : 255u) / 255u;

		switch(settings.mode)
		{
		case TerrainSurfaceBrushMode::Paint:
		case TerrainSurfaceBrushMode::LayerMask:
			SetWeight(values, settings.layerId, std::min(255u, WeightOf(values, settings.layerId) + maskedStrength));
			break;
		case TerrainSurfaceBrushMode::Erase:
			SetWeight(values, settings.layerId, WeightOf(values, settings.layerId) > maskedStrength ? WeightOf(values, settings.layerId) - maskedStrength : 0u);
			break;
		case TerrainSurfaceBrushMode::Fill:
			values.assign(1u, {settings.layerId, 255u});
			break;
		case TerrainSurfaceBrushMode::Replace:
			if(settings.replacementLayerId == 0u) return result;
			for(auto& value : values) if(value.layerId == settings.layerId) value.layerId = settings.replacementLayerId;
			break;
		case TerrainSurfaceBrushMode::Normalize:
			break;
		}

		const TerrainWeightSample normalized = TerraEngine::Terrain::NormalizeTerrainWeights(values);
		result.displacedLayerId = FindDisplacedLayer(Contributions(beforeSample), normalized, settings.layerId);
		sample = normalized;
		result.changed = !(sample.slots == beforeSample.slots);
		return result;
	}

	TerrainSurfaceBrushResult TerrainSurfaceBrush::ApplyArea(std::span<TerrainWeightSample> samples, const TerrainSurfaceBrushSettings& settings, std::span<const std::uint8_t> layerMask) noexcept
	{
		TerrainSurfaceBrushResult combined;
		if(!layerMask.empty() && layerMask.size() < samples.size()) return combined;
		for(std::size_t index = 0u; index < samples.size(); ++index)
		{
			auto one = ApplySample(samples[index], settings, layerMask.empty() ? 255u : layerMask[index]);
			combined.changed = combined.changed || one.changed;
			if(combined.displacedLayerId == 0u) combined.displacedLayerId = one.displacedLayerId;
			combined.deterministicDisplacementMm += PreviewDisplacementMm(settings.displacementSeed, static_cast<std::int64_t>(index), 0, settings.strength);
		}
		return combined;
	}

	std::int32_t TerrainSurfaceBrush::PreviewDisplacementMm(std::uint32_t seed, std::int64_t sampleX, std::int64_t sampleZ, std::int32_t amplitudeMm) noexcept
	{
		// Integer mixing keeps previews identical across CPUs and independent of
		// floating-point rounding modes. The value is symmetric around zero.
		std::uint64_t value = static_cast<std::uint64_t>(seed) ^ (static_cast<std::uint64_t>(sampleX) * 0x9E3779B185EBCA87ULL) ^ (static_cast<std::uint64_t>(sampleZ) * 0xC2B2AE3D27D4EB4FULL);
		value ^= value >> 30u; value *= 0xBF58476D1CE4E5B9ULL; value ^= value >> 27u; value *= 0x94D049BB133111EBULL; value ^= value >> 31u;
		const std::int64_t signedUnit = static_cast<std::int64_t>(value & 0xFFFFu) - 32768;
		const std::int64_t result = signedUnit * static_cast<std::int64_t>(amplitudeMm) / 32768;
		return static_cast<std::int32_t>(std::clamp(result, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()), static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
	}
}
