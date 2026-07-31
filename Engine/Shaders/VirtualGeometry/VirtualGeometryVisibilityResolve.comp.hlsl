// Route-neutral resolve validates stable identity records before normalizing
// barycentrics and carrying their screen-space derivative payload forward.
// The compact four-word ABI intentionally remains usable by hardware, compute,
// and indexed fallback producers without a route-specific descriptor layout.
#include "VirtualGeometryAbi.hlsli"
struct VisibilityConstants
{
	uint recordCount;
	uint outputCapacity;
	uint generation;
	uint expectedGeneration;
	uint primitiveCount;
	uint reserved;
};
struct RasterTriangle
{
	float4 clip0;
	float4 clip1;
	float4 clip2;
	uint stablePrimitiveId;
	uint packedOwnerMaterial;
	uint sourceGeneration;
	uint flags;
};
[[vk::binding(0, 0)]] StructuredBuffer<uint4> inputRecords;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint4> outputRecords;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> diagnostics;
[[vk::binding(3, 0)]] StructuredBuffer<RasterTriangle> triangles;
[[vk::push_constant]] ConstantBuffer<VisibilityConstants> Resolve;

float2 UnpackUnorm2x16(uint packed)
{
	return float2(packed & 0xffffu, packed >> 16u) / 65535.0f;
}

float2 UnpackSnorm2x16(uint packed)
{
	const int2 quantized = int2(
		int(packed << 16u) >> 16,
		int(packed) >> 16);
	return clamp(float2(quantized) / 32767.0f, -1.0f, 1.0f);
}

uint PackUnorm2x16(float2 value)
{
	const uint2 quantized = uint2(round(saturate(value) * 65535.0f));
	return quantized.x | (quantized.y << 16u);
}

// The raster record stores barycentric X/Y. Z is reconstructed so round-off
// cannot turn a valid covered pixel into a negative-weight material sample.
bool ReconstructBarycentrics(uint packedBarycentrics, out float3 barycentrics)
{
	const float2 xy = UnpackUnorm2x16(packedBarycentrics);
	const float sum = xy.x + xy.y;
	if(!all(isfinite(xy)) || sum > 1.0001f) {
		barycentrics = float3(0.0f, 0.0f, 0.0f);
		return false;
	}
	barycentrics = float3(xy.x, xy.y, max(0.0f, 1.0f - sum));
	const float normalization = max(dot(barycentrics, float3(1.0f, 1.0f, 1.0f)), 1e-6f);
	barycentrics /= normalization;
	return true;
}

// Resolve reconstructs the canonical clip-space position and the producer's
// screen-space barycentric gradient before handing the compact record to
// shading. The owner/material word remains discrete and is never interpolated.
bool ReconstructAttributes(
	RasterTriangle sourceTriangle,
	float3 barycentrics,
	uint packedDerivative,
	out float4 clipPosition,
	out float2 barycentricDerivative)
{
	clipPosition = sourceTriangle.clip0 * barycentrics.x
		+ sourceTriangle.clip1 * barycentrics.y
		+ sourceTriangle.clip2 * barycentrics.z;
	barycentricDerivative = UnpackSnorm2x16(packedDerivative);
	return all(isfinite(clipPosition))
		&& all(isfinite(barycentricDerivative))
		&& abs(clipPosition.w) > 1e-6f;
}

[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= Resolve.recordCount) return;
	if(id.x >= Resolve.outputCapacity || Resolve.generation != Resolve.expectedGeneration) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_PACKET_OVERFLOW); return;
	}
	const uint4 inputRecord = inputRecords[id.x];
	// Word X is a stable primitive identity and word W is the producer
	// generation. Invalid/stale records deliberately produce no shaded result.
	if(inputRecord.x == TE_VIRTUAL_GEOMETRY_INVALID_INDEX
		|| inputRecord.x >= Resolve.primitiveCount
		|| inputRecord.w != Resolve.generation) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
		return;
	}
	float3 barycentrics;
	if(!ReconstructBarycentrics(inputRecord.y, barycentrics)) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
		return;
	}
	const RasterTriangle sourceTriangle = triangles[inputRecord.x];
	float4 clipPosition;
	float2 barycentricDerivative;
	if(!ReconstructAttributes(
		sourceTriangle,
		barycentrics,
		inputRecord.z,
		clipPosition,
		barycentricDerivative)) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_INVALID_PAYLOAD);
		return;
	}
	// Word Z preserves the validated dBary/dPixel estimate. Shading can apply
	// that gradient to UV/normal/tangent streams, while word W carries the
	// triangle's discrete owner/material identity without interpolation.
	outputRecords[id.x] = uint4(
		inputRecord.x,
		PackUnorm2x16(barycentrics.xy),
		inputRecord.z,
		sourceTriangle.packedOwnerMaterial);
}
