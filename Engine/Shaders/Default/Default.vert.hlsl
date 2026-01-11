#pragma pack_matrix(column_major)

struct PushConstants
{
	column_major float4x4 viewProj;
    column_major float4x4 model;
	uint textureIndex;
	uint materialIndex;
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

VSOut main(VSIn input)
{
	VSOut output;
	
    // Transform to world space
    float4 worldPos = mul(SceneUniforms.model, float4(input.position, 1.0f));

	output.position = mul(SceneUniforms.viewProj, worldPos);
	output.uv = input.uv;
	output.normal = mul((float3x3)SceneUniforms.model, input.normal); 
	
	return output;
}
