// Conservative triangle rasterization for the compute visibility route.
// Each lane owns one source triangle and visits its bounded screen rectangle,
// which keeps large triangles correct instead of truncating them to one tile.
#include "VirtualGeometryAbi.hlsli"

struct RasterConstants
{
	uint triangleCount;
	uint pixelCount;
	uint generation;
	uint failOpen;
	uint targetWidth;
	uint targetHeight;
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

struct RasterVertex
{
	float4 clip;
	float3 barycentrics;
};

[[vk::binding(0, 0)]] StructuredBuffer<RasterTriangle> triangles;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> depth;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint4> visibility;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> diagnostics;
[[vk::push_constant]] ConstantBuffer<RasterConstants> Raster;

float EdgeFunction(float2 a, float2 b, float2 p)
{
	return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

bool IsTopLeftEdge(float2 a, float2 b)
{
	const float2 edge = b - a;
	return edge.y < 0.0f || (edge.y == 0.0f && edge.x > 0.0f);
}

uint PackUnorm2x16(float2 value)
{
	const uint2 quantized = uint2(round(saturate(value) * 65535.0f));
	return quantized.x | (quantized.y << 16u);
}

uint PackSnorm2x16(float2 value)
{
	const int2 quantized = int2(round(clamp(value, -1.0f, 1.0f) * 32767.0f));
	return (uint(quantized.x) & 0xffffu) | (uint(quantized.y) << 16u);
}

// Sutherland-Hodgman clipping preserves source barycentrics at generated
// near-plane vertices, so resolve can still interpolate one canonical record.
uint ClipAgainstNearPlane(inout RasterVertex polygon[6], uint polygonCount)
{
	RasterVertex clipped[6];
	uint clippedCount = 0u;
	for(uint index = 0u; index < polygonCount; ++index) {
		const RasterVertex current = polygon[index];
		const RasterVertex previous = polygon[(index + polygonCount - 1u) % polygonCount];
		const bool currentInside = current.clip.w > 1e-6f && current.clip.z >= 0.0f;
		const bool previousInside = previous.clip.w > 1e-6f && previous.clip.z >= 0.0f;
		if(currentInside != previousInside) {
			const float previousDistance = min(previous.clip.z, previous.clip.w - 1e-6f);
			const float currentDistance = min(current.clip.z, current.clip.w - 1e-6f);
			const float denominator = previousDistance - currentDistance;
			const float t = abs(denominator) > 1e-6f ? saturate(previousDistance / denominator) : 0.0f;
			clipped[clippedCount].clip = lerp(previous.clip, current.clip, t);
			clipped[clippedCount].barycentrics = lerp(previous.barycentrics, current.barycentrics, t);
			++clippedCount;
		}
		if(currentInside) {
			clipped[clippedCount] = current;
			++clippedCount;
		}
	}
	for(uint index = 0u; index < clippedCount; ++index) polygon[index] = clipped[index];
	return clippedCount;
}

void RasterizeTriangle(RasterVertex vertex0, RasterVertex vertex1, RasterVertex vertex2,
	RasterTriangle source, uint width, uint height)
{
	float2 screen0 = (vertex0.clip.xy / vertex0.clip.w * float2(0.5f, -0.5f) + 0.5f) * float2(width, height);
	float2 screen1 = (vertex1.clip.xy / vertex1.clip.w * float2(0.5f, -0.5f) + 0.5f) * float2(width, height);
	float2 screen2 = (vertex2.clip.xy / vertex2.clip.w * float2(0.5f, -0.5f) + 0.5f) * float2(width, height);
	float signedArea = EdgeFunction(screen0, screen1, screen2);
	if(abs(signedArea) <= 1e-8f) return;
	if(signedArea < 0.0f) {
		const RasterVertex swapVertex = vertex1; vertex1 = vertex2; vertex2 = swapVertex;
		const float2 swapScreen = screen1; screen1 = screen2; screen2 = swapScreen;
		signedArea = -signedArea;
	}
	const float2 minScreen = floor(max(min(screen0, min(screen1, screen2)), float2(0.0f, 0.0f)));
	const float2 maxScreen = ceil(min(max(screen0, max(screen1, screen2)), float2(width - 1u, height - 1u)));
	const uint2 minimumPixel = uint2(minScreen);
	const uint2 maximumPixel = uint2(maxScreen);
	const bool edge01TopLeft = IsTopLeftEdge(screen0, screen1);
	const bool edge12TopLeft = IsTopLeftEdge(screen1, screen2);
	const bool edge20TopLeft = IsTopLeftEdge(screen2, screen0);
	const float invArea = rcp(signedArea);
	for(uint y = minimumPixel.y; y <= maximumPixel.y; ++y) {
		for(uint x = minimumPixel.x; x <= maximumPixel.x; ++x) {
			const float2 samplePosition = float2(x, y) + 0.5f;
			const float edge01 = EdgeFunction(screen0, screen1, samplePosition);
			const float edge12 = EdgeFunction(screen1, screen2, samplePosition);
			const float edge20 = EdgeFunction(screen2, screen0, samplePosition);
			if(edge01 < 0.0f || edge12 < 0.0f || edge20 < 0.0f
				|| (edge01 == 0.0f && !edge01TopLeft)
				|| (edge12 == 0.0f && !edge12TopLeft)
				|| (edge20 == 0.0f && !edge20TopLeft)) continue;
			const uint pixel = y * width + x;
			if(pixel >= Raster.pixelCount) continue;
			const float3 screenBarycentrics = float3(edge12, edge20, edge01) * invArea;
			const float3 sourceBarycentrics = vertex0.barycentrics * screenBarycentrics.x
				+ vertex1.barycentrics * screenBarycentrics.y + vertex2.barycentrics * screenBarycentrics.z;
			const float depthNdc = vertex0.clip.z / vertex0.clip.w * screenBarycentrics.x
				+ vertex1.clip.z / vertex1.clip.w * screenBarycentrics.y + vertex2.clip.z / vertex2.clip.w * screenBarycentrics.z;
			const uint depthKey = uint(round(saturate(depthNdc) * 4294967295.0f));
			uint previousDepth;
			InterlockedMin(depth[pixel], depthKey, previousDepth);
			if(depthKey < previousDepth) {
				// A strictly nearer depth owns every companion field regardless of
				// the CPU clear value used by this frame's visibility record buffer.
				uint ignoredPrimitive;
				InterlockedExchange(visibility[pixel].x, source.stablePrimitiveId, ignoredPrimitive);
				visibility[pixel].y = PackUnorm2x16(sourceBarycentrics.xy);
				const float2 barycentricDerivative = float2(
					(screen1.y - screen2.y) * invArea,
					(screen2.x - screen1.x) * invArea);
				visibility[pixel].z = PackSnorm2x16(barycentricDerivative);
				visibility[pixel].w = source.sourceGeneration;
			}
			else if(depthKey == previousDepth) {
				uint previousPrimitive;
				InterlockedMin(visibility[pixel].x, source.stablePrimitiveId, previousPrimitive);
				if(source.stablePrimitiveId <= previousPrimitive || visibility[pixel].w != source.sourceGeneration) {
					// The winning primitive owns barycentrics and a constant derivative
					// estimate; a later equal-depth lower identity overwrites all fields.
					visibility[pixel].y = PackUnorm2x16(sourceBarycentrics.xy);
					const float2 barycentricDerivative = float2(
						(screen1.y - screen2.y) * invArea,
						(screen2.x - screen1.x) * invArea);
					visibility[pixel].z = PackSnorm2x16(barycentricDerivative);
					visibility[pixel].w = source.sourceGeneration;
				}
			}
		}
	}
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= Raster.triangleCount || Raster.pixelCount == 0u) return;
	const RasterTriangle source = triangles[id.x];
	if(source.stablePrimitiveId == TE_VIRTUAL_GEOMETRY_INVALID_INDEX || source.sourceGeneration != Raster.generation) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
		return;
	}
	const uint width = Raster.targetWidth;
	const uint height = Raster.targetHeight;
	if(width == 0u || height == 0u
		|| Raster.pixelCount / width != height
		|| Raster.pixelCount % width != 0u) return;
	RasterVertex polygon[6];
	polygon[0].clip = source.clip0;
	polygon[0].barycentrics = float3(1.0f, 0.0f, 0.0f);
	polygon[1].clip = source.clip1;
	polygon[1].barycentrics = float3(0.0f, 1.0f, 0.0f);
	polygon[2].clip = source.clip2;
	polygon[2].barycentrics = float3(0.0f, 0.0f, 1.0f);
	const uint polygonCount = ClipAgainstNearPlane(polygon, 3u);
	if(polygonCount < 3u) return;
	for(uint triangleIndex = 1u; triangleIndex + 1u < polygonCount; ++triangleIndex) {
		RasterizeTriangle(polygon[0], polygon[triangleIndex], polygon[triangleIndex + 1u], source, width, height);
	}
}
