// TerrainMaterial.hlsli -- Shared canonical terrain mapping helpers.
//
// Surface coordinates are reconstructed exclusively from authored local sample
// coordinates, physical sample spacing, and a CPU-reduced area phase. Camera-
// relative or translated world positions must never enter these functions.

#ifndef TERRAENGINE_TERRAIN_MATERIAL_HLSLI
#define TERRAENGINE_TERRAIN_MATERIAL_HLSLI

static const uint kTerrainMappingModeTerrainXZ = 0u;
static const uint kTerrainMappingModeTriplanar = 1u;
static const uint kTerrainMaximumRuntimeLayers = 16u;
static const uint kTerrainNeutralTextureIndex = 0xffffffffu;
static const float4 TerrainNeutralBaseColor = float4(1.0f, 1.0f, 1.0f, 1.0f);
static const float3 TerrainNeutralNormal = float3(0.5f, 0.5f, 1.0f);
static const float3 TerrainNeutralOrm = float3(1.0f, 1.0f, 0.0f);
static const float3 TerrainNeutralEmissive = float3(0.0f, 0.0f, 0.0f);

// Exact 96-byte StructuredBuffer ABI. Keep field names and order synchronized
// with TerrainMaterialRuntimeRegistry.h; every scalar occupies four bytes.
struct TerrainGpuLayerBinding
{
	uint baseColorTextureIndex;
	uint normalTextureIndex;
	uint ormTextureIndex;
	uint emissiveTextureIndex;
	float4 baseColorFactor;
	float4 emissiveFactor;
	float normalScale;
	float occlusionStrength;
	float roughnessFactor;
	float metallicFactor;
	float2 tileSizeMetres;
	float2 inverseRotationCosSin;
	float triplanarSharpness;
	uint mappingMode;
	uint stableLayerId;
	uint reserved;
};

struct TerrainPbrSample
{
	float4 baseColor;
	float3 normal;
	float ambientOcclusion;
	float roughness;
	float metallic;
	float3 emissive;
	bool valid;
};

// Terrain-only aliases share one fixed declaration in direct and page-backed
// shaders. An invalid index selects a semantic neutral without sampling.
Texture2D<float4> terrainMaterialTextures[64] : register(t8);
SamplerState terrainMaterialSamplers[64] : register(s8);
StructuredBuffer<TerrainGpuLayerBinding> terrainGpuLayers : register(t72);

struct TerrainAreaMappingConstants
{
	float2 localSampleOrigin;
	float sampleSpacingMetres;
	uint controlExtentSamples;
};

struct TerrainLayerMappingConstants
{
	float2 tileSizeMetres;
	float2 phaseMetres;
	// cos/sin for the inverse authored rotation, prepared on the CPU.
	float2 inverseRotationCosSin;
	float triplanarSharpness;
	uint mappingMode;
};

bool TerrainAreaMappingIsValid(TerrainAreaMappingConstants area)
{
	return all(isfinite(area.localSampleOrigin))
		&& isfinite(area.sampleSpacingMetres) && area.sampleSpacingMetres > 0.0f
		&& area.controlExtentSamples == 256u;
}

bool TerrainLayerMappingIsValid(TerrainLayerMappingConstants layer)
{
	const float rotationLengthSquared = dot(
		layer.inverseRotationCosSin, layer.inverseRotationCosSin);
	return all(isfinite(layer.tileSizeMetres)) && all(layer.tileSizeMetres > 0.0f)
		&& all(isfinite(layer.phaseMetres)) && all(layer.phaseMetres >= 0.0f)
		&& all(layer.phaseMetres < layer.tileSizeMetres)
		&& all(isfinite(layer.inverseRotationCosSin))
		&& abs(rotationLengthSquared - 1.0f) <= 0.001f
		&& isfinite(layer.triplanarSharpness)
		&& layer.triplanarSharpness >= 1.0f && layer.triplanarSharpness <= 32.0f
		&& (layer.mappingMode == kTerrainMappingModeTerrainXZ
			|| layer.mappingMode == kTerrainMappingModeTriplanar);
}

float2 TerrainRotateCanonicalMetres(float2 value, float2 inverseRotationCosSin)
{
	return float2(
		inverseRotationCosSin.x * value.x - inverseRotationCosSin.y * value.y,
		inverseRotationCosSin.y * value.x + inverseRotationCosSin.x * value.y);
}

