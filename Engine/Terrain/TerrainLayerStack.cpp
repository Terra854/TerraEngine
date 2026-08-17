// TerrainLayerStack.cpp -- copy-on-write terrain layer metadata operations.

#include "Terrain/TerrainLayerStack.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace TerraEngine::Terrain
{
namespace
{
	bool ValidMapping(const TerrainLayerMapping& mapping) noexcept
	{
		return std::isfinite(mapping.scaleX) && std::isfinite(mapping.scaleZ)
			&& std::isfinite(mapping.offsetX) && std::isfinite(mapping.offsetZ) && std::isfinite(mapping.rotationRadians)
			&& mapping.scaleX > 0.0 && mapping.scaleZ > 0.0;
	}
}

	TerrainCodecResult ValidateTerrainLayerStack(const TerrainLayerStack& stack)
	{
		if (stack.generation == 0u || stack.layers.empty() || stack.layers.size() > Format::kTerrainMaximumLayers)
			return {TerrainResultCode::InvalidArgument, "Terrain layers require a non-zero generation and one to sixty-four stable entries."};
		for (std::size_t index = 0; index < stack.layers.size(); ++index)
		{
			const TerrainLayer& layer = stack.layers[index];
			if (layer.stableId == 0u || layer.name.empty() || !ValidMapping(layer.mapping))
				return {TerrainResultCode::InvalidArgument, "Terrain layer metadata is missing a stable id, name, or finite mapping."};
			for (std::size_t prior = 0; prior < index; ++prior)
				if (stack.layers[prior].stableId == layer.stableId || stack.layers[prior].name == layer.name)
					return {TerrainResultCode::InvalidArgument, "Terrain layer ids and names must be unique regardless of presentation order."};
		}
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
				outStack.layers.insert(outStack.layers.begin() + static_cast<std::ptrdiff_t>(insertion), patch.layer);
			}
			break;
		case TerrainLayerPatchKind::Replace:
			if (found == outStack.layers.end() || patch.layer.stableId != patch.targetStableId) return {TerrainResultCode::InvalidArgument, "Replacement terrain metadata must retain the target stable id."};
			*found = patch.layer;
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
}
