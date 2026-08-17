// TerrainSurface.comp.hlsl -- Canonical four-material terrain control page stage.
// Each page already contains stable layer IDs on the CPU. This shader only
// normalizes the four selected weights and leaves unavailable sources obvious.
RWTexture2D<float4> terrainSurface : register(u0);
RWTexture2D<uint> terrainFallbackMask : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	const uint2 texel = dispatchId.xy;
	float4 weights = max(terrainSurface[texel], 0.0);
	const float sum = dot(weights, float4(1.0, 1.0, 1.0, 1.0));
	if (sum <= 0.0001)
	{
		// A magenta-safe material is selected by the visibility pass when all
		// referenced sources are unavailable; zero here is never silently blended.
		terrainSurface[texel] = float4(1.0, 0.0, 0.0, 0.0);
		terrainFallbackMask[texel] = 1u;
		return;
	}
	terrainSurface[texel] = weights / sum;
	terrainFallbackMask[texel] = 0u;
}
