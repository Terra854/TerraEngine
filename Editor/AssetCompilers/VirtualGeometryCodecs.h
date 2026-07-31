// VirtualGeometryCodecs.h - Deterministic prototype block encoders.
//
// These small codecs let offline experiments compare page representations
// without changing TEMF v1 or relying on a runtime-installed dependency.

#pragma once

#include "Engine/File Formats/TerraEngineMeshFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	enum class VirtualGeometryPrototypeCodec : std::uint32_t
	{
		Expanded = 0u,
		Terra = 1u
	};

	struct VirtualGeometryCodecEncodeRequest
	{
		// source is the fully decoded page-local stream supplied by the cooker.
		std::span<const std::uint8_t> source;
		std::uint32_t elementCount = 0u;
		std::uint32_t elementStride = 0u;
		std::uint32_t flags = 0u;
	};

	struct VirtualGeometryEncodedBlock
	{
		std::vector<std::uint8_t> bytes;
		std::string error;

		[[nodiscard]] bool IsComplete() const noexcept { return error.empty() && !bytes.empty(); }
	};

	// Encodes either a direct expanded stream or the bounded Terra RLE prototype.
	// Both results use VirtualGeometryCompactBlockHeader for shared validation.
	[[nodiscard]] VirtualGeometryEncodedBlock EncodeVirtualGeometryPrototypeBlock(
		VirtualGeometryPrototypeCodec codec, const VirtualGeometryCodecEncodeRequest &request);
} // namespace TerraEngine::Editor
