[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];

struct MaterialData
{
	float4 BaseColorFactor;
	float MetallicFactor;
	float RoughnessFactor;
	uint BaseColorTextureIndex;
};

[[vk::binding(1, 0)]] StructuredBuffer<MaterialData> allMaterials;

struct PushConstants
{
	float4x4 modelViewProjection;
	float4 positionOffset;
	float4 positionScale;
	float4 uvOffsetScale;
	uint textureIndex; // Kept for now, but material index is primary
	uint materialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : NORMAL;
};

float4 main(PSIn input) : SV_Target0
{
	MaterialData mat = allMaterials[SceneUniforms.materialIndex];
	uint textureIdx = mat.BaseColorTextureIndex;
	float4 baseColor = globalTextures[NonUniformResourceIndex(textureIdx)].Sample(globalSamplers[NonUniformResourceIndex(textureIdx)], input.uv);
	return baseColor * mat.BaseColorFactor;
}
