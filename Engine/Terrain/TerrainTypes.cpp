// TerrainTypes.cpp -- Checked integer terrain coordinate helpers.

#include "Terrain/TerrainTypes.h"

#include <limits>
#include <algorithm>

namespace TerraEngine::Terrain
{
	std::size_t ContentHashHasher::operator()(const ContentHash& value) const noexcept
	{
		return static_cast<std::size_t>(value.low64 ^ (value.high64 + 0x9e3779b97f4a7c15ull + (value.low64 << 6u) + (value.low64 >> 2u)));
	}

	std::size_t TerrainAreaCoordinateHasher::operator()(const TerrainAreaCoordinate& value) const noexcept
	{
		const auto first = static_cast<std::uint64_t>(value.x);
		const auto second = static_cast<std::uint64_t>(value.z);
		return static_cast<std::size_t>(first ^ (second + 0x9e3779b97f4a7c15ull + (first << 6u) + (first >> 2u)));
	}

	bool TerrainSampleBounds::IsValid() const noexcept { return minX <= maxXExclusive && minZ <= maxZExclusive; }
	bool TerrainSampleBounds::Contains(TerrainSampleCoordinate sample) const noexcept
	{
		return IsValid() && sample.x >= minX && sample.x < maxXExclusive && sample.z >= minZ && sample.z < maxZExclusive;
	}

	bool TerrainAreaBounds::IsValid() const noexcept
	{
		return minX < maxXExclusive && minZ < maxZExclusive;
	}

	bool TerrainAreaBounds::Contains(TerrainAreaCoordinate area) const noexcept
	{
		return IsValid() && area.x >= minX && area.x < maxXExclusive
			&& area.z >= minZ && area.z < maxZExclusive;
	}

	std::optional<std::size_t> TerrainAreaBounds::AreaCount() const noexcept
	{
		if (!IsValid()) return std::nullopt;
		// Subtract in unsigned space. A valid signed half-open range can span more
		// than INT64_MAX coordinates, so signed subtraction would itself overflow
		// before the allocation budget had a chance to reject the range.
		const auto checkedExtent = [](std::int64_t minimum, std::int64_t maximum) {
			return static_cast<std::uint64_t>(maximum) - static_cast<std::uint64_t>(minimum);
		};
		const std::uint64_t width = checkedExtent(minX, maxXExclusive);
		const std::uint64_t depth = checkedExtent(minZ, maxZExclusive);
		if (width != 0u && depth > (std::numeric_limits<std::size_t>::max)() / width) return std::nullopt;
		return static_cast<std::size_t>(width * depth);
	}

	bool TerrainAreaActiveRect::IsValid() const noexcept
	{
		return minX < maxXExclusive && minZ < maxZExclusive
			&& maxXExclusive <= kTerrainAreaExtentSamples && maxZExclusive <= kTerrainAreaExtentSamples;
	}

	std::int64_t FloorDivideTerrainCoordinate(std::int64_t value, std::int64_t divisor, bool& outOverflow) noexcept
	{
		outOverflow = divisor <= 0 || (value == std::numeric_limits<std::int64_t>::min() && divisor == -1);
		if (outOverflow) return 0;
		const std::int64_t quotient = value / divisor;
		const std::int64_t remainder = value % divisor;
		return remainder < 0 ? quotient - 1 : quotient;
	}

	std::int64_t FloorModuloTerrainCoordinate(std::int64_t value, std::int64_t divisor, bool& outOverflow) noexcept
	{
		const std::int64_t quotient = FloorDivideTerrainCoordinate(value, divisor, outOverflow);
		if (outOverflow) return 0;
		return value - quotient * divisor;
	}

	std::optional<TerrainAreaCoordinate> OwnerArea(TerrainSampleCoordinate sample) noexcept
	{
		bool overflow = false;
		const auto x = FloorDivideTerrainCoordinate(sample.x, kTerrainAreaExtentSamples, overflow);
		const auto z = FloorDivideTerrainCoordinate(sample.z, kTerrainAreaExtentSamples, overflow);
		return overflow ? std::nullopt : std::optional<TerrainAreaCoordinate>{{x, z}};
	}

