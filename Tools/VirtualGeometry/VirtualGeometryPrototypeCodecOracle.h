#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Tools::VirtualGeometry
{
	struct PrototypeCodecOracleInput
	{
		std::span<const std::uint8_t> decodedBytes;
		std::uint32_t pageSizeBytes = 0u;
		std::uint32_t elementStride = 1u;
		std::uint32_t elementCount = 0u;
	};

	struct PrototypeCodecOracleReport
	{
		bool available = false;
		std::string reason;
		std::uint64_t sourceBytes = 0u;
		std::uint64_t storedBytes = 0u;
		std::uint64_t decodedBytes = 0u;
		std::uint64_t encodeNanoseconds = 0u;
		std::uint64_t decodeNanoseconds = 0u;
		std::uint64_t sourceIntegrity = 0u;
		std::uint64_t encodedIntegrity = 0u;
		std::uint64_t decodedIntegrity = 0u;
		std::uint32_t pageCount = 0u;
		std::uint64_t pageWasteBytes = 0u;
		[[nodiscard]] double Amplification() const noexcept;
	};

	[[nodiscard]] PrototypeCodecOracleReport MeasurePrototypeCodecOracle(
		const PrototypeCodecOracleInput& input);
	[[nodiscard]] bool RunPrototypeCodecOracleSelfTest();
} // namespace TerraEngine::Tools::VirtualGeometry
