// Transient visibility V2 validates direct perspective weights and preserves
// canonical raster depth. Legacy derivative-bearing records are not compatible.
#include "VirtualGeometryAbi.hlsli"
#include "VirtualGeometryRasterTriangle.hlsli"
#include "VirtualGeometryReconstruction.hlsli"
struct VisibilityConstants
{
	uint recordCount;
	uint outputCapacity;
	uint generation;
	uint expectedGeneration;
	uint primitiveCount;
	uint reserved;
};
[[vk::binding(0, 1)]] StructuredBuffer<uint4> inputRecords;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint4> outputRecords;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint> diagnostics;
[[vk::binding(3, 1)]] StructuredBuffer<RasterTriangle> triangles;
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> Resolve;

uint PackUnorm2x16(float2 value)
{
	const uint2 quantized = uint2(round(saturate(value) * 65535.0f));
	return quantized.x | (quantized.y << 16u);
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= Resolve.recordCount) return;
	if (id.x >= Resolve.outputCapacity) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW); return;
	}
	// A rejected producer cannot leave a previous record visible in this slot.
	outputRecords[id.x] = uint4(TE_VIRTUAL_GEOMETRY_INVALID_INDEX, 0u, 0u, 0u);
	if (Resolve.generation != Resolve.expectedGeneration) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION); return;
	}
	const uint4 inputRecord = inputRecords[id.x];
	if (inputRecord.x == TE_VIRTUAL_GEOMETRY_INVALID_INDEX) return;
	// XOR is bijective over the full generation, avoiding either truncation or
	// an ambiguous legacy interpretation when producer/consumer modules differ.
	if (inputRecord.x >= Resolve.primitiveCount
		|| inputRecord.w != (Resolve.generation ^ TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_TOKEN)) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION); return;
	}
	float3 perspectiveWeights;
	if (!DecodeVirtualGeometryPerspectiveWeights(inputRecord.y, perspectiveWeights)) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD); return;
	}
	const RasterTriangle sourceTriangle = triangles[inputRecord.x];
	if (sourceTriangle.identity.z != Resolve.generation) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION); return;
	}
	// Material bin and route occupy the low two bytes. The upper halfword tags
	// V2 for the compositor; Z is the actual raster depth, never a derivative.
	outputRecords[id.x] = uint4(inputRecord.x, PackUnorm2x16(perspectiveWeights.xy), inputRecord.z,
		(sourceTriangle.identity.y & 0xffffu) | TE_VIRTUAL_GEOMETRY_VISIBILITY_ENCODING_TOKEN);
}
