// Optional bloom dirt modulation in exposed linear HDR, before tone mapping.
// The dirt texture is authored linear data, not another encoded scene image.
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> bloomColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState bloomSampler;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] Texture2D<float4> dirtTexture;
[[vk::combinedImageSampler]][[vk::binding(2, 0)]] SamplerState dirtSampler;
struct LensDirtConstants
{
	uint2 outputSize;
	float intensity;
	uint enabled;
	float2 textureScale;
	float2 textureOffset;
};
[[vk::push_constant]] ConstantBuffer<LensDirtConstants> config;

float4 PSMain(float4 position : SV_Position, float2 unusedUv : TEXCOORD0) : SV_Target0
{
	uint2 pixel = uint2(position.xy);
	float4 scene = sceneColor.Load(int3(pixel, 0));
	// No authored texture or bloom lookup occurs on the exact identity path.
	if (config.enabled == 0u || config.intensity <= 0.0f) return scene;
	float2 uv = (float2(pixel) + 0.5f) / max(float2(config.outputSize), 1.0f);
	float3 bloom = bloomColor.SampleLevel(bloomSampler, uv, 0.0f).rgb;
	float3 dirt = dirtTexture.SampleLevel(dirtSampler, uv * config.textureScale + config.textureOffset, 0.0f).rgb;
	if (!all(isfinite(bloom)) || !all(isfinite(dirt))) return scene;
	// Add only bloom-driven light. Dirt remains invisible where bloom is black,
	// and its independent control does not re-add the unmodulated bloom layer.
	return float4(scene.rgb + max(bloom, 0.0f) * saturate(dirt) * config.intensity, scene.a);
}
