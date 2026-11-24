// Authored fullscreen validation effect, copied into a disposable project and
// compiled by its real ShaderImporter. DisplayLinear source/destination, before
// UI; default parameters are an exact identity. No engine globals are sampled.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> sourceColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState sourceSampler;

struct FixtureTintParameters
{
	float4 tint; // Vector4 at byte0. The fixture keeps alpha=1.
	float gain; // Independent Scalar at byte16, applied to RGB only.
};
[[vk::push_constant]] ConstantBuffer<FixtureTintParameters> parameters;

float4 PSMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
	float4 source = sourceColor.SampleLevel(sourceSampler, uv, 0.0f);
	return float4(source.rgb * parameters.tint.rgb * parameters.gain, source.a * parameters.tint.a);
}
