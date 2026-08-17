// TerrainVisibility.frag.hlsl -- Terrain visibility with explicit material fallback.
struct TerrainFragmentInput { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> terrainSurface : register(t0);
Texture2D<uint> terrainFallbackMask : register(t1);
Texture2D<uint> terrainHoleMask : register(t2);
SamplerState terrainSampler : register(s0);

float4 main(TerrainFragmentInput input) : SV_Target0
{
	const uint2 texel = uint2(input.position.xy);
	// Hole coverage is sampled by every visibility path, including shadows and
	// picking. Discarding here prevents a render-only surface over a physical gap.
	if (terrainHoleMask.Load(int3(texel, 0)) != 0u)
		discard;
	if (terrainFallbackMask.Load(int3(texel, 0)) != 0u)
		return float4(1.0, 0.0, 1.0, 1.0); // Deliberate missing-source diagnostic.
	const float4 normalizedWeights = terrainSurface.SampleLevel(terrainSampler, input.uv, 0.0);
	return float4(normalizedWeights.rgb, 1.0);
}
