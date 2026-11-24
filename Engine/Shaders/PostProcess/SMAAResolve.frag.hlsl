#include "SMAACommon.hlsli"
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> sourceColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState sourceSampler;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D<float4> weightTexture;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState weightSampler;
float4 Weight(int2 p) { return Inside(p) ? weightTexture.SampleLevel(weightSampler, PixelUv(p), 0) : 0.0f.xxxx; }
float4 Color(int2 p) { return sourceColor.SampleLevel(sourceSampler, PixelUv(ClampPixel(p)), 0); }
float4 PSMain(float4 position : SV_Position) : SV_Target0
{
	const int2 p = int2(position.xy);
	const float4 original = Color(p), centerWeights = Weight(p);
	float4 w = float4(centerWeights.x, Weight(p + int2(1, 0)).y,
		centerWeights.z, Weight(p + int2(0, 1)).w);
	// North/west reconstructions of a diagonal can describe the same pixel
	// triangle. Adding both counts its coverage twice. Select one dominant
	// axis, including for diagonals, while retaining both sides of that axis.
	if(max(w.x, w.y) >= max(w.z, w.w)) w.zw = 0.0f.xx;
	else w.xy = 0.0f.xx;
	const float total = dot(w, 1.0f.xxxx);
	if(total <= 1.0e-7f) return original;
	w /= max(1.0f, total);
	float3 filtered = (1.0f - dot(w, 1.0f.xxxx)) * original.rgb;
	filtered += w.x * Color(p + int2(-1, 0)).rgb + w.y * Color(p + int2(1, 0)).rgb;
	filtered += w.z * Color(p + int2(0, -1)).rgb + w.w * Color(p + int2(0, 1)).rgb;
	return float4(filtered, original.a); // Alpha is never an edge input or blend target.
}