	std::optional<TerrainSampleCoordinate> LocalSample(TerrainSampleCoordinate sample) noexcept
	{
		bool overflow = false;
		const auto x = FloorModuloTerrainCoordinate(sample.x, kTerrainAreaExtentSamples, overflow);
		const auto z = FloorModuloTerrainCoordinate(sample.z, kTerrainAreaExtentSamples, overflow);
		return overflow ? std::nullopt : std::optional<TerrainSampleCoordinate>{{x, z}};
	}

	std::optional<TerrainAreaBounds> TerrainAreasForSamples(TerrainSampleBounds bounds) noexcept
	{
		if (!bounds.IsValid() || bounds.minX == bounds.maxXExclusive || bounds.minZ == bounds.maxZExclusive)
			return std::nullopt;
		bool overflow = false;
		const std::int64_t minX = FloorDivideTerrainCoordinate(bounds.minX, kTerrainAreaExtentSamples, overflow);
		const std::int64_t minZ = FloorDivideTerrainCoordinate(bounds.minZ, kTerrainAreaExtentSamples, overflow);
		if (overflow || bounds.maxXExclusive == (std::numeric_limits<std::int64_t>::min)()
			|| bounds.maxZExclusive == (std::numeric_limits<std::int64_t>::min)()) return std::nullopt;
		// The final owned sample determines the final owning area. Adding one in
		// area space is checked independently to keep INT64 edges well-defined.
		const std::int64_t maxOwnedX = FloorDivideTerrainCoordinate(bounds.maxXExclusive - 1, kTerrainAreaExtentSamples, overflow);
		const std::int64_t maxOwnedZ = FloorDivideTerrainCoordinate(bounds.maxZExclusive - 1, kTerrainAreaExtentSamples, overflow);
		if (overflow || maxOwnedX == (std::numeric_limits<std::int64_t>::max)()
			|| maxOwnedZ == (std::numeric_limits<std::int64_t>::max)()) return std::nullopt;
		return TerrainAreaBounds{minX, minZ, maxOwnedX + 1, maxOwnedZ + 1};
	}

	std::optional<TerrainAreaActiveRect> TerrainActiveRectForArea(
		TerrainSampleBounds bounds, TerrainAreaCoordinate area) noexcept
	{
		const auto coveredAreas = TerrainAreasForSamples(bounds);
		if (!coveredAreas || !coveredAreas->Contains(area)) return std::nullopt;
		if (area.x > (std::numeric_limits<std::int64_t>::max)() / kTerrainAreaExtentSamples
			|| area.x < (std::numeric_limits<std::int64_t>::min)() / kTerrainAreaExtentSamples
			|| area.z > (std::numeric_limits<std::int64_t>::max)() / kTerrainAreaExtentSamples
			|| area.z < (std::numeric_limits<std::int64_t>::min)() / kTerrainAreaExtentSamples) return std::nullopt;
		const std::int64_t originX = area.x * kTerrainAreaExtentSamples;
		const std::int64_t originZ = area.z * kTerrainAreaExtentSamples;
		// Compare before subtracting so extreme root bounds cannot overflow an
		// intermediate. Subtraction is performed only when the result is 1..255.
		const auto toLocal = [](std::int64_t value, std::int64_t origin) -> std::uint16_t {
			if (value <= origin) return 0u;
			if (origin <= (std::numeric_limits<std::int64_t>::max)() - kTerrainAreaExtentSamples
				&& value >= origin + kTerrainAreaExtentSamples)
				return static_cast<std::uint16_t>(kTerrainAreaExtentSamples);
			return static_cast<std::uint16_t>(value - origin);
		};
		const auto localMinX = toLocal(bounds.minX, originX);
		const auto localMinZ = toLocal(bounds.minZ, originZ);
		const auto localMaxX = toLocal(bounds.maxXExclusive, originX);
		const auto localMaxZ = toLocal(bounds.maxZExclusive, originZ);
		TerrainAreaActiveRect result{static_cast<std::uint16_t>(localMinX), static_cast<std::uint16_t>(localMinZ),
			static_cast<std::uint16_t>(localMaxX), static_cast<std::uint16_t>(localMaxZ)};
		return result.IsValid() ? std::optional<TerrainAreaActiveRect>{result} : std::nullopt;
	}

