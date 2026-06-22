#pragma pack_matrix(column_major)

// Bilateral blur for the contact-darkening target. It uses depth agreement to
// keep occlusion from bleeding across object edges.
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
// Raw AO uses its own sampled binding so material shading can keep binding 19
// pointed at the final blurred AO image for the whole draw pass.
[[vk::binding(22, 0)]] Texture2D<float> inputOcclusion;
// Blur output uses a dedicated storage binding instead of the Hi-Z output
// binding. This keeps all compute passes stable under optimized recording.
[[vk::binding(24, 0)]] RWTexture2D<float> outputOcclusion;
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

bool IsUsableDepth(float depth)
{
	return isfinite(depth) && depth > 0.0f && depth < 0.999999f;
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
	if (!IsUsableDepth(centerDepth))
	{
		outputOcclusion[pixel] = 1.0f;
		return;
	}

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
			if (!IsUsableDepth(sampleDepth))
			{
				continue;
			}
			const float depthWeight = exp(-abs(sampleDepth - centerDepth) / max(config.depthSigma, 0.0001f));
			const float spatialWeight = exp(-dot(float2(x, y), float2(x, y)) * 0.25f);
			const float weight = depthWeight * spatialWeight;
			const float sampleOcclusion = LoadOcclusionClamped(samplePixel);
			weightedOcclusion += (isfinite(sampleOcclusion) ? saturate(sampleOcclusion) : 1.0f) * weight;
			totalWeight += weight;
		}
	}

	const float fallbackOcclusion = LoadOcclusionClamped(int2(pixel));
	const float blurred = totalWeight > 0.0f
		? weightedOcclusion / totalWeight
		: (isfinite(fallbackOcclusion) ? saturate(fallbackOcclusion) : 1.0f);
	// Blend only after the spatial blur so history smooths residual AO noise.
	// Avoid sampling history while the CPU says it is invalid; optimized builds
	// may still propagate undefined texture contents through a zero-weight lerp.
	const float historyBlend = saturate(config.historyBlend);
	float resolvedOcclusion = blurred;
	if (historyBlend > 0.001f)
	{
		const float history = LoadHistoryClamped(int2(pixel));
		resolvedOcclusion = lerp(blurred, isfinite(history) ? saturate(history) : 1.0f, historyBlend);
	}
	outputOcclusion[pixel] = saturate(resolvedOcclusion);
}
