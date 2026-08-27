// TerrainTypes.h -- Exact, bounded identifiers used by authored terrain.
//
// Terrain data stays on an integer lattice so edits remain deterministic even
// when a scene instance is positioned in a very large world.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace TerraEngine::Terrain
{
	inline constexpr std::int64_t kTerrainAreaExtentSamples = 256;
	inline constexpr std::uint32_t kTerrainAreaSampleCount = 256u * 256u;
	inline constexpr std::uint32_t kTerrainMaximumWeightGroups = 16u;

	using Generation = std::uint64_t;

	// ContentHash keeps the xxHash3 128-bit result in a typed value rather than
	// passing strings through worker and publication code.
	struct ContentHash
	{
		std::uint64_t low64 = 0u;
		std::uint64_t high64 = 0u;

		[[nodiscard]] constexpr bool IsValid() const noexcept { return low64 != 0u || high64 != 0u; }
		[[nodiscard]] constexpr bool operator==(const ContentHash&) const noexcept = default;
	};

	struct ContentHashHasher
	{
		[[nodiscard]] std::size_t operator()(const ContentHash& value) const noexcept;
	};

	struct TerrainSampleCoordinate
	{
		std::int64_t x = 0;
		std::int64_t z = 0;
		[[nodiscard]] constexpr bool operator==(const TerrainSampleCoordinate&) const noexcept = default;
	};

	struct TerrainAreaCoordinate
	{
		std::int64_t x = 0;
		std::int64_t z = 0;
		[[nodiscard]] constexpr bool operator==(const TerrainAreaCoordinate&) const noexcept = default;
		[[nodiscard]] constexpr bool operator<(const TerrainAreaCoordinate& other) const noexcept
		{
			return x != other.x ? x < other.x : z < other.z;
		}
	};

	struct TerrainAreaCoordinateHasher
	{
		[[nodiscard]] std::size_t operator()(const TerrainAreaCoordinate& value) const noexcept;
	};

	struct TerrainAreaId
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		TerrainAreaCoordinate coordinate{};
		[[nodiscard]] constexpr bool operator==(const TerrainAreaId&) const noexcept = default;
	};

	// Half-open bounds prevent neighbouring edits from owning their shared edge.
	struct TerrainSampleBounds
	{
		std::int64_t minX = 0;
		std::int64_t minZ = 0;
		std::int64_t maxXExclusive = 0;
		std::int64_t maxZExclusive = 0;

		[[nodiscard]] constexpr bool operator==(const TerrainSampleBounds&) const noexcept = default;
		[[nodiscard]] bool IsValid() const noexcept;
		[[nodiscard]] bool Contains(TerrainSampleCoordinate sample) const noexcept;
	};

	enum class TerrainChannelKind : std::uint16_t
	{
		Height = 1u,
		Holes = 2u,
		WeightGroup = 3u
	};

	enum class TerrainResultCode : std::uint8_t
	{
		Ok,
		InvalidArgument,
		OutOfBounds,
		Overflow,
		BudgetExceeded,
		CorruptData,
		UnsupportedVersion,
		HashMismatch,
		Conflict,
		Cancelled,
		Failed
	};

	// Root diagnostics are stable identifiers intended for editor recovery UX,
	// logs, and automated validation. Keep their spelling stable across releases.
	enum class TerrainRootDiagnosticCode : std::uint8_t
	{
		None,
		UnsupportedTerrainRoot,
		TerrainLayerDefinitionLimitExceeded,
		TerrainEnabledLayerLimitViolated,
		TerrainLayerIdentityInvalid,
		TerrainLayerNameInvalid,
		TerrainLayerMappingInvalid,
		TerrainAreaReferenceInvalid,
		TerrainRootMalformed,
		TerrainRootPublicationRejected
	};

	TERRAENGINE_API std::int64_t FloorDivideTerrainCoordinate(std::int64_t value, std::int64_t divisor, bool& outOverflow) noexcept;
	TERRAENGINE_API std::int64_t FloorModuloTerrainCoordinate(std::int64_t value, std::int64_t divisor, bool& outOverflow) noexcept;
	TERRAENGINE_API std::optional<TerrainAreaCoordinate> OwnerArea(TerrainSampleCoordinate sample) noexcept;
	TERRAENGINE_API std::optional<TerrainSampleCoordinate> LocalSample(TerrainSampleCoordinate sample) noexcept;
	TERRAENGINE_API std::string ToHexString(ContentHash hash);
	TERRAENGINE_API std::string_view ToString(TerrainResultCode code) noexcept;
	TERRAENGINE_API std::string_view ToString(TerrainRootDiagnosticCode code) noexcept;
}
