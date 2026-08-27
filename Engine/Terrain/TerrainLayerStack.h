// TerrainLayerStack.h -- stable terrain material metadata independent of paint order.

#pragma once

#include "Terrain/TerrainAreaCodec.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Terrain
{
	// This ID is reserved for renderer-owned diagnostic material input. It can
	// never identify an authored layer, so missing paint cannot alias valid art.
	inline constexpr std::uint16_t kTerrainDiagnosticFallbackLayerId = 0xffffu;

	enum class TerrainLayerFallbackState : std::uint8_t { Healthy, MissingSource, InvalidSource, FallbackActive };
	enum class TerrainLayerMappingMode : std::uint8_t { TerrainXZ, Triplanar };

	struct TerrainLayerMapping
	{
		TerrainLayerMappingMode mode = TerrainLayerMappingMode::TerrainXZ;
		double tileSizeXMetres = 4.0;
		double tileSizeZMetres = 4.0;
		double offsetXMetres = 0.0;
		double offsetZMetres = 0.0;
		double rotationRadians = 0.0;
		double triplanarSharpness = 4.0;

		[[nodiscard]] constexpr bool operator==(const TerrainLayerMapping&) const noexcept = default;
	};

	struct TerrainLayer
	{
		std::uint16_t stableId = 0u;
		std::string name;
		TerraEngine::UUID materialAssetUuid = TerraEngine::EMPTY_UUID;
		TerrainLayerMapping mapping{};
		bool enabled = true;
		bool allowFallback = true;
		TerrainLayerFallbackState fallbackState = TerrainLayerFallbackState::Healthy;
	};

	struct TerrainLayerStack
	{
		Generation generation = 0u;
		std::vector<TerrainLayer> layers;
	};

	struct TerrainResolvedWeightContributor
	{
		std::uint16_t stableLayerId = 0u;
		double normalizedWeight = 0.0;
		bool diagnosticFallback = false;
	};

	// Four value-only contributors match the authored control sample bound. A
	// worker can pass this record to material preparation without layer pointers.
	struct TerrainResolvedWeightSample
	{
		std::array<TerrainResolvedWeightContributor, 4u> contributors{};
		std::size_t contributorCount = 0u;
		bool usedDiagnosticFallback = false;
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

	// Commit paths use one canonical representation so equivalent rotations and
	// offsets produce identical TERRAROOT1 bytes and derived material candidates.
	TERRAENGINE_API TerrainCodecResult CanonicalizeTerrainLayerMapping(
		const TerrainLayerMapping& mapping, TerrainLayerMapping& outCanonical) noexcept;
	// Layer names are compared as stable identifiers. ASCII case folding is
	// deliberate because TERRAROOT1 stores UTF-8 bytes and does not promise a
	// locale-sensitive Unicode collation table at runtime.
	[[nodiscard]] TERRAENGINE_API bool IsValidTerrainLayerName(std::string_view name) noexcept;
	[[nodiscard]] TERRAENGINE_API bool EqualNormalizedTerrainLayerName(
		std::string_view left, std::string_view right) noexcept;
	TERRAENGINE_API TerrainCodecResult ValidateTerrainLayerStack(const TerrainLayerStack& stack);
	TERRAENGINE_API TerrainCodecResult ApplyTerrainLayerPatch(const TerrainLayerStack& base, const TerrainLayerPatch& patch, TerrainLayerStack& outStack);
	[[nodiscard]] TERRAENGINE_API TerrainResolvedWeightSample ResolveTerrainWeightSample(
		const TerrainWeightSample& sample, const TerrainLayerStack& stack) noexcept;
}
