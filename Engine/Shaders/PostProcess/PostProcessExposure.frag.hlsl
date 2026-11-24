// The paired binding uses one immutable combined-image-sampler descriptor.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> inputColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState inputSampler;

struct ExposureConstants
{
	float compensationEV;
};
[[vk::push_constant]] ConstantBuffer<ExposureConstants> exposure;

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
	const float4 color = inputColor.SampleLevel(inputSampler, uv, 0.0f);
	// Exposure operates on linear HDR color. Zero EV compensation is used
	// for the final sampled composite, preserving alpha for overlay blending.
	return float4(color.rgb * exp2(exposure.compensationEV), color.a);
}
