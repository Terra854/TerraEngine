// TerrainLayerStack.cpp -- copy-on-write terrain layer metadata operations.

#include "Terrain/TerrainLayerStack.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>
#include <utility>

namespace TerraEngine::Terrain
{
namespace
{
	double PositiveModulo(double value, double modulus) noexcept
	{
		const double reduced = std::fmod(value, modulus);
		return reduced < 0.0 ? reduced + modulus : reduced;
	}

}

	bool IsValidTerrainLayerName(std::string_view name) noexcept
	{
		if(name.empty() || name.size() > Format::kTerrainMaximumLayerNameBytes
			|| name.front() == ' ' || name.back() == ' ') return false;
		for(std::size_t index = 0u; index < name.size();)
		{
			const auto byte = static_cast<unsigned char>(name[index]);
			if(byte < 0x20u || byte == 0x7fu) return false;
			if(byte < 0x80u) { ++index; continue; }
			const auto continuation = [&](std::size_t offset) {
				return index + offset < name.size()
					&& (static_cast<unsigned char>(name[index + offset]) & 0xc0u) == 0x80u;
			};
			if(byte >= 0xc2u && byte <= 0xdfu && continuation(1u)) { index += 2u; continue; }
			if(byte >= 0xe0u && byte <= 0xefu && continuation(1u) && continuation(2u))
			{
				const auto second = static_cast<unsigned char>(name[index + 1u]);
				if((byte != 0xe0u || second >= 0xa0u) && (byte != 0xedu || second < 0xa0u)) { index += 3u; continue; }
			}
			if(byte >= 0xf0u && byte <= 0xf4u && continuation(1u) && continuation(2u) && continuation(3u))
			{
				const auto second = static_cast<unsigned char>(name[index + 1u]);
				if((byte != 0xf0u || second >= 0x90u) && (byte != 0xf4u || second < 0x90u)) { index += 4u; continue; }
			}
			return false;
		}
		return true;
	}

	bool EqualNormalizedTerrainLayerName(std::string_view left, std::string_view right) noexcept
	{
		if(left.size() != right.size()) return false;
		for(std::size_t index = 0u; index < left.size(); ++index)
		{
			const auto fold = [](unsigned char value) {
				return value >= 'A' && value <= 'Z'
					? static_cast<unsigned char>(value - 'A' + 'a') : value;
			};
			if(fold(static_cast<unsigned char>(left[index]))
				!= fold(static_cast<unsigned char>(right[index]))) return false;
		}
		return true;
	}

	TerrainCodecResult CanonicalizeTerrainLayerMapping(
		const TerrainLayerMapping& mapping, TerrainLayerMapping& outCanonical) noexcept
	{
		// The patch path intentionally permits in-place canonicalization.
		const TerrainLayerMapping source = mapping;
		outCanonical = {};
		if((source.mode != TerrainLayerMappingMode::TerrainXZ && source.mode != TerrainLayerMappingMode::Triplanar)
			|| !std::isfinite(source.tileSizeXMetres) || !std::isfinite(source.tileSizeZMetres)
			|| !std::isfinite(source.offsetXMetres) || !std::isfinite(source.offsetZMetres)
			|| !std::isfinite(source.rotationRadians) || !std::isfinite(source.triplanarSharpness)
			|| source.tileSizeXMetres <= 0.0 || source.tileSizeZMetres <= 0.0
			|| source.triplanarSharpness <= 0.0)
		{
			return {TerrainResultCode::InvalidArgument,
				"Terrain mapping requires a known mode and finite positive tile sizes and triplanar sharpness."};
		}

		outCanonical = source;
		outCanonical.offsetXMetres = PositiveModulo(source.offsetXMetres, source.tileSizeXMetres);
		outCanonical.offsetZMetres = PositiveModulo(source.offsetZMetres, source.tileSizeZMetres);
		outCanonical.rotationRadians = PositiveModulo(source.rotationRadians, 2.0 * std::numbers::pi);
		// The shader exponent is deliberately bounded; extreme values create no
		// useful visual distinction and amplify floating-point noise at axis ties.
		outCanonical.triplanarSharpness = std::clamp(source.triplanarSharpness, 1.0, 32.0);
		return {};
	}

