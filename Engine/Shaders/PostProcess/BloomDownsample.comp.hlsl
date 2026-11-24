// Bloom pass: sample linear HDR into a separate half-resolution image.
// The first level applies a luminance threshold; subsequent levels only filter.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> sourceColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState sourceSampler;
#if defined(BLOOM_OUTPUT_RGBA32F)
[[vk::binding(1, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> destinationBloom;
#else
[[vk::binding(1, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> destinationBloom;
#endif

struct BloomDownsampleConstants
{
	uint2 sourceSize;
	uint2 destinationSize;
	float threshold; // Linear Rec.709 luminance in the incoming exposure domain.
	float softKnee; // Fraction of threshold in [0, 1]; zero is a hard threshold.
	uint flags; // Bit 0: execute; bit 1: apply threshold on this first level.
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<BloomDownsampleConstants> config;

float3 ReadBloom(float2 uv)
{
	const float2 halfTexel = 0.5f / float2(config.sourceSize);
	// Explicit edge clamping also covers odd extents and one-texel pyramid tails.
	float3 color = max(sourceColor.SampleLevel(sourceSampler, clamp(uv, halfTexel, 1.0f - halfTexel), 0.0f).rgb, 0.0f);
	if ((config.flags & 2u) == 0u) return color;
	const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
	const float threshold = max(config.threshold, 0.0f);
	const float knee = threshold * saturate(config.softKnee);
	float softContribution = 0.0f;
	if (knee > 0.0f) {
		const float soft = clamp(luminance - threshold + knee, 0.0f, 2.0f * knee);
		softContribution = soft * soft / (4.0f * knee);
	}
	const float contribution = max(luminance - threshold, softContribution);
	return color * (max(contribution, 0.0f) / max(luminance, 1.0e-6f));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	if ((config.flags & 1u) == 0u || any(config.sourceSize == 0u)
		|| any(dispatchId.xy >= config.destinationSize)) return;
	const float2 texel = 1.0f / float2(config.sourceSize);
	const float2 uv = (float2(dispatchId.xy) + 0.5f) / float2(config.destinationSize);
	// Normalized 13-tap low-pass reduction: weights sum to exactly one. Applying
	// extraction per tap avoids discarding a small bright source after averaging.
	float3 bloom = ReadBloom(uv) * 0.125f;
	bloom += (ReadBloom(uv + float2(-2, -2) * texel) + ReadBloom(uv + float2(2, -2) * texel)
		+ ReadBloom(uv + float2(-2, 2) * texel) + ReadBloom(uv + float2(2, 2) * texel)) * 0.03125f;
	bloom += (ReadBloom(uv + float2(0, -2) * texel) + ReadBloom(uv + float2(-2, 0) * texel)
		+ ReadBloom(uv + float2(2, 0) * texel) + ReadBloom(uv + float2(0, 2) * texel)) * 0.0625f;
	bloom += (ReadBloom(uv + float2(-1, -1) * texel) + ReadBloom(uv + float2(1, -1) * texel)
		+ ReadBloom(uv + float2(-1, 1) * texel) + ReadBloom(uv + float2(1, 1) * texel)) * 0.125f;
	// Bloom carries radiance only. Composition preserves the original scene alpha.
	destinationBloom[dispatchId.xy] = float4(bloom, 0.0f);
}
