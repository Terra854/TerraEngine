// TerrainVisibility.frag.hlsl -- Terrain visibility with explicit material fallback.
// This interpolator contract is emitted by TerrainVisibility.mesh.hlsl.
#include "TerrainMaterial.hlsli"

struct TerrainFragmentInput
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 normal : TEXCOORD1;
};
Texture2D<float4> terrainControlWeights : register(t0);
Texture2D<float4> terrainControlStableIds : register(t1);
Texture2D<uint> terrainFallbackMask : register(t2);
Texture2D<uint> terrainHoleMask : register(t3);
SamplerState terrainSampler : register(s0);

cbuffer TerrainVisibilityMapping : register(b0)
{
	TerrainAreaMappingConstants terrainAreaMapping;
	TerrainLayerMappingConstants terrainLayerMappings[kTerrainMaximumRuntimeLayers];
	uint terrainMappingLayerCount;
	uint3 terrainVisibilityMappingPadding;
};

float4 TerrainVisibilityDiagnosticPattern(float2 surfaceUv)
{
	const int2 cell = int2(floor(surfaceUv * 2.0f));
	return ((cell.x + cell.y) & 1) != 0
		? float4(1.0f, 0.82f, 0.0f, 1.0f)
		: float4(0.015f, 0.015f, 0.015f, 1.0f);
}

float4 main(TerrainFragmentInput input) : SV_Target0
{
	const uint2 texel = uint2(input.position.xy);
	// Hole coverage is sampled by every visibility path, including shadows and
	// picking. Discarding here prevents a render-only surface over a physical gap.
	if (terrainHoleMask.Load(int3(texel, 0)) != 0u)
		discard;
	uint controlWidth = 0u;
	uint controlHeight = 0u;
	terrainControlWeights.GetDimensions(controlWidth, controlHeight);
	const uint2 controlTexel = TerrainControlTexelFromUv(
		input.uv, uint2(controlWidth, controlHeight));
	const float2 diagnosticUv = terrainMappingLayerCount > 0u
		? TerrainDiagnosticSurfaceUv(
			input.uv, terrainAreaMapping, terrainLayerMappings[0])
		: input.uv;
	if (terrainFallbackMask.Load(int3(texel, 0)) != 0u)
		return TerrainVisibilityDiagnosticPattern(diagnosticUv);
	// Painted layer selection is an exact control-map load. Tile size, phase,
	// rotation, and future triplanar projection never feed this address.
	float4 normalizedWeights = max(
		terrainControlWeights.Load(int3(controlTexel, 0)), 0.0f);
	const float weightSum = dot(normalizedWeights, 1.0f);
	if(weightSum <= 0.0001f)
		return TerrainVisibilityDiagnosticPattern(diagnosticUv);
	normalizedWeights /= weightSum;
	const uint4 stableIds = uint4(round(saturate(
		terrainControlStableIds.Load(int3(controlTexel, 0))) * 65535.0f));
	TerrainPbrSample contributors[4];
	const float2 areaLocalSample = TerrainLocalSampleFromControlUv(
		input.uv, terrainAreaMapping);
	[unroll]
	for(uint contributor = 0u; contributor < 4u; ++contributor)
	{
		contributors[contributor].valid = false;
		if(normalizedWeights[contributor] <= 0.0001f || stableIds[contributor] == 0u) continue;
		[loop]
		for(uint layerIndex = 0u;
			layerIndex < min(terrainMappingLayerCount, kTerrainMaximumRuntimeLayers);
			++layerIndex)
		{
			const TerrainGpuLayerBinding layer = terrainGpuLayers[layerIndex];
			if(layer.stableLayerId != stableIds[contributor]) continue;
			const float2 surfaceUv = TerrainSurfaceUvFromAreaLocalSample(
				areaLocalSample, terrainAreaMapping, terrainLayerMappings[layerIndex]);
			const float2 localMetres = areaLocalSample * terrainAreaMapping.sampleSpacingMetres;
			const float2 mappedHorizontal = TerrainRotateCanonicalMetres(
				localMetres, layer.inverseRotationCosSin)
				+ terrainLayerMappings[layerIndex].phaseMetres;
			if(layer.mappingMode == kTerrainMappingModeTriplanar)
				contributors[contributor] = TerrainSampleLayerTriplanar(
					layer, float3(mappedHorizontal.x, 0.0f, mappedHorizontal.y),
					normalize(input.normal));
			else contributors[contributor] = TerrainSampleLayerPlanar(
				layer, surfaceUv, normalize(input.normal));
			break;
		}
	}
	const TerrainPbrSample blended = BlendTerrainContributors(
		contributors, normalizedWeights, normalize(input.normal));
	if(!blended.valid) return TerrainVisibilityDiagnosticPattern(diagnosticUv);
	// This visibility target carries the fully resolved surface color. The page
	// compute route writes the same shared sample fields for deferred consumers.
	return float4(blended.baseColor.rgb + blended.emissive, blended.baseColor.a);
}
