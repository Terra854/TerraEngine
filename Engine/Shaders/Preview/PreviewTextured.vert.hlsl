struct PushConstants
{
	float4x4 modelViewProjection;
	float4 positionOffset; // xyz, w unused
	float4 positionScale;  // xyz, w unused
	float4 uvOffsetScale;  // xy = offset, zw = scale
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
	
	// De-quantize position
	float3 worldPos = input.position * SceneUniforms.positionScale.xyz + SceneUniforms.positionOffset.xyz;
	
	// De-quantize UVs
	float2 realUV = input.uv * SceneUniforms.uvOffsetScale.zw + SceneUniforms.uvOffsetScale.xy;

	output.position = mul(SceneUniforms.modelViewProjection, float4(worldPos, 1.0f));
	output.uv = realUV;
	output.normal = input.normal; // Normals are usually just direction, scale might affect them but usually we re-normalize or use inverse transpose. 
	// Since we are just translating/uniform scaling, the direction *should* be fine if input.normal was normalized 
	// and stored as SNORM. SNORM unpacks to -1..1, which is correct direction. 
	// Ideally we might want to normalize it again to be safe.
	
	return output;
}
