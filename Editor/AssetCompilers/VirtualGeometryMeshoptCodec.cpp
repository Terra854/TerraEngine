// VirtualGeometryMeshoptCodec.cpp - meshoptimizer-backed prototype encoding.

#include "VirtualGeometryMeshoptCodec.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace TerraEngine::Editor
{
	VirtualGeometryEncodedBlock EncodeVirtualGeometryMeshoptBlock(const VirtualGeometryMeshoptCodecEncodeRequest &request)
	{
		VirtualGeometryEncodedBlock result;
		if (request.source.empty() || request.elementCount == 0u || request.elementStride == 0u)
		{
			result.error = "meshoptimizer prototype input needs bytes and non-zero element metadata.";
			return result;
		}
		if (request.elementCount > std::numeric_limits<std::size_t>::max() / request.elementStride ||
			static_cast<std::size_t>(request.elementCount) * request.elementStride != request.source.size())
		{
			result.error = "meshoptimizer prototype input byte count does not match its element metadata.";
			return result;
		}

		std::vector<std::uint8_t> payload;
		std::uint32_t flags = 0u;
		if (request.streamKind == VirtualGeometryMeshoptStreamKind::TriangleIndices)
		{
			if (request.elementStride != sizeof(std::uint32_t) || request.elementCount % 3u != 0u)
			{
				result.error = "meshoptimizer triangle streams require uint32 triangle-list indices.";
				return result;
			}
			// The public request is byte-oriented to share the common block API.
			// Copy into aligned words before calling meshoptimizer so a page slice
			// with an arbitrary byte offset cannot cause an unaligned uint32 read.
			std::vector<std::uint32_t> indices(request.elementCount);
			std::memcpy(indices.data(), request.source.data(), request.source.size());
			std::uint32_t vertexCount = 0u;
			for (std::uint32_t index = 0u; index < request.elementCount; ++index)
			{
				if (indices[index] == std::numeric_limits<std::uint32_t>::max())
				{
					result.error = "meshoptimizer triangle stream contains an unrepresentable vertex index.";
					return result;
				}
				vertexCount = std::max(vertexCount, indices[index] + 1u);
			}
			payload.resize(meshopt_encodeIndexBufferBound(request.elementCount, vertexCount));
			const std::size_t written = meshopt_encodeIndexBuffer(payload.data(), payload.size(), indices.data(), request.elementCount);
			if (written == 0u)
			{
				result.error = "meshoptimizer failed to encode the triangle index stream.";
				return result;
			}
			payload.resize(written);
			flags = kVirtualGeometryCompactBlockFlagMeshoptIndexStream;
		}
		else
		{
			payload.resize(meshopt_encodeVertexBufferBound(request.elementCount, request.elementStride));
			const std::size_t written =
				meshopt_encodeVertexBuffer(payload.data(), payload.size(), request.source.data(), request.elementCount, request.elementStride);
			if (written == 0u)
			{
				result.error = "meshoptimizer failed to encode the quantized attribute stream.";
				return result;
			}
			payload.resize(written);
			flags = kVirtualGeometryCompactBlockFlagMeshoptVertexStream;
		}
		if (payload.size() > std::numeric_limits<std::uint32_t>::max())
		{
			result.error = "meshoptimizer encoded payload exceeds the v1 block byte range.";
			return result;
		}

		const VirtualGeometryCompactBlockHeader header{ kVirtualGeometryCompactBlockMagicValue,
			kVirtualGeometryCompactBlockVersion,
			static_cast<std::uint16_t>(sizeof(VirtualGeometryCompactBlockHeader)),
			static_cast<std::uint32_t>(VirtualGeometryCompactCodec::Meshoptimizer),
			flags,
			static_cast<std::uint32_t>(request.source.size()),
			static_cast<std::uint32_t>(payload.size()),
			request.elementCount,
			request.elementStride,
			kVirtualGeometryCodecCapabilityMeshoptimizer,
			CalculateVirtualGeometryCompactHash(request.source) };
		result.bytes.resize(sizeof(header) + payload.size());
		std::memcpy(result.bytes.data(), &header, sizeof(header));
		std::memcpy(result.bytes.data() + sizeof(header), payload.data(), payload.size());
		return result;
	}
} // namespace TerraEngine::Editor
