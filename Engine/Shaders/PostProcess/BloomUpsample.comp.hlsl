// Bloom pass: reconstruct one pyramid level without reading its output.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> lowBloom;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState lowSampler;
[[vk::binding(1, 0)]] Texture2D<float4> highBloom;
#if defined(BLOOM_OUTPUT_RGBA32F)
[[vk::binding(2, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> destinationBloom;
#else
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> destinationBloom;
#endif

struct BloomUpsampleConstants
{
	uint2 lowSize;
	uint2 highSize;
	float spread; // [0, 1]: finest-level bloom to the widest retained pyramid.
	float filterRadius; // Low-resolution texels; use 1 for the calibrated tent.
	uint flags; // Bit 0: execute. Disabled/out-of-range threads write nothing.
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<BloomUpsampleConstants> config;

float3 ReadLow(float2 uv)
{
	const float2 halfTexel = 0.5f / float2(config.lowSize);
	return lowBloom.SampleLevel(lowSampler, clamp(uv, halfTexel, 1.0f - halfTexel), 0.0f).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	if ((config.flags & 1u) == 0u || any(config.lowSize == 0u)
		|| any(dispatchId.xy >= config.highSize)) return;
	const float2 uv = (float2(dispatchId.xy) + 0.5f) / float2(config.highSize);
	const float2 radius = max(config.filterRadius, 0.0f) / float2(config.lowSize);
	// Separable [1, 2, 1] tent expanded to 3x3; normalized weights preserve DC.
	float3 filtered = ReadLow(uv) * 0.25f;
	filtered += (ReadLow(uv + float2(-1, 0) * radius) + ReadLow(uv + float2(1, 0) * radius)
		+ ReadLow(uv + float2(0, -1) * radius) + ReadLow(uv + float2(0, 1) * radius)) * 0.125f;
	filtered += (ReadLow(uv + float2(-1, -1) * radius) + ReadLow(uv + float2(1, -1) * radius)
		+ ReadLow(uv + float2(-1, 1) * radius) + ReadLow(uv + float2(1, 1) * radius)) * 0.0625f;
	const float3 high = highBloom.Load(uint3(dispatchId.xy, 0)).rgb;
	// A convex multiresolution blend keeps brightness independent of level count.
	// Spread changes the spatial distribution rather than multiplying intensity.
	destinationBloom[dispatchId.xy] = float4(lerp(high, filtered, saturate(config.spread)), 0.0f);
}
