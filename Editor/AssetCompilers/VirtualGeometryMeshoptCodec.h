// VirtualGeometryMeshoptCodec.h - meshoptimizer-backed compact block prototype.

#pragma once

#include "VirtualGeometryCodecs.h"

#include <span>

namespace TerraEngine::Editor
{
	enum class VirtualGeometryMeshoptStreamKind : std::uint32_t
	{
		TriangleIndices = 0u,
		QuantizedAttributes = 1u
	};

	struct VirtualGeometryMeshoptCodecEncodeRequest
	{
		std::span<const std::uint8_t> source;
		VirtualGeometryMeshoptStreamKind streamKind = VirtualGeometryMeshoptStreamKind::TriangleIndices;
		std::uint32_t elementCount = 0u;
		std::uint32_t elementStride = 0u;
	};

	// Encodes one topology or quantized-attribute stream using only vendored meshoptimizer.
	[[nodiscard]] VirtualGeometryEncodedBlock EncodeVirtualGeometryMeshoptBlock(const VirtualGeometryMeshoptCodecEncodeRequest &request);
} // namespace TerraEngine::Editor
