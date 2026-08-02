// Conservative triangle rasterization for the compute visibility route.
// Each lane owns one source triangle and visits its bounded screen rectangle,
// which keeps large triangles correct instead of truncating them to one tile.
#include "VirtualGeometryAbi.hlsli"
#include "VirtualGeometryRasterTriangle.hlsli"

struct RasterConstants
{
	uint triangleCount;
	uint pixelCount;
	uint generation;
	// 0 selects depth, 1 selects the stable owner, and 2 publishes attributes.
	// Vulkan inserts a storage barrier between these phases so companion fields
	// can never race ahead of the final depth/identity decision.
	uint phase;
	uint targetWidth;
	uint targetHeight;
};

struct RasterVertex
{
	float4 clip;
	float3 barycentrics;
};

[[vk::binding(0, 1)]] StructuredBuffer<RasterTriangle> triangles;
[[vk::binding(1, 1)]] RWStructuredBuffer<uint> depth;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint4> visibility;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> diagnostics;
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

// Vulkan homogeneous clip distances cover positive W, X/Y side planes, and
// zero-to-one Z. Keeping this work before division prevents behind-camera or
// far-plane vertices from projecting into detached screen-space fragments.
float FrustumClipDistance(float4 clip, uint plane)
{
	if(plane == 0u) return clip.w - 1.0e-6f;
	if(plane == 1u) return clip.x + clip.w;
	if(plane == 2u) return clip.w - clip.x;
	if(plane == 3u) return clip.y + clip.w;
	if(plane == 4u) return clip.w - clip.y;
	if(plane == 5u) return clip.z;
	return clip.w - clip.z;
}

// Sutherland-Hodgman clipping preserves source barycentrics at every generated
// frustum vertex, so resolve can still interpolate one canonical source record.
uint ClipAgainstFrustumPlane(
	inout RasterVertex polygon[12], uint polygonCount, uint plane)
{
	RasterVertex clipped[12];
	uint clippedCount = 0u;
	for(uint index = 0u; index < polygonCount; ++index) {
		const RasterVertex current = polygon[index];
		const RasterVertex previous = polygon[(index + polygonCount - 1u) % polygonCount];
		const float currentDistance = FrustumClipDistance(current.clip, plane);
		const float previousDistance = FrustumClipDistance(previous.clip, plane);
		const bool currentInside = currentDistance >= 0.0f;
		const bool previousInside = previousDistance >= 0.0f;
		if(currentInside != previousInside) {
			const float denominator = previousDistance - currentDistance;
			const float t = abs(denominator) > 1e-8f
				? saturate(previousDistance / denominator) : 0.0f;
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
	// Terra's projection matrices already contain the Vulkan Y flip. The active
	// graphics viewport has positive height, so use Vulkan's direct NDC mapping
	// here instead of applying a second image-space inversion.
	float2 screen0 = (vertex0.clip.xy / vertex0.clip.w * 0.5f + 0.5f) * float2(width, height);
	float2 screen1 = (vertex1.clip.xy / vertex1.clip.w * 0.5f + 0.5f) * float2(width, height);
	float2 screen2 = (vertex2.clip.xy / vertex2.clip.w * 0.5f + 0.5f) * float2(width, height);
	float signedArea = EdgeFunction(screen0, screen1, screen2);
	// EdgeFunction has the opposite sign from the conventional cross product, so
	// Vulkan's CCW front faces arrive negative. Double-sided default materials
	// accept either winding; single-sided packets retain hardware culling parity.
	const bool doubleSided = (source.identity.w
		& TE_VIRTUAL_GEOMETRY_RASTER_FLAG_DOUBLE_SIDED) != 0u;
	if(!doubleSided && signedArea >= -1e-8f) return;
	if(abs(signedArea) <= 1e-8f) return;
	if(signedArea < 0.0f)
	{
		const RasterVertex swapVertex = vertex1; vertex1 = vertex2; vertex2 = swapVertex;
		const float2 swapScreen = screen1; screen1 = screen2; screen2 = swapScreen;
		signedArea = -signedArea;
	}
	const float2 rawMinimum = min(screen0, min(screen1, screen2));
	const float2 rawMaximum = max(screen0, max(screen1, screen2));
	if(rawMaximum.x < 0.0f || rawMaximum.y < 0.0f
		|| rawMinimum.x >= float(width) || rawMinimum.y >= float(height)) return;
	const float2 minScreen = floor(clamp(
		rawMinimum, float2(0.0f, 0.0f), float2(width - 1u, height - 1u)));
	const float2 maxScreen = ceil(clamp(
		rawMaximum, float2(0.0f, 0.0f), float2(width - 1u, height - 1u)));
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
			if(Raster.phase == 0u) {
				uint previousDepth;
				InterlockedMin(depth[pixel], depthKey, previousDepth);
			}
			else if(Raster.phase == 1u) {
				if(depthKey != depth[pixel]) continue;
				uint previousPrimitive;
				InterlockedMin(
					visibility[pixel].x,
					source.identity.x,
					previousPrimitive);
			}
			else {
				if(depthKey != depth[pixel]
					|| visibility[pixel].x != source.identity.x) continue;
				// Only the final depth and stable-identity winner reaches this phase,
				// so one lane owns every non-atomic companion field.
				visibility[pixel].y = PackUnorm2x16(sourceBarycentrics.xy);
				const float2 barycentricDerivative = float2(
					(screen1.y - screen2.y) * invArea,
					(screen2.x - screen1.x) * invArea);
				visibility[pixel].z = PackSnorm2x16(barycentricDerivative);
				visibility[pixel].w = source.identity.z;
			}
		}
	}
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if(id.x >= Raster.triangleCount || Raster.pixelCount == 0u) return;
	const RasterTriangle source = triangles[id.x];
	if(source.identity.x == TE_VIRTUAL_GEOMETRY_INVALID_INDEX || source.identity.z != Raster.generation) {
		InterlockedOr(diagnostics[0], TE_VIRTUAL_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
		return;
	}
	const uint width = Raster.targetWidth;
	const uint height = Raster.targetHeight;
	if(width == 0u || height == 0u
		|| Raster.pixelCount / width != height
		|| Raster.pixelCount % width != 0u) return;
	RasterVertex polygon[12];
	polygon[0].clip = source.clip0;
	polygon[0].barycentrics = float3(1.0f, 0.0f, 0.0f);
	polygon[1].clip = source.clip1;
	polygon[1].barycentrics = float3(0.0f, 1.0f, 0.0f);
	polygon[2].clip = source.clip2;
	polygon[2].barycentrics = float3(0.0f, 0.0f, 1.0f);
	uint polygonCount = 3u;
	for(uint plane = 0u; plane < 7u && polygonCount >= 3u; ++plane) {
		polygonCount = ClipAgainstFrustumPlane(polygon, polygonCount, plane);
	}
	if(polygonCount < 3u) return;
	for(uint triangleIndex = 1u; triangleIndex + 1u < polygonCount; ++triangleIndex) {
		RasterizeTriangle(polygon[0], polygon[triangleIndex], polygon[triangleIndex + 1u], source, width, height);
	}
}
