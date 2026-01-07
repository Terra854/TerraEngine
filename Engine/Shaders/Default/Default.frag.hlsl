[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];

struct MaterialData
{
	float4 BaseColorFactor;
	float4 EmissiveFactor;
	float4 UVTransform; // xy = scale, zw = offset
	float MetallicFactor;
	float RoughnessFactor;
	float NormalScale;
	float OcclusionStrength;
	float AlphaCutoff;
	uint AlphaMode;
	uint Flags;
	uint BaseColorTextureIndex;
	uint NormalTextureIndex;
	uint MaskTextureIndex;
	uint OcclusionTextureIndex;
	uint EmissiveTextureIndex;
	uint Padding[2];
};

[[vk::binding(1, 0)]] StructuredBuffer<MaterialData> allMaterials;

struct PushConstants
{
	float4x4 modelViewProjection;
	float4 positionOffset;
	float4 positionScale;
	float4 uvOffsetScale;
	uint textureIndex; 
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
	
	// Apply Material-level UV Transform
	float2 uv = input.uv * mat.UVTransform.xy + mat.UVTransform.zw;

	// Sample Base Color
	float4 baseColor = globalTextures[NonUniformResourceIndex(mat.BaseColorTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.BaseColorTextureIndex)], uv);
	float4 finalColor = baseColor * mat.BaseColorFactor;

	// Alpha Masking (Cutout)
	if (mat.AlphaMode == 1) // 1 = Mask
	{
		clip(finalColor.a - mat.AlphaCutoff);
	}

	// Basic PBR Texture Unpacking (Ready for lighting math)
	float3 normalMap = globalTextures[NonUniformResourceIndex(mat.NormalTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.NormalTextureIndex)], uv).rgb;
	float4 maskSample = globalTextures[NonUniformResourceIndex(mat.MaskTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.MaskTextureIndex)], uv);
	
	// glTF / Unity Mask Map unpacking: 
	// Green = Roughness, Blue = Metallic
	float roughness = maskSample.g * mat.RoughnessFactor;
	float metallic = maskSample.b * mat.MetallicFactor;

	float3 emissive = globalTextures[NonUniformResourceIndex(mat.EmissiveTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.EmissiveTextureIndex)], uv).rgb * mat.EmissiveFactor.rgb;

	// For now, return the shaded color (Unlit/Basic style until full PBR lighting is added)
	return float4(finalColor.rgb + emissive, finalColor.a);
}
