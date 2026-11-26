[[vk::binding(0, 0)]] Texture2D gTextures[] : register(t0);
[[vk::binding(0, 0)]] SamplerState gSamplers[] : register(s1);

struct PushConstants
{
	float2 gViewportSize;
	uint gTextureIndex;
	uint gUseTexture;
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
	if (push.gUseTexture != 0)
	{
		return gTextures[push.gTextureIndex].Sample(gSamplers[push.gTextureIndex], input.uv) * input.color;
	}
	return input.color;
}
