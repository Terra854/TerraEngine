// ValidAnimatedSurface.frag.hlsl
// This fixture changes only surface color. Geometry, including animation and
// motion history, is supplied through TerraEngine's installed geometry contract.

#include "Geometry/TerraGeometryContract.hlsli"

float4 main(TerraSurfaceInput input) : SV_TARGET
{
	const float facing = saturate(dot(normalize(input.normal), normalize(float3(0.3F, 0.8F, 0.4F))));
	return float4(0.15F + 0.75F * facing, input.uv.x, input.uv.y, 1.0F);
}