	std::optional<std::vector<TerrainAreaCoordinate>> EnumerateTerrainAreas(
		TerrainSampleBounds bounds, std::size_t maximumAreas) noexcept
	{
		const auto areaBounds = TerrainAreasForSamples(bounds);
		const auto count = areaBounds ? areaBounds->AreaCount() : std::nullopt;
		if (!count || *count > maximumAreas) return std::nullopt;
		std::vector<TerrainAreaCoordinate> result;
		result.reserve(*count);
		for (std::int64_t z = areaBounds->minZ; z < areaBounds->maxZExclusive; ++z)
			for (std::int64_t x = areaBounds->minX; x < areaBounds->maxXExclusive; ++x)
				result.push_back({x, z});
		return result;
	}

	std::string ToHexString(ContentHash hash)
	{
		static constexpr char kDigits[] = "0123456789abcdef";
		std::string text(32u, '0');
		const std::array<std::uint64_t, 2u> words = {hash.high64, hash.low64};
		for (std::size_t word = 0; word < words.size(); ++word)
		{
			for (std::size_t byte = 0; byte < 8u; ++byte)
			{
				const auto value = static_cast<std::uint8_t>(words[word] >> ((7u - byte) * 8u));
				text[(word * 16u) + (byte * 2u)] = kDigits[value >> 4u];
				text[(word * 16u) + (byte * 2u) + 1u] = kDigits[value & 0x0fu];
			}
		}
		return text;
	}

	std::string_view ToString(TerrainResultCode code) noexcept
	{
		switch (code)
		{
		case TerrainResultCode::Ok: return "Ok";
		case TerrainResultCode::InvalidArgument: return "InvalidArgument";
		case TerrainResultCode::OutOfBounds: return "OutOfBounds";
		case TerrainResultCode::Overflow: return "Overflow";
		case TerrainResultCode::BudgetExceeded: return "BudgetExceeded";
		case TerrainResultCode::CorruptData: return "CorruptData";
		case TerrainResultCode::UnsupportedVersion: return "UnsupportedVersion";
		case TerrainResultCode::HashMismatch: return "HashMismatch";
		case TerrainResultCode::Conflict: return "Conflict";
		case TerrainResultCode::Cancelled: return "Cancelled";
		case TerrainResultCode::Failed: return "Failed";
		}
		return "Unknown";
	}

	std::string_view ToString(TerrainRootDiagnosticCode code) noexcept
	{
		switch (code)
		{
		case TerrainRootDiagnosticCode::None: return "None";
		case TerrainRootDiagnosticCode::UnsupportedTerrainRoot: return "UnsupportedTerrainRoot";
		case TerrainRootDiagnosticCode::TerrainLayerDefinitionLimitExceeded: return "TerrainLayerDefinitionLimitExceeded";
		case TerrainRootDiagnosticCode::TerrainEnabledLayerLimitViolated: return "TerrainEnabledLayerLimitViolated";
		case TerrainRootDiagnosticCode::TerrainLayerIdentityInvalid: return "TerrainLayerIdentityInvalid";
		case TerrainRootDiagnosticCode::TerrainLayerNameInvalid: return "TerrainLayerNameInvalid";
		case TerrainRootDiagnosticCode::TerrainLayerMappingInvalid: return "TerrainLayerMappingInvalid";
		case TerrainRootDiagnosticCode::TerrainAreaReferenceInvalid: return "TerrainAreaReferenceInvalid";
		case TerrainRootDiagnosticCode::TerrainRootMalformed: return "TerrainRootMalformed";
		case TerrainRootDiagnosticCode::TerrainRootPublicationRejected: return "TerrainRootPublicationRejected";
		}
		return "Unknown";
	}
}
