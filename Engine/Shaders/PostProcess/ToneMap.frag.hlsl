#ifndef TERRAENGINE_COLOR_LOOKUPS
#define TERRAENGINE_COLOR_LOOKUPS 0
#endif
#include "ColorGrade.hlsli"
#include "ToneGT7.hlsli"

[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> inputColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState inputSampler;
[[vk::push_constant]] ConstantBuffer<ToneMapConstants> tone;

float FiniteHdrChannel(float value)
{
	// Negative/non-finite radiance is outside the tone operators' domains.
	// The bound prevents overflow in the rational polynomial without affecting
	// finite source HDR textures (including half-float's entire positive range).
	return isnan(value) ? 0.0f : clamp(value, 0.0f, 1.0e18f);
}
float3 NonnegativeFiniteHdr(float3 color)
{
	return float3(FiniteHdrChannel(color.r), FiniteHdrChannel(color.g), FiniteHdrChannel(color.b));
}

float3 ToneNeutral(float3 radiance)
{
	// Independently expressed from the published Khronos PBR Neutral equations:
	// https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral
	// Linear Rec.709 nonnegative HDR -> linear Rec.709 SDR. No transfer encoding.
	const float minimum = min(radiance.r, min(radiance.g, radiance.b));
	const float offset = minimum <= 0.08f ? minimum - minimum * minimum / 0.16f : 0.04f;
	const float3 base = radiance - offset;
	const float peak = max(base.r, max(base.g, base.b));
	if(peak <= 0.76f) return base;
	const float compressedPeak = 1.0f - 0.0576f / (peak - 0.52f);
	const float retainedChroma = rcp(1.0f + 0.15f * (peak - compressedPeak));
	return lerp(compressedPeak.xxx, base * (compressedPeak / peak), retainedChroma);
}

float3 ToneFilmic(float3 radiance)
{
	// Narkowicz 2016 CC0 fitted filmic rational curve, linear Rec.709 in/out:
	// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
	// This defines the named fitted operator; it is not a complete ACES pipeline.
	return saturate((radiance * (2.51f * radiance + 0.03f)) /
		(radiance * (2.43f * radiance + 0.59f) + 0.14f));
}

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
	const float4 source = inputColor.SampleLevel(inputSampler, uv, 0.0f);
	// The disabled stack preserves the legacy signal, including alpha/HDR, exactly.
	if(tone.options.x == 0u && tone.options.y == 0u) return source;
	float3 color = NonnegativeFiniteHdr(source.rgb);
	if((tone.options.y & GradeWhiteBalance) != 0u) color = NonnegativeFiniteHdr(ApplyWhiteBalance(color, tone));
	if(tone.options.x == 1u) color = ToneNeutral(color);
	else if(tone.options.x == 2u) color = ToneFilmic(color);
	else if(tone.options.x == 3u) color = ToneGT7(color);
	if((tone.options.y & GradeAdjustments) != 0u) color = ApplyColorAdjustments(color, tone);
#if TERRAENGINE_COLOR_LOOKUPS
	if((tone.options.y & GradeCurves) != 0u && tone.options.z >= 2u && tone.gain.w > 0.0f)
		color = lerp(color, ApplyGradeCurves(color, tone.options.z), saturate(tone.gain.w));
	if((tone.options.y & GradeLut) != 0u && tone.options.w >= 2u && tone.lift.w > 0.0f)
		color = lerp(color, SampleGradeLut(color, tone.options.w), saturate(tone.lift.w));
#endif
	// Preserve source alpha. SDR lookups are explicitly display-linear; the
	// final compositor alone applies output transfer, after overlays are composed.
	return float4(NonnegativeFiniteHdr(color), source.a);
}