float2 TerrainControlUvFromLocalSample(
	float2 localSample,
	TerrainAreaMappingConstants area)
{
	const float extent = max((float)area.controlExtentSamples, 1.0f);
	return localSample / extent;
}

float2 TerrainLocalSampleFromControlUv(
	float2 controlUv,
	TerrainAreaMappingConstants area)
{
	return controlUv * (float)max(area.controlExtentSamples, 1u);
}

uint2 TerrainControlTexelFromUv(float2 controlUv, uint2 dimensions)
{
	const uint2 lastTexel = max(dimensions, 1u) - 1u;
	return min(uint2(floor(saturate(controlUv) * (float2)dimensions)), lastTexel);
}

float2 TerrainSurfaceUvFromAreaLocalSample(
	float2 areaLocalSample,
	TerrainAreaMappingConstants area,
	TerrainLayerMappingConstants layer)
{
	// phaseMetres already contains inverseRotate(areaOrigin) + authoredOffset,
	// reduced modulo tile size in CPU double precision. Only the bounded local
	// contribution is evaluated in float, so distant signed areas remain stable.
	const float2 localMetres = areaLocalSample * area.sampleSpacingMetres;
	const float2 rotatedLocalMetres = TerrainRotateCanonicalMetres(
		localMetres, layer.inverseRotationCosSin);
	return (rotatedLocalMetres + layer.phaseMetres) / layer.tileSizeMetres;
}

float2 TerrainSurfaceUvFromPageLocalSample(
	float2 pageLocalSample,
	TerrainAreaMappingConstants area,
	TerrainLayerMappingConstants layer)
{
	return TerrainSurfaceUvFromAreaLocalSample(
		area.localSampleOrigin + pageLocalSample, area, layer);
}

float2 TerrainDiagnosticSurfaceUv(
	float2 controlUv,
	TerrainAreaMappingConstants area,
	TerrainLayerMappingConstants layer)
{
	if(!TerrainAreaMappingIsValid(area) || !TerrainLayerMappingIsValid(layer))
		return controlUv;
	return TerrainSurfaceUvFromAreaLocalSample(
		TerrainLocalSampleFromControlUv(controlUv, area), area, layer);
}

float3 TerrainFiniteUnitNormal(float3 candidate, float3 geometricNormal)
{
	const float candidateLengthSquared = dot(candidate, candidate);
	if(all(isfinite(candidate)) && candidateLengthSquared > 1.0e-10f)
		return candidate * rsqrt(candidateLengthSquared);
	const float geometricLengthSquared = dot(geometricNormal, geometricNormal);
	return all(isfinite(geometricNormal)) && geometricLengthSquared > 1.0e-10f
		? geometricNormal * rsqrt(geometricLengthSquared) : float3(0.0f, 1.0f, 0.0f);
}

float3 TerrainUnpackNormal(float3 encoded, float scale)
{
	float3 tangentNormal = encoded * 2.0f - 1.0f;
	tangentNormal.xy *= max(scale, 0.0f);
	return TerrainFiniteUnitNormal(tangentNormal, float3(0.0f, 0.0f, 1.0f));
}

float3 TerrainPlanarNormalToCanonical(float3 tangentNormal, float3 geometricNormal)
{
	const float3 normal = TerrainFiniteUnitNormal(geometricNormal, float3(0.0f, 1.0f, 0.0f));
	float3 tangent = float3(1.0f, 0.0f, 0.0f) - normal * normal.x;
	if(dot(tangent, tangent) <= 1.0e-8f)
		tangent = float3(0.0f, 0.0f, 1.0f) - normal * normal.z;
	tangent = TerrainFiniteUnitNormal(tangent, float3(1.0f, 0.0f, 0.0f));
	const float3 bitangent = TerrainFiniteUnitNormal(cross(normal, tangent), float3(0.0f, 0.0f, 1.0f));
	return TerrainFiniteUnitNormal(
		tangent * tangentNormal.x + bitangent * tangentNormal.y + normal * tangentNormal.z,
		normal);
}

float3 TerrainTriplanarWeights(float3 geometricNormal, float sharpness)
{
	const float3 normal = TerrainFiniteUnitNormal(geometricNormal, float3(0.0f, 1.0f, 0.0f));
	const float3 powered = pow(abs(normal), max(sharpness, 1.0f));
	const float sum = powered.x + powered.y + powered.z;
	return isfinite(sum) && sum > 1.0e-8f
		? powered / sum : float3(0.0f, 1.0f, 0.0f);
}

