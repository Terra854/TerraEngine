// HDR camera motion blur before tone mapping, without temporal color history.
// Both velocities are previous UNJITTERED UV minus current UNJITTERED UV;
// xy stores velocity and w>=0.5 certifies motion. Convert raster+jitter motion
// at the native producer boundary, never reinterpret temporal motion here.
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::binding(1, 0)]] Texture2D<float> linearDepth;
[[vk::binding(2, 0)]] Texture2D<float4> totalVelocity;
[[vk::binding(3, 0)]] Texture2D<float4> cameraVelocity;
[[vk::binding(4, 0)]] Texture2D<float> disocclusion;
#if defined(CAMERA_OUTPUT_RGBA32F)
[[vk::binding(5, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> outputColor;
#else
[[vk::binding(5, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> outputColor;
#endif
struct MotionBlurConstants
{
	uint2 extent;
	float strength; // Fraction of the supplied frame displacement to integrate.
	float cameraContribution;
	float objectContribution;
	float maxRadiusPixels;
	float relativeDepthTolerance;
	uint enabled;
};
[[vk::push_constant]] ConstantBuffer<MotionBlurConstants> config;

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
	uint2 size, destinationSize; sceneColor.GetDimensions(size.x, size.y);
	outputColor.GetDimensions(destinationSize.x, destinationSize.y);
	if (any(id.xy >= size) || any(id.xy >= destinationSize)) return;
	float4 center = sceneColor.Load(int3(id.xy, 0));
	outputColor[id.xy] = center;
	if (config.enabled == 0u || config.strength <= 0.0f
		|| (config.cameraContribution <= 0.0f && config.objectContribution <= 0.0f)) return;
	uint2 depthSize, totalSize, cameraSize, validitySize;
	linearDepth.GetDimensions(depthSize.x, depthSize.y);
	totalVelocity.GetDimensions(totalSize.x, totalSize.y);
	cameraVelocity.GetDimensions(cameraSize.x, cameraSize.y);
	disocclusion.GetDimensions(validitySize.x, validitySize.y);
	if (any(size != config.extent) || any(depthSize != size) || any(totalSize != size)
		|| any(cameraSize != size) || any(validitySize != size) || any(destinationSize != size)) return;
	float4 total = totalVelocity.Load(int3(id.xy, 0));
	float4 camera = cameraVelocity.Load(int3(id.xy, 0));
	float depth = linearDepth.Load(int3(id.xy, 0));
	float invalid = disocclusion.Load(int3(id.xy, 0));
	if (!all(isfinite(total)) || !all(isfinite(camera)) || total.w < 0.5f || camera.w < 0.5f
		|| !isfinite(invalid) || invalid > 0.5f || !isfinite(depth) || depth <= 0.0f || !all(isfinite(center))) return;
	// Subtraction separates residual object motion from camera reprojection.
	// The two controls are independent even when they cancel in total motion.
	float2 motion = (camera.xy * max(config.cameraContribution, 0.0f)
		+ (total.xy - camera.xy) * max(config.objectContribution, 0.0f)) * max(config.strength, 0.0f) * float2(size);
	float lengthPixels = length(motion);
	if (!isfinite(lengthPixels) || lengthPixels < 0.5f) return;
	motion *= min(lengthPixels, min(max(config.maxRadiusPixels, 0.0f), 128.0f)) / lengthPixels;
	if (length(motion) < 0.5f) return;
	float3 sum = center.rgb; float weightSum = 1.0f;
	// Gather only current-frame radiance. Newly visible or invalid pixels cannot
	// pull old history into the image, and bilateral weights reject depth seams.
	[loop] for (uint tap = 1u; tap <= 24u; ++tap)
	{
		float shutter = (float(tap) - 0.5f) / 24.0f - 0.5f;
		int2 pixel = int2(round(float2(id.xy) + motion * shutter));
		if (any(pixel < 0) || any(pixel >= int2(size))) continue;
		float sampleInvalid = disocclusion.Load(int3(pixel, 0));
		float sampleDepth = linearDepth.Load(int3(pixel, 0));
		float4 sampleColor = sceneColor.Load(int3(pixel, 0));
		if (!isfinite(sampleInvalid) || sampleInvalid > 0.5f || !isfinite(sampleDepth)
			|| sampleDepth <= 0.0f || !all(isfinite(sampleColor))) continue;
		float tolerance = max(0.001f, depth * max(config.relativeDepthTolerance, 0.0f));
		float weight = saturate(1.0f - abs(sampleDepth - depth) / tolerance);
		sum += sampleColor.rgb * weight; weightSum += weight;
	}
	outputColor[id.xy] = float4(sum / weightSum, center.a);
}
