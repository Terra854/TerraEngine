// Bounded screen-space lens ghosts and halo from the exposed HDR bloom input.
// This camera pass belongs before tone mapping and before default UI/overlays.
[[vk::binding(0, 0)]] Texture2D<float4> sceneColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] Texture2D<float4> bloomColor;
[[vk::combinedImageSampler]][[vk::binding(1, 0)]] SamplerState bloomSampler;
struct LensFlareConstants
{
	uint2 outputSize;
	float intensity;
	uint enabled;
	float threshold;
	float ghostSpacing;
	float haloRadius; // Normalized image height, independent of aspect ratio.
	float haloWidth;
	float3 tint;
	uint ghostCount; // Bounded to8; zero removes only the ghost contribution.
};
[[vk::push_constant]] ConstantBuffer<LensFlareConstants> config;

float3 BrightSource(float2 uv)
{
	if (any(uv < 0.0f) || any(uv > 1.0f)) return 0.0f;
	float3 color = bloomColor.SampleLevel(bloomSampler, uv, 0.0f).rgb;
	if (!all(isfinite(color))) return 0.0f;
	float luminance = dot(max(color, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
	// Threshold chroma uniformly so a bright colored source retains its hue.
	float weight = max(luminance - max(config.threshold, 0.0f), 0.0f) / max(luminance, 0.000001f);
	return max(color, 0.0f) * weight;
}

float4 PSMain(float4 position : SV_Position, float2 unusedUv : TEXCOORD0) : SV_Target0
{
	uint2 pixel = uint2(position.xy);
	float4 scene = sceneColor.Load(int3(pixel, 0));
	if (config.enabled == 0u || config.intensity <= 0.0f) return scene;
	float2 size = max(float2(config.outputSize), 1.0f);
	float2 uv = (float2(pixel) + 0.5f) / size;
	float2 axis = float2(0.5f, 0.5f) - uv;
	float3 flare = 0.0f;
	uint count = min(config.ghostCount, 8u);
	// Mirrored source copies form ghosts along the optical axis. Samples outside
	// the original image are absent, rather than wrapped or clamped into streaks.
	[loop] for (uint ghost = 0u; ghost < count; ++ghost)
	{
		float scale = 1.0f + float(ghost) * max(config.ghostSpacing, 0.0f);
		float2 sampleUv = float2(0.5f, 0.5f) + axis * scale;
		float attenuation = 1.0f / (1.0f + float(ghost));
		flare += BrightSource(sampleUv) * attenuation;
	}
	// Aspect-correct radial gathering forms a halo without division at center.
	float aspect = size.x / size.y;
	float2 radial = (uv - 0.5f) * float2(aspect, 1.0f);
	float radius = length(radial);
	if (config.haloRadius > 0.0f && config.haloWidth > 0.0f && radius > 0.000001f)
	{
		float2 direction = radial / radius;
		float width = max(config.haloWidth, 0.000001f);
		float haloWeight = 1.0f - smoothstep(0.0f, width, abs(radius - config.haloRadius));
		float2 haloUv = 0.5f - direction * config.haloRadius / float2(aspect, 1.0f);
		flare += BrightSource(haloUv) * haloWeight;
	}
	return float4(scene.rgb + flare * max(config.tint, 0.0f) * config.intensity, scene.a);
}
