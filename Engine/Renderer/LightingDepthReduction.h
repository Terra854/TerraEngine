// Numerical policy for the fixed lighting tiles. CPU results are an oracle,
// never evidence that a Vulkan shader ran or that its timestamps completed.
#pragma once
#include "Core/Export.h"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Renderer::LightingDepthReduction
{
	inline constexpr std::uint32_t kTilesX=16, kTilesY=9, kTileCount=kTilesX*kTilesY, kLaneCount=64;
	// These caps bound validation recipes, not ordinary production viewports.
	inline constexpr std::uint32_t kMaximumWidth=1920, kMaximumHeight=1080;
	struct Extent { std::uint32_t width=0,height=0; bool operator==(const Extent&) const=default; };
	struct TileRect { std::uint32_t xBegin=0,yBegin=0,xEnd=0,yEnd=0; };
	struct TileBounds { float minDepth=1,maxDepth=1; bool operator==(const TileBounds&) const=default; };
	struct TileCoverage { std::uint32_t visited=0,eligible=0; bool operator==(const TileCoverage&) const=default; };
	static_assert(sizeof(TileBounds)==8 && sizeof(TileCoverage)==8,"Shader bounds and validation coverage retain float2/uint2 ABI.");
	struct TileResult { TileBounds bounds{}; TileCoverage coverage{}; };
	using TileResults=std::array<TileResult,kTileCount>;
	struct PixelOverride { std::uint32_t x=0,y=0,bits=0; bool operator==(const PixelOverride&) const=default; };
	struct DepthRecipe { Extent extent{}; std::uint32_t fillBits=0x3f800000U; std::vector<PixelOverride> overrides; bool operator==(const DepthRecipe&) const=default; };
	TERRAENGINE_API bool BuildTileRect(Extent extent,std::uint32_t x,std::uint32_t y,TileRect& output);
	TERRAENGINE_API bool MaterializeDepthRecipe(const DepthRecipe& recipe,std::span<std::uint32_t> output);
	TERRAENGINE_API bool ReduceCompleteScan(Extent extent,std::span<const std::uint32_t> input,TileResults& output);
	TERRAENGINE_API bool ReduceCooperativeOracle(Extent extent,std::span<const std::uint32_t> input,TileResults& output);
	// The caller supplies independently captured identities from native recording,
	// submission and completion. This gate rejects absent proof; it creates none.
	struct ReadbackOwner {
		std::uint64_t rendererSession=0,frameSerial=0,commandSerial=0,submitSerial=0,completionSerial=0;
		std::uint32_t frameSlot=0,snapshotSlot=0,viewType=0,outputId=0;
		std::uint64_t backendGeneration=0,queryPoolGeneration=0,controlGeneration=0,optionsSessionGeneration=0,settingsGeneration=0,mutationGeneration=0;
		std::string operationId,fixtureStepId;
		bool operator==(const ReadbackOwner&) const=default;
	};
	struct OracleReadback {
		ReadbackOwner owner{}; Extent extent{};
		bool validationOptIn=false,recorded=false,submitted=false,completed=false;
		std::span<const std::uint32_t> input;
		std::span<const TileBounds> bounds;
		std::span<const TileCoverage> coverage;
	};
	TERRAENGINE_API bool ValidateCompletedOracleReadback(const OracleReadback& readback,const ReadbackOwner& expected);
}
