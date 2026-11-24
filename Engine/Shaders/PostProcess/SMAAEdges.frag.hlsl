#include "SMAACommon.hlsli"
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> sourceColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState sourceSampler;
float Luma(int2 p) { return SpatialPerceptualLuminance(sourceColor.SampleLevel(sourceSampler, PixelUv(ClampPixel(p)), 0).rgb); }
float2 PSMain(float4 position : SV_Position) : SV_Target0
{
	const int2 p = int2(position.xy);
	const float center = Luma(p);
	const float left = Luma(p + int2(-1, 0)), north = Luma(p + int2(0, -1));
	float2 delta = abs(center - float2(left, north));
	float2 edges = step(smaa.threshold.xx, delta);
	if(!any(edges)) return 0.0f.xx; // Write zero: render-pass contents need no stencil/discard contract.
	const float rightDelta = abs(center - Luma(p + int2(1, 0)));
	const float downDelta = abs(center - Luma(p + int2(0, 1)));
	const float leftSecond = abs(left - Luma(p + int2(-2, 0)));
	const float northSecond = abs(north - Luma(p + int2(0, -2)));
	const float common = max(max(delta.x, delta.y), max(rightDelta, downDelta));
	// Separate second-neighbor tests preserve the paper's perceptual masking
	// relation for each primary boundary; no edge is fabricated at a screen border.
	edges *= step(float2(max(common, leftSecond), max(common, northSecond)), smaa.localContrastFactor * delta);
	return edges;
}