	TerrainCodecResult ValidateTerrainLayerStack(const TerrainLayerStack& stack)
	{
		if (stack.generation == 0u || stack.layers.empty() || stack.layers.size() > Format::kTerrainMaximumLayers)
			return {TerrainResultCode::InvalidArgument, "Terrain layers require a non-zero generation and one to sixty-four stable entries."};
		std::size_t enabledCount = 0u;
		for (std::size_t index = 0; index < stack.layers.size(); ++index)
		{
			const TerrainLayer& layer = stack.layers[index];
			if (layer.enabled) ++enabledCount;
			TerrainLayerMapping canonical{};
			const bool knownFallbackState = layer.fallbackState == TerrainLayerFallbackState::Healthy
				|| layer.fallbackState == TerrainLayerFallbackState::MissingSource
				|| layer.fallbackState == TerrainLayerFallbackState::InvalidSource
				|| layer.fallbackState == TerrainLayerFallbackState::FallbackActive;
			if (layer.stableId == 0u || layer.stableId == kTerrainDiagnosticFallbackLayerId
				|| !IsValidTerrainLayerName(layer.name) || !knownFallbackState
				|| !CanonicalizeTerrainLayerMapping(layer.mapping, canonical).Succeeded())
				return {TerrainResultCode::InvalidArgument, "Terrain layer metadata is missing a non-reserved stable id, name, or finite mapping."};
			for (std::size_t prior = 0; prior < index; ++prior)
				if (stack.layers[prior].stableId == layer.stableId || EqualNormalizedTerrainLayerName(stack.layers[prior].name, layer.name))
					return {TerrainResultCode::InvalidArgument, "Terrain layer ids and names must be unique regardless of presentation order."};
		}
		if (enabledCount == 0u || enabledCount > Format::kTerrainMaximumEnabledLayers)
			return {TerrainResultCode::InvalidArgument, "Terrain requires between one and sixteen enabled layers."};
		return {};
	}

	TerrainCodecResult ApplyTerrainLayerPatch(const TerrainLayerStack& base, const TerrainLayerPatch& patch, TerrainLayerStack& outStack)
	{
		outStack = {};
		if (const auto validation = ValidateTerrainLayerStack(base); !validation.Succeeded()) return validation;
		if (patch.expectedGeneration != 0u && patch.expectedGeneration != base.generation)
			return {TerrainResultCode::Conflict, "Terrain layer metadata changed while this patch was preparing."};
		outStack = base;
		auto found = std::find_if(outStack.layers.begin(), outStack.layers.end(), [&patch](const TerrainLayer& layer) { return layer.stableId == patch.targetStableId; });
		switch (patch.kind)
		{
		case TerrainLayerPatchKind::Add:
			{
				const std::size_t insertion = patch.destinationIndex == 0xffffffffu ? outStack.layers.size() : patch.destinationIndex;
				if (patch.layer.stableId == 0u || found != outStack.layers.end() || insertion > outStack.layers.size()) return {TerrainResultCode::InvalidArgument, "New terrain layers require an unused stable id and valid presentation index."};
				TerrainLayer canonicalLayer = patch.layer;
				if(const TerrainCodecResult mapping = CanonicalizeTerrainLayerMapping(
					canonicalLayer.mapping, canonicalLayer.mapping); !mapping.Succeeded()) return mapping;
				outStack.layers.insert(outStack.layers.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(canonicalLayer));
			}
			break;
		case TerrainLayerPatchKind::Replace:
			if (found == outStack.layers.end() || patch.layer.stableId != patch.targetStableId) return {TerrainResultCode::InvalidArgument, "Replacement terrain metadata must retain the target stable id."};
			{
				TerrainLayer canonicalLayer = patch.layer;
				if(const TerrainCodecResult mapping = CanonicalizeTerrainLayerMapping(
					canonicalLayer.mapping, canonicalLayer.mapping); !mapping.Succeeded()) return mapping;
				*found = std::move(canonicalLayer);
			}
			break;
		case TerrainLayerPatchKind::Remove:
			if (found == outStack.layers.end() || outStack.layers.size() == 1u) return {TerrainResultCode::InvalidArgument, "Terrain requires one fallback-valid layer after metadata changes."};
			outStack.layers.erase(found);
			break;
		case TerrainLayerPatchKind::Move:
			if (found == outStack.layers.end() || patch.destinationIndex >= outStack.layers.size()) return {TerrainResultCode::InvalidArgument, "Terrain layer move target is outside the presentation order."};
			{
				TerrainLayer moved = *found;
				outStack.layers.erase(found);
				outStack.layers.insert(outStack.layers.begin() + patch.destinationIndex, std::move(moved));
			}
			break;
		case TerrainLayerPatchKind::SetEnabled:
			if (found == outStack.layers.end()) return {TerrainResultCode::InvalidArgument, "Terrain layer enable target is missing."};
			found->enabled = patch.enabled;
			break;
		case TerrainLayerPatchKind::SetFallbackState:
			if (found == outStack.layers.end()) return {TerrainResultCode::InvalidArgument, "Terrain layer fallback target is missing."};
			found->fallbackState = patch.fallbackState;
			break;
		}
		++outStack.generation;
		return ValidateTerrainLayerStack(outStack);
	}

