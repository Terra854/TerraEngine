// TerrainTypes.cpp -- Checked integer terrain coordinate helpers.

#include "Terrain/TerrainTypes.h"

#include <limits>

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
