// Output-space styling after spatial/temporal AA and before default UI.
// Source and destination are display-referred LINEAR Rec.709; encode once later.
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] Texture2D<float4> inputColor;
[[vk::combinedImageSampler]][[vk::binding(0, 0)]] SamplerState inputSampler;
struct LensPresentationConstants
{
	uint2 outputSize;
	uint enabledMask; // Bit0 vignette, bit1 grain, bit2 chromatic, bit3 distortion.
	uint frameSeed; // Native per-view/per-frame seed; never shared mutable state.
	float vignetteIntensity;
	float vignetteRadius; // Radius in image-height units about opticalCenter.
	float vignetteSoftness;
	float grainIntensity;
	float grainResponse; // Nonnegative strength of shadow-biased grain response.
	float chromaticPixels;
	float distortionK1;
	float distortionK2;
	float2 opticalCenter; // Normalized UV, normally(0.5,0.5).
	float2 reserved;
};
[[vk::push_constant]] ConstantBuffer<LensPresentationConstants> config;

uint MixBits(uint value)
{
	value ^= value >> 16u; value *= 0x7feb352du;
	value ^= value >> 15u; value *= 0x846ca68bu;
	return value ^ (value >> 16u);
}
float Noise(uint2 pixel)
{
	uint value = MixBits(pixel.x ^ MixBits(pixel.y ^ config.frameSeed));
	return float(value >> 8u) * (1.0f / 16777216.0f) - 0.5f;
}
float3 AtUv(float2 uv)
{
	// A distorted ray outside the rendered image has no source radiance. Avoid
	// stretching clamp-to-edge border texels into the uncovered lens region.
	if (any(uv < 0.0f) || any(uv > 1.0f)) return 0.0f;
	return inputColor.SampleLevel(inputSampler, uv, 0.0f).rgb;
}

float4 PSMain(float4 position : SV_Position, float2 unusedUv : TEXCOORD0) : SV_Target0
{
	uint2 pixel = uint2(position.xy);
	float4 center = inputColor.Load(int3(pixel, 0));
	bool vignette = (config.enabledMask & 1u) != 0u && config.vignetteIntensity > 0.0f;
	bool grain = (config.enabledMask & 2u) != 0u && config.grainIntensity > 0.0f;
	bool chromatic = (config.enabledMask & 4u) != 0u && config.chromaticPixels != 0.0f;
	bool distortion = (config.enabledMask & 8u) != 0u && (config.distortionK1 != 0.0f || config.distortionK2 != 0.0f);
	// Return original bits if all contributions are off, even if flags are set.
	if (!vignette && !grain && !chromatic && !distortion) return center;
	float2 size = max(float2(config.outputSize), 1.0f);
	float2 uv = (float2(pixel) + 0.5f) / size;
	float aspect = size.x / size.y;
	float2 radial = (uv - config.opticalCenter) * float2(aspect, 1.0f);
	float radiusSquared = dot(radial, radial);
	float radius = sqrt(radiusSquared);
	float2 sampleUv = uv;
	if (distortion)
	{
		float scale = 1.0f + config.distortionK1 * radiusSquared + config.distortionK2 * radiusSquared * radiusSquared;
		sampleUv = config.opticalCenter + (uv - config.opticalCenter) * scale;
	}
	// Point-load the center when no spatial warp is requested; grain/vignette
	// alone must not accidentally resample or soften the already resolved image.
	float3 color = distortion ? AtUv(sampleUv) : center.rgb;
	if (chromatic && radius > 0.000001f)
	{
		float2 displacement = radial / radius * config.chromaticPixels / size;
		color.r = AtUv(sampleUv + displacement).r;
		color.b = AtUv(sampleUv - displacement).b;
	}
	if (vignette)
	{
		float falloff = smoothstep(max(config.vignetteRadius, 0.0f),
			max(config.vignetteRadius, 0.0f) + max(config.vignetteSoftness, 0.000001f), radius);
		color *= 1.0f - saturate(config.vignetteIntensity) * falloff;
	}
	if (grain)
	{
		float luminance = max(dot(color, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
		float response = rcp(1.0f + luminance * max(config.grainResponse, 0.0f));
		color = max(color + Noise(pixel) * config.grainIntensity * response, 0.0f);
	}
	// Camera styling never modifies the original transparency/coverage channel.
	return float4(color, center.a);
}
