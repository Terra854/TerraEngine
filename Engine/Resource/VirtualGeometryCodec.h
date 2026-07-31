// VirtualGeometryCodec.h - Bounded CPU/reference decoder for compact pages.
//
// Runtime code uses this validator before a page is published. It accepts no
// process-local pointers and allocates only after all header ranges are checked.

#pragma once

#include "Core/Export.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Resource
{
	struct TERRAENGINE_API VirtualGeometryCodecDecodeLimits
	{
		std::size_t maximumEncodedBytes = 128u * 1024u;
		std::size_t maximumDecodedBytes = 128u * 1024u;
		std::uint32_t maximumElementCount = 1u << 20u;
		std::uint64_t availableCapabilities = kVirtualGeometryCodecCapabilityAll;
	};

	struct TERRAENGINE_API VirtualGeometryCodecDecodeResult
	{
		std::vector<std::uint8_t> bytes;
		VirtualGeometryCompactBlockHeader header{};
		std::string error;

		[[nodiscard]] bool IsComplete() const noexcept { return error.empty() && !bytes.empty(); }
	};

	// Decodes an expanded, Terra, or meshoptimizer block after validating sizes,
	// integrity, and required capabilities. Failure leaves bytes empty.
	[[nodiscard]] TERRAENGINE_API VirtualGeometryCodecDecodeResult DecodeVirtualGeometryCompactBlock(
		std::span<const std::uint8_t> block, const VirtualGeometryCodecDecodeLimits &limits);

	// Validates only the VGXD directory and its entries. Callers may then fetch
	// page blocks asynchronously using the already-checked relative ranges.
	[[nodiscard]] TERRAENGINE_API bool ValidateVirtualGeometryExtensionDirectory(
		std::span<const std::uint8_t> directory, const VirtualGeometryCodecDecodeLimits &limits, std::string *outError = nullptr);
} // namespace TerraEngine::Resource
