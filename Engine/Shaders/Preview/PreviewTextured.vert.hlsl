cbuffer SceneUniforms : register(b0)
{
	float4x4 modelViewProjection;
};

struct VSIn
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float2 uv : TEXCOORD0;
};

struct VSOut
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(VSIn input)
{
	VSOut output;
	output.position = mul(modelViewProjection, float4(input.position, 1.0f));
	output.uv = input.uv;
	return output;
}
