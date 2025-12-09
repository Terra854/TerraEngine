Texture2D baseColorTex : register(t0);
SamplerState linearClamp : register(s0);

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

float4 PSMain(PSIn input) : SV_Target0
{
	return baseColorTex.Sample(linearClamp, input.uv);
}
