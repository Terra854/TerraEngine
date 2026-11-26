[[vk::binding(0, 0)]] Texture2D gFontAtlas : register(t0);
[[vk::binding(1, 0)]] SamplerState gFontSampler : register(s1);

struct PushConstants
{
	float2 gViewportSize;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

struct PSInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

float4 PSMain(PSInput input) : SV_Target0
{
	const float distanceField = gFontAtlas.Sample(gFontSampler, input.uv).r;
	// Derivative-based smoothing keeps small glyphs readable instead of relying
	// on one fixed threshold that only looks correct at a narrow size range.
	const float smoothing = max(fwidth(distanceField), 1.0f / 64.0f);
	const float alpha = smoothstep(0.5f - smoothing, 0.5f + smoothing, distanceField);
	if (alpha <= 0.01f)
	{
		discard;
	}
	return float4(input.color.rgb, input.color.a * alpha);
}
