#include "VirtualGeometryPrototypeCodecOracle.h"

#include "VirtualGeometryCodecs.h"
#include "Resource/VirtualGeometryCodec.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace TerraEngine::Tools::VirtualGeometry
{
	namespace
	{
		std::uint64_t Hash(std::span<const std::uint8_t> bytes) noexcept
		{
			std::uint64_t value = 1469598103934665603ull;
			for(const std::uint8_t byte : bytes) value = (value ^ byte) * 1099511628211ull;
			return value;
		}
	}

	double PrototypeCodecOracleReport::Amplification() const noexcept
	{
		return decodedBytes == 0u ? 0.0 : static_cast<double>(storedBytes) / static_cast<double>(decodedBytes);
	}

	PrototypeCodecOracleReport MeasurePrototypeCodecOracle(const PrototypeCodecOracleInput& input)
	{
		PrototypeCodecOracleReport report{};
		report.sourceBytes = input.decodedBytes.size();
		report.decodedBytes = input.decodedBytes.size();
		report.sourceIntegrity = Hash(input.decodedBytes);
		if(input.decodedBytes.empty() || input.pageSizeBytes == 0u || input.elementStride == 0u
			|| input.elementCount == 0u || input.decodedBytes.size() % input.elementStride != 0u
			|| static_cast<std::uint64_t>(input.elementCount) * input.elementStride != input.decodedBytes.size())
		{
			report.reason = "prototype-input-invalid-or-unavailable";
			return report;
		}
		const std::size_t pagePayloadBytes =
			(static_cast<std::size_t>(input.pageSizeBytes) / input.elementStride) * input.elementStride;
		if(pagePayloadBytes == 0u)
		{
			report.reason = "prototype-page-cannot-fit-one-element";
			return report;
		}
		for(std::size_t offset = 0u; offset < input.decodedBytes.size(); offset += pagePayloadBytes)
		{
			const auto page = input.decodedBytes.subspan(
				offset, std::min<std::size_t>(pagePayloadBytes, input.decodedBytes.size() - offset));
			const auto begin = std::chrono::steady_clock::now();
			const auto encoded = TerraEngine::Editor::EncodeVirtualGeometryPrototypeBlock(
				TerraEngine::Editor::VirtualGeometryPrototypeCodec::Expanded,
				{ page, static_cast<std::uint32_t>(page.size() / input.elementStride), input.elementStride, 0u });
			report.encodeNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count());
			if(!encoded.IsComplete()) { report.reason = encoded.error.empty() ? "prototype-encode-unavailable" : encoded.error; return report; }
			const auto decodeBegin = std::chrono::steady_clock::now();
			const auto decoded = TerraEngine::Resource::DecodeTemfProductionBlock({ encoded.descriptor, encoded.bytes, {}, encoded.descriptor.requiredCapabilityMask }, { .availableCapabilities = encoded.descriptor.requiredCapabilityMask });
			report.decodeNanoseconds += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - decodeBegin).count());
			if(!decoded.IsComplete() || decoded.bytes.size() != page.size() || !std::equal(page.begin(), page.end(), decoded.bytes.begin())) { report.reason = "prototype-round-trip-integrity-failed"; return report; }
			report.storedBytes += encoded.bytes.size();
			// Fold page hashes in order instead of XORing them. XOR would erase
			// two identical pages and falsely report a zero aggregate identity.
			report.encodedIntegrity =
				(report.encodedIntegrity ^ Hash(encoded.bytes)) * 1099511628211ull;
			report.decodedIntegrity =
				(report.decodedIntegrity ^ Hash(decoded.bytes)) * 1099511628211ull;
			++report.pageCount;
		}
		report.pageWasteBytes = static_cast<std::uint64_t>(report.pageCount) * input.pageSizeBytes - report.decodedBytes;
		report.available = true;
		report.reason = "offline-prototype-measured-not-production-selected";
		return report;
	}

	bool RunPrototypeCodecOracleSelfTest()
	{
		const std::array<std::uint8_t, 32u> bytes = { 0u };
		const auto report = MeasurePrototypeCodecOracle({ bytes, 16u, 1u, 32u });
		return report.available && report.pageCount == 2u && report.decodedIntegrity != 0u;
	}
} // namespace TerraEngine::Tools::VirtualGeometry
