// VirtualGeometryCodecs.cpp - production-only evaluator implementation.
#include "VirtualGeometryCodecs.h"

namespace TerraEngine::Editor
{
	VirtualGeometryEncodedBlock EncodeVirtualGeometryPrototypeBlock(VirtualGeometryPrototypeCodec codec, const VirtualGeometryCodecEncodeRequest& request)
	{
		VirtualGeometryEncodedBlock result;
		if(request.bytes.empty() || request.elementCount == 0u || request.elementStride == 0u
			|| static_cast<std::uint64_t>(request.elementCount) * request.elementStride != request.bytes.size())
		{
			result.error = "Production profile evaluation requires exact element metadata.";
			return result;
		}
		if (codec == VirtualGeometryPrototypeCodec::Terra)
		{
			result.error = "The Terra prototype is offline-only and is not a production evaluator profile.";
			return result;
		}
		result.bytes.assign(request.bytes.begin(), request.bytes.end());
		result.descriptor = { 1u, kTemfDirectResidentPageId, 0u,
			static_cast<std::uint16_t>(TemfStreamKind::Metadata), 0u,
			static_cast<std::uint32_t>(TemfCodecProfile::ExpandedProduction), 1u, 0u,
			kTemfBlockFlagDirectResident, 0u, result.bytes.size(), result.bytes.size(),
			request.elementCount, request.elementStride, 1u, 0u, 0u, 0u, 0u,
			CalculateVirtualGeometryCompactHash(result.bytes), CalculateVirtualGeometryCompactHash(result.bytes) };
		return result;
	}
}
