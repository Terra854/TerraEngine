[[vk::binding(0, 0)]] Texture2D baseColorTex;
[[vk::binding(1, 0)]] SamplerState linearClamp;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : NORMAL;
};

float4 PSMain(PSIn input) : SV_Target0
{
	return baseColorTex.Sample(linearClamp, input.uv);
}
