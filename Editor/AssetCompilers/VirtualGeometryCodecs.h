// VirtualGeometryCodecs.h - production block profile evaluation helpers.
#pragma once

#include "Engine/File Formats/TerraEngineMeshFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	enum class VirtualGeometryPrototypeCodec : std::uint8_t { Expanded, Terra };
	struct VirtualGeometryCodecEncodeRequest { std::span<const std::uint8_t> bytes; std::uint32_t elementCount; std::uint32_t elementStride; std::uint32_t reserved; };
	struct VirtualGeometryEncodedBlock
	{
		std::vector<std::uint8_t> bytes;
		TemfBlockDescriptor descriptor{};
		std::string error;
		[[nodiscard]] bool IsComplete() const noexcept { return error.empty() && !bytes.empty(); }
	};
	[[nodiscard]] VirtualGeometryEncodedBlock EncodeVirtualGeometryPrototypeBlock(
		VirtualGeometryPrototypeCodec codec, const VirtualGeometryCodecEncodeRequest& request);
}