	TerrainResolvedWeightSample ResolveTerrainWeightSample(
		const TerrainWeightSample& sample, const TerrainLayerStack& stack) noexcept
	{
		struct WeightedId
		{
			std::uint16_t stableLayerId = 0u;
			std::uint32_t weight = 0u;
			bool diagnosticFallback = false;
		};
		std::array<WeightedId, 4u> accumulated{};
		std::size_t accumulatedCount = 0u;
		std::uint32_t totalWeight = 0u;

		for(const TerrainWeightSlot& slot : sample.slots)
		{
			if(slot.weight == 0u) continue;
			totalWeight += slot.weight;
			const auto layer = std::find_if(stack.layers.begin(), stack.layers.end(), [&](const TerrainLayer& candidate) {
				return candidate.stableId == slot.layerId && candidate.enabled;
			});
			const bool diagnostic = slot.layerId == 0u || layer == stack.layers.end();
			const std::uint16_t resolvedId = diagnostic ? kTerrainDiagnosticFallbackLayerId : slot.layerId;
			auto prior = std::find_if(accumulated.begin(), accumulated.begin() + static_cast<std::ptrdiff_t>(accumulatedCount),
				[&](const WeightedId& value) { return value.stableLayerId == resolvedId; });
			if(prior != accumulated.begin() + static_cast<std::ptrdiff_t>(accumulatedCount))
			{
				prior->weight += slot.weight;
			}
			else if(accumulatedCount < accumulated.size())
			{
				accumulated[accumulatedCount++] = {resolvedId, slot.weight, diagnostic};
			}
		}

		TerrainResolvedWeightSample result{};
		if(totalWeight == 0u)
		{
			result.contributors[0] = {kTerrainDiagnosticFallbackLayerId, 1.0, true};
			result.contributorCount = 1u;
			result.usedDiagnosticFallback = true;
			return result;
		}

		// Merging every unresolved ID before sorting guarantees that an authored
		// four-slot sample remains a bounded four-slot resolved value.
		std::sort(accumulated.begin(), accumulated.begin() + static_cast<std::ptrdiff_t>(accumulatedCount),
			[](const WeightedId& left, const WeightedId& right) {
				return left.weight != right.weight ? left.weight > right.weight
					: left.stableLayerId < right.stableLayerId;
			});
		result.contributorCount = accumulatedCount;
		for(std::size_t index = 0u; index < accumulatedCount; ++index)
		{
			result.contributors[index] = {
				accumulated[index].stableLayerId,
				static_cast<double>(accumulated[index].weight) / static_cast<double>(totalWeight),
				accumulated[index].diagnosticFallback};
			result.usedDiagnosticFallback = result.usedDiagnosticFallback
				|| accumulated[index].diagnosticFallback;
		}
		return result;
	}
}