float3 TerrainProjectionNormalToCanonical(
	float3 tangentNormal, uint axis, float axisSign, float3 geometricNormal)
{
	// Each basis is right-handed: tangent cross bitangent equals the signed
	// projection normal. Sign-aware tangents prevent mirrored negative-axis seams.
	float3 tangent;
	float3 bitangent;
	float3 projectionNormal;
	if(axis == 0u)
	{
		tangent = float3(0.0f, 0.0f, -axisSign);
		bitangent = float3(0.0f, 1.0f, 0.0f);
		projectionNormal = float3(axisSign, 0.0f, 0.0f);
	}
	else if(axis == 1u)
	{
		tangent = float3(1.0f, 0.0f, 0.0f);
		bitangent = float3(0.0f, 0.0f, -axisSign);
		projectionNormal = float3(0.0f, axisSign, 0.0f);
	}
	else
	{
		tangent = float3(axisSign, 0.0f, 0.0f);
		bitangent = float3(0.0f, 1.0f, 0.0f);
		projectionNormal = float3(0.0f, 0.0f, axisSign);
	}
	return TerrainFiniteUnitNormal(
		tangent * tangentNormal.x + bitangent * tangentNormal.y
			+ projectionNormal * tangentNormal.z,
		geometricNormal);
}

float4 TerrainSampleOrNeutral(uint textureIndex, float2 uv, float4 neutralValue)
{
	if(textureIndex == kTerrainNeutralTextureIndex || textureIndex >= 64u)
		return neutralValue;
	const float4 value = terrainMaterialTextures[textureIndex].Sample(
		terrainMaterialSamplers[textureIndex], uv);
	return all(isfinite(value)) ? value : neutralValue;
}

TerrainPbrSample ApplyTerrainLayerFactors(
	TerrainPbrSample sample, TerrainGpuLayerBinding layer)
{
	sample.baseColor *= layer.baseColorFactor;
	sample.emissive *= layer.emissiveFactor.rgb;
	sample.ambientOcclusion = saturate(lerp(1.0f, sample.ambientOcclusion, layer.occlusionStrength));
	sample.roughness = saturate(sample.roughness * layer.roughnessFactor);
	sample.metallic = saturate(sample.metallic * layer.metallicFactor);
	return sample;
}

TerrainPbrSample TerrainSampleLayerPlanar(
	TerrainGpuLayerBinding layer, float2 surfaceUv, float3 geometricNormal)
{
	TerrainPbrSample sample;
	sample.baseColor = TerrainSampleOrNeutral(
		layer.baseColorTextureIndex, surfaceUv, TerrainNeutralBaseColor);
	const float3 encodedNormal = TerrainSampleOrNeutral(
		layer.normalTextureIndex, surfaceUv, float4(TerrainNeutralNormal, 1.0f)).rgb;
	sample.normal = TerrainPlanarNormalToCanonical(
		TerrainUnpackNormal(encodedNormal, layer.normalScale), geometricNormal);
	const float3 orm = TerrainSampleOrNeutral(
		layer.ormTextureIndex, surfaceUv, float4(TerrainNeutralOrm, 1.0f)).rgb;
	sample.ambientOcclusion = orm.r;
	sample.roughness = orm.g;
	sample.metallic = orm.b;
	sample.emissive = TerrainSampleOrNeutral(
		layer.emissiveTextureIndex, surfaceUv, float4(TerrainNeutralEmissive, 1.0f)).rgb;
	sample.valid = all(isfinite(sample.baseColor)) && all(isfinite(sample.normal))
		&& all(isfinite(orm)) && all(isfinite(sample.emissive));
	return ApplyTerrainLayerFactors(sample, layer);
}

