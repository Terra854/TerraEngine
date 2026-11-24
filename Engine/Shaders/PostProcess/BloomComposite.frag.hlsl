// Bloom pass: combine scene and bloom before the tone-mapping transfer.
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> bloomColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState bloomSampler;

struct BloomCompositeConstants
{
	float intensity; // Non-negative linear radiance multiplier; zero is identity.
	uint enabled;
	uint2 outputSize;
};
[[vk::push_constant]] ConstantBuffer<BloomCompositeConstants> config;

float4 PSMain(float4 position : SV_Position, float2 unusedUv : TEXCOORD0) : SV_Target0
{
	const uint2 pixel = uint2(position.xy);
	const float4 scene = sceneColor.Load(uint3(pixel, 0));
	// The scheduler omits the chain when disabled. This defensive identity path
	// neither samples bloom nor changes the scene's RGB or transparency.
	if (config.enabled == 0u || config.intensity <= 0.0f) return scene;
	const float2 uv = (float2(pixel) + 0.5f) / float2(config.outputSize);
	const float3 bloom = bloomColor.SampleLevel(bloomSampler, uv, 0.0f).rgb;
	return float4(scene.rgb + bloom * config.intensity, scene.a);
}
