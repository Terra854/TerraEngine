// VirtualGeometryMeshoptCodec.cpp - production evaluator profile implementation.
#include "VirtualGeometryMeshoptCodec.h"
#include "meshoptimizer.h"

#include <algorithm>
namespace TerraEngine::Editor
{
	VirtualGeometryEncodedBlock EncodeVirtualGeometryMeshoptBlock(const VirtualGeometryMeshoptCodecEncodeRequest& request)
	{
		VirtualGeometryEncodedBlock result;
		if (request.bytes.empty() || request.elementCount == 0u || request.elementStride == 0u
			|| static_cast<std::uint64_t>(request.elementCount) * request.elementStride != request.bytes.size())
		{
			result.error = "meshoptimizer evaluation requires exact element metadata.";
			return result;
		}
		if (request.kind == VirtualGeometryMeshoptStreamKind::TriangleIndices)
		{
			if (request.elementStride != sizeof(std::uint32_t) || request.elementCount % 3u != 0u)
			{
				result.error = "meshoptimizer triangle evaluation requires uint32 triangle-list indices.";
				return result;
			}
			const auto* indices = reinterpret_cast<const std::uint32_t*>(request.bytes.data());
			std::uint32_t maximum = 0u;
			for (std::uint32_t index = 0u; index < request.elementCount; ++index) maximum = std::max(maximum, indices[index]);
			result.bytes.resize(meshopt_encodeIndexBufferBound(request.elementCount, maximum + 1u));
			result.bytes.resize(meshopt_encodeIndexBuffer(result.bytes.data(), result.bytes.size(), indices, request.elementCount));
		}
		else
		{
			result.bytes.resize(meshopt_encodeVertexBufferBound(request.elementCount, request.elementStride));
			result.bytes.resize(meshopt_encodeVertexBuffer(result.bytes.data(), result.bytes.size(), request.bytes.data(), request.elementCount, request.elementStride));
		}
		if (result.bytes.empty()) { result.error = "meshoptimizer production encoding failed."; return result; }
		result.descriptor = { 1u, kTemfDirectResidentPageId, 0u, static_cast<std::uint16_t>(TemfStreamKind::Metadata), 0u,
			static_cast<std::uint32_t>(TemfCodecProfile::MeshoptimizerProductionStatic), 1u, 0u, 0u, 0u,
			result.bytes.size(), request.bytes.size(), request.elementCount, request.elementStride, 1u, 0u, 0u, 0u,
			kVirtualGeometryCodecCapabilityMeshoptimizer, CalculateVirtualGeometryCompactHash(result.bytes), CalculateVirtualGeometryCompactHash(request.bytes) };
		return result;
	}
}
