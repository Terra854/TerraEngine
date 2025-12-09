struct PushConstants
{
	float4x4 modelViewProjection;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

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
	float3 normal : NORMAL;
};

VSOut VSMain(VSIn input)
{
	VSOut output;
	output.position = mul(SceneUniforms.modelViewProjection, float4(input.position, 1.0f));
	output.uv = input.uv;
	output.normal = input.normal;
	return output;
}
