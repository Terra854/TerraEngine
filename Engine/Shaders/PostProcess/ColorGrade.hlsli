#ifndef TERRAENGINE_COLOR_GRADE_HLSLI
#define TERRAENGINE_COLOR_GRADE_HLSLI

// All data stays linear Rec.709. The CPU supplies a validated chromatic
// adaptation matrix, allowing temperature/tint UI without a shader CCT model.
// Six aligned 16-byte rows make the Vulkan push range exactly 96 bytes.
struct ToneMapConstants
{
	float4 whiteBalance0; // xyz matrix row; w contrast, default 1
	float4 whiteBalance1; // xyz matrix row; w saturation, default 1
	float4 whiteBalance2; // xyz matrix row; w positive contrast pivot, default 0.18
	float4 gain; // xyz independent linear channel gain, default 1; w curve blend
	float4 lift; // xyz independent linear channel offset, default 0; w LUT blend
	uint4 options; // tone mode 0=legacy,1=neutral,2=filmic; flags; curve width; LUT edge
};

static const uint GradeWhiteBalance = 1u;
static const uint GradeAdjustments = 2u;
static const uint GradeCurves = 4u;
static const uint GradeLut = 8u;

float3 ApplyWhiteBalance(float3 color, ToneMapConstants settings)
{
	return float3(dot(settings.whiteBalance0.xyz, color),
		dot(settings.whiteBalance1.xyz, color), dot(settings.whiteBalance2.xyz, color));
}

float3 ApplyColorAdjustments(float3 color, ToneMapConstants settings)
{
	// Each control is independent. Gain1/lift 0/saturation 1/contrast 1 is identity.
	color = color * settings.gain.xyz + settings.lift.xyz;
	const float luminance = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
	color = lerp(luminance.xxx, color, settings.whiteBalance1.w);
	const float pivot = settings.whiteBalance2.w;
	return (color - pivot) * settings.whiteBalance0.w + pivot;
}

#if TERRAENGINE_COLOR_LOOKUPS
// Resource ownership and ingestion validation precede enabling either flag.
// Combined samplers match the existing fullscreen pass descriptor convention.
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> gradeCurves;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState gradeCurveSampler;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D<float4> gradeLut;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState gradeLutSampler;

float2 CurveUv(float value, uint width)
{
	// Nx1 RGBA stores R/G/B channel curves and a master curve in alpha.
	return float2((saturate(value) * float(width - 1u) + 0.5f) / float(width), 0.5f);
}

float3 ApplyGradeCurves(float3 color, uint width)
{
	const float3 master = float3(
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(color.r, width), 0.0f).a,
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(color.g, width), 0.0f).a,
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(color.b, width), 0.0f).a);
	return float3(
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(master.r, width), 0.0f).r,
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(master.g, width), 0.0f).g,
		gradeCurves.SampleLevel(gradeCurveSampler, CurveUv(master.b, width), 0.0f).b);
}

float3 SampleGradeLut(float3 color, uint edge)
{
	// The trusted 2D asset has width=N*N,height=N. Each horizontal B slice is
	// NxN, with R increasing right and G increasing down. Pixel-center bounds
	// keep bilinear taps inside their slice, including exact black/white corners.
	const float3 index = saturate(color) * float(edge - 1u);
	const float lowerBlue = floor(index.b);
	const float upperBlue = min(lowerBlue + 1.0f, float(edge - 1u));
	const float2 inverseSize = 1.0f / float2(float(edge) * float(edge), float(edge));
	const float2 lowerUv = float2(lowerBlue * float(edge) + index.r + 0.5f, index.g + 0.5f) * inverseSize;
	const float2 upperUv = float2(upperBlue * float(edge) + index.r + 0.5f, index.g + 0.5f) * inverseSize;
	const float3 lower = gradeLut.SampleLevel(gradeLutSampler, lowerUv, 0.0f).rgb;
	const float3 upper = gradeLut.SampleLevel(gradeLutSampler, upperUv, 0.0f).rgb;
	return lerp(lower, upper, index.b - lowerBlue);
}
#endif
#endif
