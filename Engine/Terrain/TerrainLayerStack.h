// TerrainLayerStack.h -- stable terrain material metadata independent of paint order.

#pragma once

#include "Terrain/TerrainAreaCodec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	enum class TerrainLayerFallbackState : std::uint8_t { Healthy, MissingSource, InvalidSource, FallbackActive };

	struct TerrainLayerMapping
	{
		double scaleX = 1.0;
		double scaleZ = 1.0;
		double offsetX = 0.0;
		double offsetZ = 0.0;
		double rotationRadians = 0.0;
	};

	struct TerrainLayer
	{
		std::uint16_t stableId = 0u;
		std::string name;
		TerraEngine::UUID materialAssetUuid = TerraEngine::EMPTY_UUID;
		TerrainLayerMapping mapping{};
		bool enabled = true;
		TerrainLayerFallbackState fallbackState = TerrainLayerFallbackState::Healthy;
	};

	struct TerrainLayerStack
	{
		Generation generation = 0u;
		std::vector<TerrainLayer> layers;
	};

	enum class TerrainLayerPatchKind : std::uint8_t { Add, Replace, Remove, Move, SetEnabled, SetFallbackState };
	struct TerrainLayerPatch
	{
		TerrainLayerPatchKind kind = TerrainLayerPatchKind::Add;
		Generation expectedGeneration = 0u;
		std::uint16_t targetStableId = 0u;
		// The sentinel appends a new layer; explicit indices preserve undo order.
		std::uint32_t destinationIndex = 0xffffffffu;
		TerrainLayer layer{};
		bool enabled = true;
		TerrainLayerFallbackState fallbackState = TerrainLayerFallbackState::Healthy;
	};

	TERRAENGINE_API TerrainCodecResult ValidateTerrainLayerStack(const TerrainLayerStack& stack);
	TERRAENGINE_API TerrainCodecResult ApplyTerrainLayerPatch(const TerrainLayerStack& base, const TerrainLayerPatch& patch, TerrainLayerStack& outStack);
}
