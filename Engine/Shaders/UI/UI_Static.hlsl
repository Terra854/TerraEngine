[[vk::binding(0, 0)]] Texture2D gTextures[] : register(t0);
[[vk::binding(0, 0)]] SamplerState gSamplers[] : register(s1);

struct PushConstants
{
	float2 gViewportSize;
	uint gTextureIndex;
	uint gUseTexture;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

struct VSInput
{
	float2 position : POSITION;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

struct PSInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float4 color : COLOR0;
};

// Converts screen-space quad vertices into clip space for static menu rendering.
PSInput VSMain(VSInput input)
{
	PSInput output;
	const float2 safeViewport = max(push.gViewportSize, float2(1.0f, 1.0f));
	const float2 clipPosition = float2(
		(input.position.x / safeViewport.x) * 2.0f - 1.0f,
		(input.position.y / safeViewport.y) * 2.0f - 1.0f);

	output.position = float4(clipPosition, 0.0f, 1.0f);
	output.uv = input.uv;
	output.color = input.color;
	return output;
}

float4 PSMain(PSInput input) : SV_Target0
{
	if (push.gUseTexture != 0)
	{
		return gTextures[push.gTextureIndex].Sample(gSamplers[push.gTextureIndex], input.uv) * input.color;
	}
	return input.color;
}
