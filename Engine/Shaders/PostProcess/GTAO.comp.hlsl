#pragma pack_matrix(column_major)

// First-pass contact darkening shader. The Vulkan integration can bind a
// full-resolution depth texture and write one R8/R16 occlusion target; until
// that pass is wired, this file documents and compiles the expected GPU side.
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
// Dedicated AO storage binding. Binding 16 is reserved for Hi-Z output, and
// reusing it for AO made optimized command buffers depend on descriptor update
// ordering instead of the recorded command order.
[[vk::binding(23, 0)]] RWTexture2D<float> outputOcclusion;

struct PushConstants
{
	uint2 viewportSize;
	float radiusPixels;
	float strength;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

float LoadDepthClamped(int2 pixel)
{
	const int2 clampedPixel = clamp(pixel, int2(0, 0), int2(config.viewportSize) - int2(1, 1));
	return inputDepth.Load(uint3(uint2(clampedPixel), 0));
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
	const int sampleRadius = max(1, (int)round(config.radiusPixels));
	float occlusion = 0.0f;
	float weight = 0.0f;

	// This depth-only approximation is intentionally conservative. Nearby
	// samples that are closer to the camera than the center pixel contribute
	// contact darkening, while far samples are ignored to avoid halos.
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			if (x == 0 && y == 0)
			{
				continue;
			}

			const int2 offset = int2(x, y) * sampleRadius;
			const float sampleDepth = LoadDepthClamped(int2(pixel) + offset);
			const float depthDelta = centerDepth - sampleDepth;
			occlusion += saturate(depthDelta * 128.0f);
			weight += 1.0f;
		}
	}

	const float normalizedOcclusion = weight > 0.0f ? occlusion / weight : 0.0f;
	outputOcclusion[pixel] = saturate(1.0f - normalizedOcclusion * max(config.strength, 0.0f));
}