TerrainPbrSample TerrainSampleLayerTriplanar(
	TerrainGpuLayerBinding layer, float3 mappedMetres, float3 geometricNormal)
{
	const float3 normal = TerrainFiniteUnitNormal(geometricNormal, float3(0.0f, 1.0f, 0.0f));
	const float3 weights = TerrainTriplanarWeights(normal, layer.triplanarSharpness);
	const float3 signs = float3(normal.x < 0.0f ? -1.0f : 1.0f,
		normal.y < 0.0f ? -1.0f : 1.0f, normal.z < 0.0f ? -1.0f : 1.0f);
	const float2 uvX = float2(-signs.x * mappedMetres.z, mappedMetres.y) / layer.tileSizeMetres.y;
	const float2 uvY = float2(mappedMetres.x, -signs.y * mappedMetres.z) / layer.tileSizeMetres;
	const float2 uvZ = float2(signs.z * mappedMetres.x, mappedMetres.y) / layer.tileSizeMetres.x;
	const float2 projectionUv[3] = {uvX, uvY, uvZ};
	TerrainPbrSample projections[3];
	[unroll]
	for(uint axis = 0u; axis < 3u; ++axis)
	{
		const float2 uv = projectionUv[axis];
		projections[axis].baseColor = TerrainSampleOrNeutral(
			layer.baseColorTextureIndex, uv, TerrainNeutralBaseColor);
		const float3 encodedNormal = TerrainSampleOrNeutral(
			layer.normalTextureIndex, uv, float4(TerrainNeutralNormal, 1.0f)).rgb;
		projections[axis].normal = TerrainProjectionNormalToCanonical(
			TerrainUnpackNormal(encodedNormal, layer.normalScale), axis, signs[axis], normal);
		const float3 orm = TerrainSampleOrNeutral(
			layer.ormTextureIndex, uv, float4(TerrainNeutralOrm, 1.0f)).rgb;
		projections[axis].ambientOcclusion = orm.r;
		projections[axis].roughness = orm.g;
		projections[axis].metallic = orm.b;
		projections[axis].emissive = TerrainSampleOrNeutral(
			layer.emissiveTextureIndex, uv, float4(TerrainNeutralEmissive, 1.0f)).rgb;
		projections[axis].valid = true;
	}
	TerrainPbrSample sample;
	sample.baseColor = projections[0].baseColor * weights.x
		+ projections[1].baseColor * weights.y + projections[2].baseColor * weights.z;
	sample.normal = TerrainFiniteUnitNormal(projections[0].normal * weights.x
		+ projections[1].normal * weights.y + projections[2].normal * weights.z, normal);
	sample.ambientOcclusion = projections[0].ambientOcclusion * weights.x
		+ projections[1].ambientOcclusion * weights.y + projections[2].ambientOcclusion * weights.z;
	sample.roughness = projections[0].roughness * weights.x
		+ projections[1].roughness * weights.y + projections[2].roughness * weights.z;
	sample.metallic = projections[0].metallic * weights.x
		+ projections[1].metallic * weights.y + projections[2].metallic * weights.z;
	sample.emissive = projections[0].emissive * weights.x
		+ projections[1].emissive * weights.y + projections[2].emissive * weights.z;
	sample.valid = true;
	return ApplyTerrainLayerFactors(sample, layer);
}

TerrainPbrSample BlendTerrainContributors(
	TerrainPbrSample contributors[4], float4 controlWeights, float3 geometricNormal)
{
	TerrainPbrSample blended;
	blended.baseColor = 0.0f;
	blended.normal = 0.0f;
	blended.ambientOcclusion = 0.0f;
	blended.roughness = 0.0f;
	blended.metallic = 0.0f;
	blended.emissive = 0.0f;
	blended.valid = false;
	float accumulatedWeight = 0.0f;
	[unroll]
	for(uint contributor = 0u; contributor < 4u; ++contributor)
	{
		const float weight = max(controlWeights[contributor], 0.0f);
		if(weight <= 0.0001f || !contributors[contributor].valid) continue;
		blended.baseColor += contributors[contributor].baseColor * weight;
		blended.normal += contributors[contributor].normal * weight;
		blended.ambientOcclusion += contributors[contributor].ambientOcclusion * weight;
		blended.roughness += contributors[contributor].roughness * weight;
		blended.metallic += contributors[contributor].metallic * weight;
		blended.emissive += contributors[contributor].emissive * weight;
		accumulatedWeight += weight;
	}
	if(accumulatedWeight <= 0.0001f) return blended;
	const float inverseWeight = rcp(accumulatedWeight);
	blended.baseColor *= inverseWeight;
	blended.normal = normalize(blended.normal * inverseWeight);
	blended.normal = TerrainFiniteUnitNormal(blended.normal, geometricNormal);
	blended.ambientOcclusion *= inverseWeight;
	blended.roughness *= inverseWeight;
	blended.metallic *= inverseWeight;
	blended.emissive *= inverseWeight;
	blended.valid = true;
	return blended;
}

#endif
