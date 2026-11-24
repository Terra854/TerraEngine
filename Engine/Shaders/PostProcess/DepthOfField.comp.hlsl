// HDR camera gather after temporal reconstruction and before tone mapping.
// Depth is finite positive linear eye distance in metres, at the color extent.
// The native graph must omit this pass and its private resources when disabled.
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::binding(1, 0)]] Texture2D<float> linearDepth;
#if defined(CAMERA_OUTPUT_RGBA32F)
[[vk::binding(2, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> outputColor;
#else
[[vk::binding(2, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> outputColor;
#endif
struct DepthOfFieldConstants
{
	uint2 extent;
	float focusDistance;
	float focusRange; // Full thickness of the sharp region, in metres.
	float blurRadiusPixels; // Maximum gather radius, bounded to64 pixels.
	uint enabled;
	float relativeDepthTolerance; // Bilateral surface tolerance; recommended0.02.
	uint reserved;
};
[[vk::push_constant]] ConstantBuffer<DepthOfFieldConstants> config;

float CircleRadius(float depth)
{
	float range = max(config.focusRange, 0.001f);
	float defocus = max(abs(depth - config.focusDistance) - 0.5f * range, 0.0f);
	return min(config.blurRadiusPixels, 64.0f) * saturate(defocus / range);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	uint2 size, destinationSize; sceneColor.GetDimensions(size.x, size.y);
	outputColor.GetDimensions(destinationSize.x, destinationSize.y);
	if (any(id.xy >= size) || any(id.xy >= destinationSize)) return;
	float4 center = sceneColor.Load(int3(id.xy, 0));
	outputColor[id.xy] = center;
	// The defensive bypass copies all four channels exactly and reads no depth.
	if (config.enabled == 0u || config.blurRadiusPixels <= 0.0f) return;
	uint2 depthSize;
	linearDepth.GetDimensions(depthSize.x, depthSize.y);
	if (any(size != config.extent) || any(depthSize != size) || any(destinationSize != size)) return;
	float depth = linearDepth.Load(int3(id.xy, 0));
	if (!isfinite(depth) || depth <= 0.0f || !all(isfinite(center))) return;
	float radius = CircleRadius(depth);
	if (!isfinite(radius) || radius < 0.5f) return;
	float3 sum = center.rgb; float weightSum = 1.0f;
	// A deterministic disk avoids frame-dependent noise. A sharp foreground
	// can occlude neighboring blur. A defocused silhouette can have partial
	// coverage and reveal the farther color within the lens footprint.
	[loop] for (uint tap = 0u; tap < 32u; ++tap)
	{
		float distance = sqrt((float(tap) + 0.5f) / 32.0f);
		float angle = float(tap) * 2.39996323f;
		int2 offset = int2(round(float2(cos(angle), sin(angle)) * distance * radius));
		int2 pixel = int2(id.xy) + offset;
		if (any(pixel < 0) || any(pixel >= int2(size))) continue;
		float sampleDepth = linearDepth.Load(int3(pixel, 0));
		float4 sampleColor = sceneColor.Load(int3(pixel, 0));
		if (!isfinite(sampleDepth) || sampleDepth <= 0.0f || !all(isfinite(sampleColor))) continue;
		float tolerance = max(0.001f, depth * max(config.relativeDepthTolerance, 0.0f));
		float difference = sampleDepth - depth;
		float surfaceWeight = saturate(1.0f - abs(difference) / tolerance);
		if (difference > tolerance)
		{
			// Behind the defocused center, the aperture sees a portion of the
			// farther surface. Limit its contribution at the center of the disk.
			surfaceWeight = 0.5f * distance;
		}
		else if (difference < -tolerance)
		{
			// A nearer sample contributes only when its own defocus footprint
			// reaches this pixel. A sharp occluder never spreads its color.
			float coverage = CircleRadius(sampleDepth) - length(float2(offset));
			surfaceWeight = saturate(coverage / max(radius, 1.0f));
		}
		float diskWeight = 1.0f - 0.5f * distance;
		float weight = surfaceWeight * diskWeight;
		sum += sampleColor.rgb * weight; weightSum += weight;
	}
	outputColor[id.xy] = float4(sum / weightSum, center.a);
}
