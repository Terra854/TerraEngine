#pragma pack_matrix(column_major)

// Bilateral blur for the contact-darkening target. It uses depth agreement to
// keep occlusion from bleeding across object edges.
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
[[vk::binding(19, 0)]] Texture2D<float> inputOcclusion;
[[vk::binding(16, 0)]] RWTexture2D<float> outputOcclusion;
[[vk::binding(21, 0)]] Texture2D<float> inputHistory;

struct PushConstants
{
	uint2 viewportSize;
	float depthSigma;
	float historyBlend;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

float LoadDepthClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputDepth.Load(uint3(uint2(clampedPixel), 0));
}

float LoadOcclusionClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputOcclusion.Load(uint3(uint2(clampedPixel), 0));
}

float LoadHistoryClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputHistory.Load(uint3(uint2(clampedPixel), 0));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint2 pixel = dispatchThreadId.xy;
	if (any(pixel >= config.viewportSize))
	{
		return;
	}

	const float centerDepth = LoadDepthClamped(int2(pixel));
	float weightedOcclusion = 0.0f;
	float totalWeight = 0.0f;

	[unroll]
	for (int y = -2; y <= 2; ++y)
	{
		[unroll]
		for (int x = -2; x <= 2; ++x)
		{
			const int2 samplePixel = int2(pixel) + int2(x, y);
			const float sampleDepth = LoadDepthClamped(samplePixel);
			const float depthWeight = exp(-abs(sampleDepth - centerDepth) / max(config.depthSigma, 0.0001f));
			const float spatialWeight = exp(-dot(float2(x, y), float2(x, y)) * 0.25f);
			const float weight = depthWeight * spatialWeight;
			weightedOcclusion += LoadOcclusionClamped(samplePixel) * weight;
			totalWeight += weight;
		}
	}

	const float blurred = totalWeight > 0.0f ? weightedOcclusion / totalWeight : LoadOcclusionClamped(int2(pixel));
	// Blend only after the spatial blur so history smooths residual AO noise
	// without bleeding raw high-frequency samples across object silhouettes.
	const float history = LoadHistoryClamped(int2(pixel));
	outputOcclusion[pixel] = saturate(lerp(blurred, history, saturate(config.historyBlend)));
}
