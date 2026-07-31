// VirtualGeometryMeshoptCodec.h - production evaluator profile declaration.
#pragma once
#include "VirtualGeometryCodecs.h"
namespace TerraEngine::Editor
{
	enum class VirtualGeometryMeshoptStreamKind : std::uint8_t { TriangleIndices, QuantizedAttributes };
	struct VirtualGeometryMeshoptCodecEncodeRequest { std::span<const std::uint8_t> bytes; VirtualGeometryMeshoptStreamKind kind; std::uint32_t elementCount; std::uint32_t elementStride; };
	[[nodiscard]] VirtualGeometryEncodedBlock EncodeVirtualGeometryMeshoptBlock(const VirtualGeometryMeshoptCodecEncodeRequest& request);
}
