// TerrainBrushPreview.vert.hlsl -- Editor-only terrain brush overlay vertex stage.

struct BrushVertexInput
{
	float3 position : POSITION;
	float2 uv : TEXCOORD0;
};

struct BrushVertexOutput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
};

cbuffer TerrainBrushPreviewConstants : register(b0)
{
	float4x4 viewProjection;
	float4x4 brushWorld;
};

BrushVertexOutput main(BrushVertexInput input)
{
	BrushVertexOutput output;
	// The CPU supplies canonical-to-view-local translation in brushWorld so this
	// overlay remains precise when the scene camera operates far from origin.
	output.position = mul(viewProjection, mul(brushWorld, float4(input.position, 1.0)));
	output.uv = input.uv;
	return output;
}
