#include "ExposureCommon.hlsli"
[[vk::binding(0, 0)]] Texture2D<float4> unexposedScene;
[[vk::binding(3, 0)]] StructuredBuffer<ExposureState> currentExposure;
#if defined(EXPOSURE_OUTPUT_RGBA32F)
[[vk::binding(4, 0), vk::image_format("rgba32f")]] RWTexture2D<float4> exposedScene;
static const float maximumFiniteOutput = 3.402823466e+38f;
#else
[[vk::binding(4, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> exposedScene;
static const float maximumFiniteOutput = 65504.0f;
#endif
struct ApplicationConstants
{
	uint width;
	uint height;
	float compensationEV;
	uint automaticExposureEnabled;
};
[[vk::push_constant]] ConstantBuffer<ApplicationConstants> parameters;
float SanitizeChannel(float value)
{
	return isfinite(value) ? value : 0.0f;
}
float ExposeChannel(float value, float multiplier)
{
	value = SanitizeChannel(value);
	// Clamp before multiplication, preserving every representable finite result
	// while preventing HDR overflow from introducing an infinity into bloom.
	const float safeInput = min(3.402823466e+38f, maximumFiniteOutput / multiplier);
	const float result = clamp(value, -safeInput, safeInput) * multiplier;
	return isfinite(result) ? clamp(result, -maximumFiniteOutput, maximumFiniteOutput)
		: (value < 0.0f ? -maximumFiniteOutput : maximumFiniteOutput);
}
[numthreads(8, 8, 1)]
void ApplyExposure(uint3 pixel : SV_DispatchThreadID)
{
	if(pixel.x >= parameters.width || pixel.y >= parameters.height) return;
	float automaticEV = 0.0f;
	if(parameters.automaticExposureEnabled != 0u)
	{
		const ExposureState state = currentExposure[0];
		if(state.valid == 1u && isfinite(state.exposureEV)) automaticEV = clamp(state.exposureEV, -24.0f, 24.0f);
	}
	const float compensationEV = isfinite(parameters.compensationEV) ? clamp(parameters.compensationEV, -24.0f, 24.0f) : 0.0f;
	// The meter and history exclude compensation. This is the single application
	// of both adapted EV and authored fixed compensation at the linear boundary.
	const float multiplier = exp2(automaticEV + compensationEV);
	const float4 color = unexposedScene.Load(int3(pixel.xy, 0));
	exposedScene[pixel.xy] = float4(ExposeChannel(color.r, multiplier), ExposeChannel(color.g, multiplier),
		ExposeChannel(color.b, multiplier), clamp(SanitizeChannel(color.a), -maximumFiniteOutput, maximumFiniteOutput));
}
