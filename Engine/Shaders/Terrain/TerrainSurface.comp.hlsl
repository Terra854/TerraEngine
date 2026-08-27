// TerrainSurface.comp.hlsl -- Canonical four-material terrain surface stage.
#include "TerrainMaterial.hlsli"

Texture2D<float4> terrainControlWeights : register(t0);
Texture2D<float4> terrainControlStableIds : register(t1);
RWTexture2D<float4> terrainBaseColor : register(u0);
RWTexture2D<float4> terrainNormalRoughness : register(u1);
RWTexture2D<float4> terrainOrm : register(u2);
RWTexture2D<float4> terrainEmissive : register(u3);
RWTexture2D<uint> terrainFallbackMask : register(u4);

cbuffer TerrainSurfaceMapping : register(b0)
{
	TerrainAreaMappingConstants terrainAreaMapping;
	TerrainLayerMappingConstants terrainLayerMappings[kTerrainMaximumRuntimeLayers];
	uint terrainMappingLayerCount;
	uint3 terrainSurfaceMappingPadding;
};

[numthreads(8, 8, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
	const uint2 texel = dispatchId.xy;
	uint width = 0u;
	uint height = 0u;
	terrainControlWeights.GetDimensions(width, height);
	if(texel.x >= width || texel.y >= height) return;
	// Control texels address painted contributors directly. Physical mapping is
	// validated separately and cannot change which control texel this dispatch owns.
	const float2 pageLocalSample = (float2)texel;
	const float2 controlUv = TerrainControlUvFromLocalSample(
		pageLocalSample + terrainAreaMapping.localSampleOrigin, terrainAreaMapping);
	bool mappingValid = terrainMappingLayerCount == 0u
		|| (terrainMappingLayerCount <= kTerrainMaximumRuntimeLayers
			&& TerrainAreaMappingIsValid(terrainAreaMapping));
	[loop]
	for(uint layerIndex = 0u;
		layerIndex < min(terrainMappingLayerCount, kTerrainMaximumRuntimeLayers);
		++layerIndex)
	{
		const float2 surfaceUv = TerrainSurfaceUvFromPageLocalSample(
			pageLocalSample, terrainAreaMapping, terrainLayerMappings[layerIndex]);
		mappingValid = mappingValid
			&& TerrainLayerMappingIsValid(terrainLayerMappings[layerIndex])
			&& all(isfinite(surfaceUv));
	}
	float4 weights = max(terrainControlWeights.Load(int3(texel, 0)), 0.0f);
	const float sum = dot(weights, float4(1.0, 1.0, 1.0, 1.0));
	if (sum <= 0.0001 || !mappingValid)
	{
		terrainBaseColor[texel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
		terrainNormalRoughness[texel] = float4(0.0f, 1.0f, 0.0f, 1.0f);
		terrainOrm[texel] = float4(TerrainNeutralOrm, 1.0f);
		terrainEmissive[texel] = float4(TerrainNeutralEmissive, 1.0f);
		terrainFallbackMask[texel] = 1u;
		return;
	}
	weights /= sum;
	const uint4 stableIds = uint4(round(saturate(
		terrainControlStableIds.Load(int3(texel, 0))) * 65535.0f));
	TerrainPbrSample contributors[4];
	[unroll]
	for(uint contributor = 0u; contributor < 4u; ++contributor)
	{
		contributors[contributor].valid = false;
		if(weights[contributor] <= 0.0001f || stableIds[contributor] == 0u) continue;
		[loop]
		for(uint layerIndex = 0u;
			layerIndex < min(terrainMappingLayerCount, kTerrainMaximumRuntimeLayers);
			++layerIndex)
		{
			const TerrainGpuLayerBinding layer = terrainGpuLayers[layerIndex];
			if(layer.stableLayerId != stableIds[contributor]) continue;
			const float2 surfaceUv = TerrainSurfaceUvFromPageLocalSample(
				pageLocalSample, terrainAreaMapping, terrainLayerMappings[layerIndex]);
			const float2 localMetres = pageLocalSample * terrainAreaMapping.sampleSpacingMetres;
			const float2 mappedHorizontal = TerrainRotateCanonicalMetres(
				localMetres, layer.inverseRotationCosSin)
				+ terrainLayerMappings[layerIndex].phaseMetres;
			if(layer.mappingMode == kTerrainMappingModeTriplanar)
				contributors[contributor] = TerrainSampleLayerTriplanar(
					layer, float3(mappedHorizontal.x, 0.0f, mappedHorizontal.y),
					float3(0.0f, 1.0f, 0.0f));
			else contributors[contributor] = TerrainSampleLayerPlanar(
				layer, surfaceUv, float3(0.0f, 1.0f, 0.0f));
			break;
		}
	}
	const TerrainPbrSample blended = BlendTerrainContributors(
		contributors, weights, float3(0.0f, 1.0f, 0.0f));
	if(!blended.valid)
	{
		terrainBaseColor[texel] = float4(1.0f, 0.0f, 1.0f, 1.0f);
		terrainFallbackMask[texel] = 1u;
		return;
	}
	terrainBaseColor[texel] = blended.baseColor;
	terrainNormalRoughness[texel] = float4(blended.normal, blended.roughness);
	terrainOrm[texel] = float4(blended.ambientOcclusion, blended.roughness, blended.metallic, 1.0f);
	terrainEmissive[texel] = float4(blended.emissive, 1.0f);
	terrainFallbackMask[texel] = 0u;
}
