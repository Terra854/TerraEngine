// TerrainVisibility.vert.hlsl -- Terrain heightfield page visibility vertex stage.
struct TerrainVertexInput { float3 position : POSITION; float2 uv : TEXCOORD0; };
struct TerrainVertexOutput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
cbuffer TerrainViewConstants : register(b0) { float4x4 terrainViewProjection; };
TerrainVertexOutput main(TerrainVertexInput input) { TerrainVertexOutput output; output.position = mul(terrainViewProjection, float4(input.position, 1.0)); output.uv = input.uv; return output; }
