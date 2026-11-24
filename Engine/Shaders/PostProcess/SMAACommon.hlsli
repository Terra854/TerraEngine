#ifndef TERRAENGINE_SMAA_COMMON_HLSLI
#define TERRAENGINE_SMAA_COMMON_HLSLI
#include "SpatialAALuminance.hlsli"

// Independent analytic SMAA 1x reconstruction; no imported implementation or lookup bytes.
// Source: Jimenez et al., Eurographics 2012, sections 4.1 and 4.2.
struct SmaaConstants
{
	uint2 extent;
	uint maxSearchSteps;
	uint maxDiagonalSteps;
	float threshold;
	float localContrastFactor;
	float cornerRounding;
	uint flags;
};
[[vk::push_constant]] ConstantBuffer<SmaaConstants> smaa;

bool Inside(int2 p) { return all(p >= 0) && all(p < int2(smaa.extent)); }
int2 ClampPixel(int2 p) { return clamp(p, int2(0, 0), int2(smaa.extent) - 1); }
float2 PixelUv(int2 p) { return (float2(p) + 0.5f) / float2(smaa.extent); }
// Integrate max(y,0) exactly along a unit interval with a linear y(t).
// Clipping a reconstructed line against either neighboring pixel is therefore
// geometric coverage, not a distance-derived blur factor or a copied LUT.
float PositiveArea(float a, float b)
{
	if(min(a, b) >= 0.0f) return 0.5f * (a + b);
	if(max(a, b) <= 0.0f) return 0.0f;
	const float positive = max(a, b);
	return 0.5f * positive * positive / abs(b - a);
}
float2 PixelCrossingAreas(float a, float b)
{
	return float2(PositiveArea(a, b) - PositiveArea(a - 1.0f, b - 1.0f),
		PositiveArea(-a, -b) - PositiveArea(-a - 1.0f, -b - 1.0f));
}

#endif
