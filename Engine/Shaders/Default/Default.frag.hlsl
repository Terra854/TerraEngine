[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(0, 0)]] SamplerState globalSamplers[];

struct MaterialData
{
	float4 BaseColorFactor;
	float4 EmissiveFactor;
	float4 UVTransform;
	float MetallicFactor;
	float RoughnessFactor;
	float NormalScale;
	float OcclusionStrength;
	float AlphaCutoff;
	uint AlphaMode;
	uint ShadingModel;
	uint Flags;
	uint BaseColorTextureIndex;
	uint NormalTextureIndex;
	uint MaskTextureIndex;
	uint OcclusionTextureIndex;
	uint EmissiveTextureIndex;
	uint ShadowBehavior;
};

static const uint kShadingModel_Unlit = 0u;
static const uint kShadingModel_MetallicRoughness = 1u;
static const uint kShadingModel_SpecularGlossiness = 2u;
static const uint kMaterialShadowBehavior_Opaque = 0u;
static const uint kMaterialShadowBehavior_AlphaCutout = 1u;
static const uint kMaterialShadowBehavior_TransparentNoShadow = 2u;
static const uint kMaterialShadowBehavior_TranslucentApproximate = 3u;
static const uint kMaterialShadowBehavior_EmissiveUsesOpacity = 4u;

struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	column_major float4x4 viewMatrix;
	column_major float4x4 invProjection;
	uint4 renderSettings0; // x: shadowRes, y: shadowFilter, z: retainedLightBudget, w: enableLTC
	uint4 renderSettings1; // x: enableIBL, y: aniso, z: shadowLightBudget, w: uploadedLightBudget
	float4 renderSettings2; // x: indirectStrength, y: aoStrength, z: aoQuality, w: hiZPyramidAvailable
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};

struct GPULight
{
	float4 positionAndType;     // xyz: position, w: type
	float4 directionAndRange;   // xyz: direction, w: range
	float4 colorAndIntensity;   // xyz: color, w: intensity
	float4 coneAnglesAndArea;   // x: innerCos, y: outerCos, z: width, w: height
	float4 shadowAndPadding;    // x: castsShadows, y: shadow texture/point slot, z: ltcAmp, w: ltcInv
	float4 shadowRegion;        // xy: atlas UV offset, zw: atlas UV scale for managed 2D shadow regions
	float4 extraParams;         // x: shadowStrength, y: shadowBias, z: indirectMultiplier, w: authoredPriority
	float4 shadowMatrixC0;      // Column 0 of the CPU light view-projection matrix
	float4 shadowMatrixC1;      // Column 1; keeping columns avoids transposing GLM data
	float4 shadowMatrixC2;      // Column 2; used by directional/spot/area shadow sampling
	float4 shadowMatrixC3;      // Column 3; point shadows still use cubemap face depth
};

struct ClusterData
{
	uint offset;
	uint count;
	uint2 padding;
	float3 minBounds;
	float minBoundsPadding;
	float3 maxBounds;
	float maxBoundsPadding;
};

[[vk::binding(1, 0)]] StructuredBuffer<MaterialData> allMaterials;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> globalLights;
[[vk::binding(5, 0)]] StructuredBuffer<ClusterData> clusterGrid;
[[vk::binding(6, 0)]] StructuredBuffer<uint> lightIndices;
[[vk::binding(8, 0)]] SamplerState pointShadowSampler;
[[vk::binding(9, 0)]] StructuredBuffer<uint> globalBuffers[];
[[vk::binding(19, 0)]] Texture2D<float> ambientOcclusionTexture;
[[vk::binding(20, 0)]] TextureCube pointShadowCubes[32];

#pragma pack_matrix(column_major)
#include "../Geometry/TerraGeometryContract.hlsli"

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint materialIndex;
	uint meshDataIndex;
	uint textureIndex;
	uint vrsActive;
	uint taskCullingEnabled;
	uint statsSlotIndex;
	float cullingEpsilon;
	uint lightDataOffset;
	uint clusterDataOffset;
	uint lightIndexOffset;
	uint indirectDrawOffset;
	uint virtualMeshletOffset;
	uint virtualMeshletCount;
	uint selectedVirtualLod;
	uint virtualGeometryFlags;
	uint selectedVirtualClusterOffset;
	uint selectedVirtualClusterCount;
	uint selectedVirtualPageNeedOffset;
	uint selectedVirtualPageNeedCount;
	uint virtualGeometryPacketBufferIndex;
	uint virtualGeometryBatchBufferIndex;
	uint virtualGeometryPagePayloadBufferIndex; // Mirrors C++ PushConstants even though fragment shading currently only needs common material/debug fields.
	uint virtualGeometryPayloadSource;
	uint virtualGeometryEmittedSource;
	uint virtualGeometryPagePayloadByteOffset;
	uint virtualGeometryPagePayloadByteSize;
	uint virtualGeometryPacketDescriptorIndex;
	float4 editorTint;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

// The mesh stage and every compatible authored surface shader consume one
// installed geometry contract for static and animated page output.
#define PSIn TerraSurfaceInput

static const uint kLightTypeDirectional = 0u;
static const uint kLightTypePoint = 1u;
static const uint kLightTypeSpot = 2u;
static const uint kLightTypeAreaRect = 3u;
static const uint kMaxStatsSlots = 16u;
static const uint kMaxShadowResourceSlots = 32u;
static const float kPi = 3.14159265f;
static const float kDirectionalShadowNear = 0.1f;
static const float kDirectionalShadowFar = 180.0f;
static const float kDirectionalShadowExtent = 40.0f;
static const float kPerspectiveShadowNear = 0.1f;
static const float kDirectionalShadowBias = 0.0015f;
static const float kSpotShadowBias = 0.0020f;
static const float kPointShadowBias = 0.0050f;
static const float kAreaShadowBias = 0.0020f;
// Microfacet BRDFs become undefined at exactly-zero roughness because the
// normal-distribution term can evaluate 0 / 0. Keep a tiny floor so polished
// materials still look sharp without producing optimized-build NaNs.
static const float kMinimumPerceptualRoughness = 0.045f;
static const uint kMaxBindlessTextureCount = 1024u;
// Match the sky system rule: once the sun is roughly ten degrees below the
// horizon, direct sunlight should no longer illuminate scene geometry.
static const float kDirectionalLightNightHeight = -0.17364818f;
static const uint kMeshletCullFlagFrustum = 1u;
static const uint kMeshletCullFlagCone = 2u;
static const uint kMeshletCullFlagUnavailable = 4u;
static const uint kVirtualGeometryDebugModeLOD = 1u;
static const uint kVirtualGeometryDebugModeMeshletId = 2u;
static const uint kVirtualGeometryDebugModeBounds = 3u;
static const uint kVirtualGeometryDebugModeCulling = 4u;
static const uint kVirtualGeometryDebugModeHiZ = 5u;
static const uint kVirtualGeometryDebugModeResidency = 6u;
static const uint kVirtualGeometryDebugModeError = 7u;
static const uint kVirtualGeometryDebugModeCluster = 8u;
// Point lights with zero height relative to a surface have almost no Lambert
// term on grazing faces. This small wrapped term approximates nearby bounce and
// finite light size until full global illumination/area-light sampling exists.
static const float kLocalIndirectWrap = 0.55f;
static const float kLocalIndirectStrength = 0.24f;
static const float kLocalIndirectMaxMultiplier = 16.0f;
float3 SafeNormalize(float3 value, float3 fallback)
{
	const float lenSq = dot(value, value);
	if (lenSq <= 1e-8f || !all(isfinite(value)))
	{
		return fallback;
	}
	return value * rsqrt(lenSq);
}

static const float kLocalIndirectBounceFloor = 0.12f;

float3 BuildLightingNormal(float3 vertexNormal, float3 worldPosition, float2 uv, float3 sampledNormal, float normalScale, float4 bakedTangent)
{
	const float3 n = SafeNormalize(vertexNormal, float3(0.0f, 1.0f, 0.0f));
	const float3 fallbackTangentSeed = abs(n.y) < 0.999f
		? float3(0.0f, 1.0f, 0.0f)
		: float3(1.0f, 0.0f, 0.0f);
	const float3 fallbackTangent = SafeNormalize(cross(fallbackTangentSeed, n), float3(1.0f, 0.0f, 0.0f));
	const float3 orthogonalTangent = bakedTangent.xyz - n * dot(bakedTangent.xyz, n);
	const float3 t = SafeNormalize(orthogonalTangent, fallbackTangent);
	const float tangentSign = bakedTangent.w < 0.0f ? -1.0f : 1.0f;
	const float3 b = SafeNormalize(cross(n, t) * tangentSign, SafeNormalize(cross(n, fallbackTangent), float3(0.0f, 0.0f, 1.0f)));
	const float3x3 tbn = float3x3(t, b, n);

	// Some generated or imported meshes do not have a valid tangent basis. A
	// release shader that normalizes a zero or parallel tangent can produce NaN
	// lighting, which shows up as optimized/export-only speckled pixels. Fall
	// back to the geometric normal whenever the sampled normal or TBN output is
	// not finite.
	float3 unpackedNormal = sampledNormal * 2.0f - 1.0f;
	if (!all(isfinite(unpackedNormal)) || dot(unpackedNormal, unpackedNormal) <= 1e-8f)
	{
		unpackedNormal = float3(0.0f, 0.0f, 1.0f);
	}
	const float3 scaledNormal = SafeNormalize(lerp(float3(0.0f, 0.0f, 1.0f), unpackedNormal, saturate(normalScale)), float3(0.0f, 0.0f, 1.0f));
	return SafeNormalize(mul(scaledNormal, tbn), n);
}

uint ComputeClusterIndex(PSIn input, FrameCullingData frame)
{
	const uint3 gridDim = max(frame.clusterGridAndLightCount.xyz, uint3(1u, 1u, 1u));
	const float2 viewport = max(frame.clusterDepthAndViewport.zw, float2(1.0, 1.0));
	const uint x = min((uint)((input.position.x / viewport.x) * gridDim.x), gridDim.x - 1u);
	const uint y = min((uint)((input.position.y / viewport.y) * gridDim.y), gridDim.y - 1u);
	const uint z = min((uint)(saturate(input.position.z) * gridDim.z), gridDim.z - 1u);
	return x + y * gridDim.x + z * (gridDim.x * gridDim.y);
}

float ComputePointAttenuation(GPULight light, float3 worldPosition)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float distanceSq = dot(toLight, toLight);
	const float range = max(light.directionAndRange.w, 0.001f);
	const float rangeSq = range * range;

	// Inverse square law
	const float attenuation = 1.0f / (distanceSq + 0.0001f);

	// Smooth range windowing (Epic Games approach)
	const float distanceOverRangeSq = distanceSq / rangeSq;
	const float windowFactor = saturate(1.0f - (distanceOverRangeSq * distanceOverRangeSq));

	return attenuation * (windowFactor * windowFactor);
}

float SampleShadowPCF(Texture2D shadowMap, SamplerState shadowSampler, float2 uv, float depthNdc, float bias, uint filterSize, float2 texelSize)
{
	if (!all(isfinite(uv)) || !isfinite(depthNdc))
	{
		return 1.0f;
	}

	if (filterSize <= 1u)
	{
		const float shadowDepth = shadowMap.Sample(shadowSampler, uv).r;
		if (!isfinite(shadowDepth))
		{
			return 1.0f;
		}
		return (depthNdc - bias) <= shadowDepth ? 1.0f : 0.0f;
	}

	float visibility = 0.0f;
	float weight = 0.0f;
	int range = (int)(filterSize / 2u);

	for (int y = -range; y <= range; ++y)
	{
		for (int x = -range; x <= range; ++x)
		{
			const float2 offset = float2((float)x, (float)y) * texelSize;
			const float tapDepth = shadowMap.Sample(shadowSampler, uv + offset).r;
			if (!isfinite(tapDepth))
			{
				weight += 1.0f;
				visibility += 1.0f;
				continue;
			}
			visibility += (depthNdc - bias) <= tapDepth ? 1.0f : 0.0f;
			weight += 1.0f;
		}
	}
	return visibility / weight;
}

float ComputePerspectiveShadowDepth01(float forwardDistance, float farPlane)
{
	const float nearPlane = kPerspectiveShadowNear;
	const float safeFar = max(farPlane, nearPlane + 0.001f);
	const float safeDistance = max(forwardDistance, nearPlane);
	// Spot, point, and area shadow maps are rendered through hardware
	// perspective projections, so receivers must compare against the same
	// non-linear depth that the depth attachment stored.
	return saturate(safeFar / (safeFar - nearPlane) - (safeFar * nearPlane) / ((safeFar - nearPlane) * safeDistance));
}

float4 TransformShadowClip(GPULight light, float3 worldPosition)
{
	const float4 world = float4(worldPosition, 1.0f);
	// C++ uploads the exact GLM matrix columns that rendered the shadow map.
	// Multiplying by columns here keeps the receiver projection bit-for-bit in
	// the same convention as the shadow pass instead of rebuilding it manually.
	return light.shadowMatrixC0 * world.x
		+ light.shadowMatrixC1 * world.y
		+ light.shadowMatrixC2 * world.z
		+ light.shadowMatrixC3 * world.w;
}

float SampleProjectedShadowMap(Texture2D shadowMap, SamplerState shadowSampler, GPULight light, FrameCullingData frame, float3 worldPosition, float fallbackBias, float2 texelSize)
{
	const float4 shadowClip = TransformShadowClip(light, worldPosition);
	if (shadowClip.w <= 1e-5f)
	{
		return 1.0f;
	}

	const float3 shadowNdc = shadowClip.xyz / shadowClip.w;
	if (shadowNdc.z < 0.0f || shadowNdc.z > 1.0f)
	{
		return 1.0f;
	}

	float2 uv = shadowNdc.xy * 0.5f + 0.5f;
	if (any(uv < float2(0.0f, 0.0f)) || any(uv > float2(1.0f, 1.0f)))
	{
		return 1.0f;
	}
	const float2 regionScale = max(light.shadowRegion.zw, float2(0.0001f, 0.0001f));
	uv = light.shadowRegion.xy + uv * regionScale;

	const float authoredBias = max(light.extraParams.y, 0.0f);
	return SampleShadowPCF(shadowMap, shadowSampler, uv, shadowNdc.z, max(fallbackBias, authoredBias), frame.renderSettings0.y, texelSize);
}

float SampleDirectionalShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(min(shadowTextureIndex, kMaxBindlessTextureCount - 1u));
	const float res = (float)frame.renderSettings0.x;
	const float2 texelSize = 1.0f / max(res, 512.0f);
	return SampleProjectedShadowMap(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], light, frame, worldPosition, kDirectionalShadowBias, texelSize);
}

float SampleSpotShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(min(shadowTextureIndex, kMaxBindlessTextureCount - 1u));
	const float res = (float)frame.renderSettings0.x;
	const float2 texelSize = 1.0f / max(res, 512.0f);
	return SampleProjectedShadowMap(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], light, frame, worldPosition, kSpotShadowBias, texelSize);
}

float SamplePointShadow(GPULight light, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const float3 toPoint = worldPosition - light.positionAndType.xyz;
	const float3 sampleDir = SafeNormalize(toPoint, float3(0.0, 0.0, 1.0));
	const uint shadowSlot = min((uint)(light.shadowAndPadding.y + 0.5f), kMaxShadowResourceSlots - 1u);
	const uint nonUniformSlot = NonUniformResourceIndex(shadowSlot);
	const float sampledDepth = pointShadowCubes[nonUniformSlot].Sample(pointShadowSampler, sampleDir).r;
	if (!isfinite(sampledDepth))
	{
		return 1.0f;
	}
	const float faceForwardDistance = max(max(abs(toPoint.x), abs(toPoint.y)), abs(toPoint.z));
	const float range = max(light.directionAndRange.w, 0.1f);
	const float depthNdc = ComputePerspectiveShadowDepth01(faceForwardDistance, range);
	const float authoredBias = max(light.extraParams.y, 0.0f);
	return (depthNdc - max(kPointShadowBias, authoredBias)) <= sampledDepth ? 1.0f : 0.0f;
}

float SampleAreaShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(min(shadowTextureIndex, kMaxBindlessTextureCount - 1u));
	const float range = max(light.directionAndRange.w, 0.1f);
	const float width = max(light.coneAnglesAndArea.z, 0.1f);
	const float height = max(light.coneAnglesAndArea.w, 0.1f);
	const float res = (float)frame.renderSettings0.x;
	const float2 texel = 1.0f / max(res, 512.0f);
	const float kernelScale = saturate(max(width, height) / max(range, 0.1f)) * 2.0f;
	return SampleProjectedShadowMap(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], light, frame, worldPosition, kAreaShadowBias, texel * kernelScale);
}

float ComputeShadowFactor(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	// Manual debug switch for shadow isolation. It defaults off so runtime
	// builds sample the real shadow maps, while a future release-only issue can
	// still force fully open visibility without changing CPU shadow submission.
	static const bool kForceOpenShadowsForArtifactIsolation = false;
	if (kForceOpenShadowsForArtifactIsolation)
	{
		return 1.0f;
	}

	const uint lightType = (uint)(light.positionAndType.w + 0.5);
	float rawVisibility = 1.0f;
	if (lightType == kLightTypeDirectional)
	{
		rawVisibility = SampleDirectionalShadow(light, frame, worldPosition);
	}
	else if (lightType == kLightTypePoint)
	{
		rawVisibility = SamplePointShadow(light, worldPosition);
	}
	else if (lightType == kLightTypeSpot)
	{
		rawVisibility = SampleSpotShadow(light, frame, worldPosition);
	}
	else if (lightType == kLightTypeAreaRect)
	{
		rawVisibility = SampleAreaShadow(light, frame, worldPosition);
	}

	// LightComponent::shadowStrength is packed in extraParams.x. A strength of
	// zero intentionally disables darkening while preserving the shadow-map
	// assignment for diagnostics; one applies the sampled visibility directly.
	return lerp(1.0f, rawVisibility, saturate(light.extraParams.x));
}

float D_GGX(float nDotH, float roughness)
{
	roughness = max(roughness, kMinimumPerceptualRoughness);
	float a = roughness * roughness;
	float a2 = a * a;
	float d = (nDotH * a2 - nDotH) * nDotH + 1.0;
	return a2 / max(kPi * d * d, 1e-8f);
}

float V_SmithJointSchlick(float nDotL, float nDotV, float roughness)
{
	roughness = max(roughness, kMinimumPerceptualRoughness);
	float a = roughness * roughness;
	float ggxV = nDotL * (nDotV * (1.0 - a) + a);
	float ggxL = nDotV * (nDotL * (1.0 - a) + a);
	return 0.5 / (ggxV + ggxL + 0.00001); // 0.5 is already combined with the 1/4 factor of Cook-Torrance denominator
}

float3 F_Schlick(float cosTheta, float3 f0)
{
	float f = pow(1.0 - cosTheta, 5.0);
	return f0 + (1.0 - f0) * f;
}

float3 EvaluateLocalIndirectDiffuse(GPULight light, FrameCullingData frame, float rawNDotL, float attenuation, float3 albedo, float metallic, float roughness)
{
	// Artists can deliberately push this above 1 for editor/debug setups with
	// low lights. Clamp only the upper bound so accidental huge values do not
	// flood the scene with distance-attenuated fill.
	const float indirectMultiplier = clamp(light.extraParams.z, 0.0f, kLocalIndirectMaxMultiplier);
	const float globalIndirectStrength = saturate(frame.renderSettings2.x / 8.0f) * 8.0f;
	if (indirectMultiplier <= 0.0f || globalIndirectStrength <= 0.0f || attenuation <= 0.0f)
	{
		return float3(0.0f, 0.0f, 0.0f);
	}

	// The wrapped and broad terms cover grazing faces. The floor represents
	// first-bounce local spill, so faces turned fully away from a nearby low
	// light are not pitch black while they remain inside the light radius.
	const float wrappedNoL = saturate((rawNDotL + kLocalIndirectWrap) / (1.0f + kLocalIndirectWrap));
	const float broadFill = 0.22f * saturate(1.0f + rawNDotL);
	const float bounceVisibility = max(max(wrappedNoL, broadFill), kLocalIndirectBounceFloor);
	const float diffuseMaterialWeight = (1.0f - saturate(metallic)) * lerp(0.55f, 1.0f, saturate(roughness));
	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	const float3 diffuse = albedo / kPi;
	return diffuse * radiance * attenuation * bounceVisibility * diffuseMaterialWeight * indirectMultiplier * globalIndirectStrength * kLocalIndirectStrength;
}

float ComputeDirectionalLightVisibility(float3 lightDir)
{
	// The directional light points from the surface toward the sun. When its Y
	// component falls below the threshold, the sun is far enough under the
	// horizon that direct sunlight should be fully gone.
	return smoothstep(kDirectionalLightNightHeight, 0.0f, lightDir.y);
}

float3 EvaluateDirectionalLight(GPULight light, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 lightDir = SafeNormalize(-light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float lightVisibility = ComputeDirectionalLightVisibility(lightDir);
	if (lightVisibility <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

	const float nDotL = saturate(dot(normal, lightDir));
	if (nDotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

	const float3 halfVector = SafeNormalize(lightDir + viewDir, lightDir);
	const float nDotV = saturate(dot(normal, viewDir)) + 1e-5f;
	const float nDotH = saturate(dot(normal, halfVector));
	const float vDotH = saturate(dot(viewDir, halfVector));

	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, saturate(metallic));
	const float3 F = F_Schlick(vDotH, f0);
	const float D = D_GGX(nDotH, roughness);
	const float V = V_SmithJointSchlick(nDotL, nDotV, roughness);

	const float3 specular = D * V * F;
	const float3 kD = (1.0 - F) * (1.0 - saturate(metallic));
	const float3 diffuse = kD * albedo / kPi;

	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	return (diffuse + specular) * radiance * nDotL * lightVisibility;
}

float3 EvaluatePointLight(GPULight light, float3 worldPosition, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float rawNDotL = dot(normal, lightDir);
	const float attenuation = ComputePointAttenuation(light, worldPosition);
	if (attenuation <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

	const float nDotL = saturate(rawNDotL);
	if (nDotL <= 0.0f) return float3(0.0f, 0.0f, 0.0f);

	const float3 halfVector = SafeNormalize(lightDir + viewDir, lightDir);
	const float nDotV = saturate(dot(normal, viewDir)) + 1e-5f;
	const float nDotH = saturate(dot(normal, halfVector));
	const float vDotH = saturate(dot(viewDir, halfVector));

	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, saturate(metallic));
	const float3 F = F_Schlick(vDotH, f0);
	const float D = D_GGX(nDotH, roughness);
	const float V = V_SmithJointSchlick(nDotL, nDotV, roughness);

	const float3 specular = D * V * F;
	const float3 kD = (1.0 - F) * (1.0 - saturate(metallic));
	const float3 diffuse = kD * albedo / kPi;

	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	return (diffuse + specular) * radiance * nDotL * attenuation;
}

float3 EvaluatePointLightIndirect(GPULight light, FrameCullingData frame, float3 worldPosition, float3 normal, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float rawNDotL = dot(normal, lightDir);
	const float attenuation = ComputePointAttenuation(light, worldPosition);
	return EvaluateLocalIndirectDiffuse(light, frame, rawNDotL, attenuation, albedo, metallic, roughness);
}

float3 EvaluateSpotLight(GPULight light, float3 worldPosition, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float3 spotDir = SafeNormalize(-light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float cosTheta = dot(lightDir, spotDir);
	const float innerCos = light.coneAnglesAndArea.x;
	const float outerCos = light.coneAnglesAndArea.y;
	const float coneAttenuation = saturate((cosTheta - outerCos) / max(innerCos - outerCos, 0.0001));
	return EvaluatePointLight(light, worldPosition, normal, viewDir, albedo, metallic, roughness) * coneAttenuation;
}

float3 EvaluateSpotLightIndirect(GPULight light, FrameCullingData frame, float3 worldPosition, float3 normal, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float3 spotDir = SafeNormalize(-light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float cosTheta = dot(lightDir, spotDir);
	const float innerCos = light.coneAnglesAndArea.x;
	const float outerCos = light.coneAnglesAndArea.y;
	const float coneAttenuation = saturate((cosTheta - outerCos) / max(innerCos - outerCos, 0.0001));
	return EvaluatePointLightIndirect(light, frame, worldPosition, normal, albedo, metallic, roughness) * coneAttenuation;
}

float3 EvaluateAreaLightLTC(GPULight light, float3 worldPosition, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float distanceAttenuation = ComputePointAttenuation(light, worldPosition);
	const float nDotL = saturate(dot(normal, SafeNormalize(toLight, float3(0.0, -1.0, 0.0))));
	const float areaScale = max(light.coneAnglesAndArea.z * light.coneAnglesAndArea.w, 0.001);
	const uint amplitudeLutIndex = (uint)(light.shadowAndPadding.z + 0.5f);
	const uint inverseLutIndex = (uint)(light.shadowAndPadding.w + 0.5f);
	const uint ampResource = NonUniformResourceIndex(amplitudeLutIndex);
	const uint invResource = NonUniformResourceIndex(inverseLutIndex);
	const float2 lutUv = float2(saturate(roughness), saturate(1.0f - dot(normal, viewDir)));
	const float4 amplitudeLut = globalTextures[ampResource].Sample(globalSamplers[ampResource], lutUv);
	const float4 inverseLut = globalTextures[invResource].Sample(globalSamplers[invResource], lutUv);
	const float ltcDiffuseScale = max(amplitudeLut.x, 0.05f);
	const float ltcSpecularScale = max(amplitudeLut.y, 0.05f);
	const float3 warpedNormal = SafeNormalize(float3(
		normal.x * inverseLut.x + normal.y * inverseLut.y,
		normal.x * inverseLut.z + normal.y * inverseLut.w,
		normal.z), normal);
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float3 halfVector = SafeNormalize(lightDir + viewDir, lightDir);
	const float warpedNdotL = saturate(dot(warpedNormal, lightDir));
	const float specPower = lerp(96.0, 6.0, saturate(roughness));
	const float specularTerm = pow(saturate(dot(warpedNormal, halfVector)), specPower);
	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, saturate(metallic));
	const float3 diffuse = albedo * (1.0 - saturate(metallic)) / kPi;
	const float3 specular = f0 * specularTerm;
	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	return (diffuse * ltcDiffuseScale + specular * ltcSpecularScale) * radiance * warpedNdotL * distanceAttenuation * areaScale * 0.1;
}

float3 EvaluateSingleLight(uint lightIndex, PSIn input, FrameCullingData frame, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const GPULight light = globalLights[SceneUniforms.lightDataOffset + lightIndex];
	const uint lightType = (uint)(light.positionAndType.w + 0.5);
	const float shadowFactor = ComputeShadowFactor(light, frame, input.worldPosition);
	if (lightType == kLightTypeDirectional)
	{
		return EvaluateDirectionalLight(light, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
	}
	if (lightType == kLightTypePoint)
	{
		const float3 directLighting = EvaluatePointLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
		// The local indirect approximation comes from this same light, so it
		// must respect blocker visibility. Environment IBL remains separate.
		const float3 indirectLighting = EvaluatePointLightIndirect(light, frame, input.worldPosition, normal, albedo, metallic, roughness) * shadowFactor;
		return directLighting + indirectLighting;
	}
	if (lightType == kLightTypeSpot)
	{
		const float3 directLighting = EvaluateSpotLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
		// Spot bounce is still emitted by the local light, so occluders should
		// damp it together with the direct spotlight contribution.
		const float3 indirectLighting = EvaluateSpotLightIndirect(light, frame, input.worldPosition, normal, albedo, metallic, roughness) * shadowFactor;
		return directLighting + indirectLighting;
	}
	if (lightType == kLightTypeAreaRect)
	{
		if (frame.renderSettings0.w > 0u)
		{
			return EvaluateAreaLightLTC(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
		}
		else
		{
			// Fallback to point light if LTC is disabled
			const float3 directLighting = EvaluatePointLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
			const float3 indirectLighting = EvaluatePointLightIndirect(light, frame, input.worldPosition, normal, albedo, metallic, roughness) * shadowFactor;
			return directLighting + indirectLighting;
		}
	}
	return float3(0.0, 0.0, 0.0);
}

float3 EvaluateIBL(FrameCullingData frame, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness, float ambientOcclusion)
{
	const float globalIndirectStrength = saturate(frame.renderSettings2.x / 8.0f) * 8.0f;
	if (globalIndirectStrength <= 0.0f)
	{
		return float3(0.0f, 0.0f, 0.0f);
	}

	const float skyVisibility = saturate(normal.y * 0.5f + 0.5f);
	const float3 horizon = float3(0.18f, 0.22f, 0.28f);
	const float3 zenith = float3(0.45f, 0.55f, 0.72f);
	const float3 ground = float3(0.06f, 0.055f, 0.05f);
	const float3 irradiance = lerp(ground, lerp(horizon, zenith, skyVisibility), skyVisibility);
	const float3 diffuse = albedo * (1.0f - saturate(metallic)) * irradiance / kPi;
	const float3 reflectDir = reflect(-viewDir, normal);
	const float reflectionWeight = pow(saturate(reflectDir.y * 0.5f + 0.5f), lerp(1.0f, 0.25f, saturate(roughness)));
	const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, saturate(metallic));
	const float3 specular = f0 * irradiance * reflectionWeight * (1.0f - 0.65f * saturate(roughness));
	return (diffuse + specular) * globalIndirectStrength * ambientOcclusion;
}

float SampleScreenAmbientOcclusion(PSIn input, FrameCullingData frame)
{
	const float2 viewport = max(frame.clusterDepthAndViewport.zw, float2(1.0f, 1.0f));
	const int2 maxPixel = int2(max(viewport, float2(1.0f, 1.0f))) - int2(1, 1);
	const int2 pixel = clamp(int2(input.position.xy), int2(0, 0), maxPixel);
	// The AO pass stores visibility, not darkness: 1 means fully open and
	// lower values mean nearby depth created contact occlusion.
	const float visibility = ambientOcclusionTexture.Load(uint3(uint2(pixel), 0));
	return isfinite(visibility) ? saturate(visibility) : 1.0f;
}

float3 BuildHeatmapColor(float normalizedLightCount)
{
	const float t = saturate(normalizedLightCount);
	return lerp(float3(0.0, 0.2, 1.0), float3(1.0, 0.2, 0.0), t);
}

float3 ApplyEditorTint(float3 color)
{
	return lerp(color, SceneUniforms.editorTint.rgb, saturate(SceneUniforms.editorTint.a));
}

float3 BuildVirtualGeometryHashColor(uint value)
{
	value ^= value >> 16u;
	value *= 0x7feb352du;
	value ^= value >> 15u;
	value *= 0x846ca68bu;
	value ^= value >> 16u;
	return float3(
		(float)(value & 255u) / 255.0f,
		(float)((value >> 8u) & 255u) / 255.0f,
		(float)((value >> 16u) & 255u) / 255.0f);
}

float3 BuildVirtualGeometryDebugColor(uint debugMode, PSIn input, FrameCullingData frame, uint stablePrimitiveId)
{
	const uint virtualActive = SceneUniforms.virtualGeometryFlags & 1u;
	const uint failOpen = (SceneUniforms.virtualGeometryFlags >> 1u) & 1u;
	const uint selectedLod = input.virtualGeometryDebugData & 0xFFu;
	const uint selectedClusterCount = (input.virtualGeometryDebugData >> 8u) & 0xFFu;
	const uint packedMissingPageCount = (input.virtualGeometryDebugData >> 16u) & 0xFFu;
	const uint clusterHashSeed = (input.virtualGeometryDebugData >> 24u) & 0xFFu;
	const uint missingPageCount = max(SceneUniforms.selectedVirtualPageNeedCount, packedMissingPageCount);
	const bool hiZTrusted = frame.renderSettings2.w > 0.5f;
	if (debugMode == kVirtualGeometryDebugModeLOD)
	{
		// LOD mode: every selected detail level receives a stable hash color.
		return BuildVirtualGeometryHashColor(selectedLod + 17u);
	}
	if (debugMode == kVirtualGeometryDebugModeMeshletId)
	{
		// Meshlet Id mode: dense patches show up as rapid per-meshlet color changes.
		return BuildVirtualGeometryHashColor(input.meshletIndex + 31u);
	}
	if (debugMode == kVirtualGeometryDebugModeBounds)
	{
		if ((input.meshletCullFlags & kMeshletCullFlagUnavailable) != 0u)
		{
			return float3(1.0f, 0.0f, 1.0f);
		}
		// Bounds mode: blue means small projected groups, yellow means large groups.
		const float radiusBand = saturate(log2(max(input.meshletBoundsRadius, 1e-5f) * 64.0f + 1.0f) / 8.0f);
		return lerp(float3(0.05f, 0.25f, 1.0f), float3(1.0f, 0.9f, 0.05f), radiusBand);
	}
	if (debugMode == kVirtualGeometryDebugModeCulling)
	{
		if ((input.meshletCullFlags & kMeshletCullFlagUnavailable) != 0u)
		{
			return float3(1.0f, 0.0f, 1.0f);
		}
		if ((input.meshletCullFlags & kMeshletCullFlagFrustum) != 0u)
		{
			return float3(1.0f, 0.0f, 0.15f);
		}
		if ((input.meshletCullFlags & kMeshletCullFlagCone) != 0u)
		{
			return float3(1.0f, 0.55f, 0.05f);
		}
		// Culling mode: visible meshlets are green, or cyan when Hi-Z is trusted.
		return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.1f, 0.85f, 0.25f);
	}
	if (debugMode == kVirtualGeometryDebugModeHiZ)
	{
		if (failOpen != 0u)
		{
			return float3(1.0f, 0.0f, 0.15f);
		}
		// Hi-Z mode: cyan means the current-view pyramid is available; gray means missing.
		return hiZTrusted ? float3(0.05f, 0.8f, 1.0f) : float3(0.35f, 0.35f, 0.35f);
	}
	if (debugMode == kVirtualGeometryDebugModeResidency)
	{
		if (virtualActive == 0u)
		{
			return float3(0.55f, 0.35f, 0.1f);
		}
		// Residency mode: orange highlights requested or missing high-detail pages.
		return missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.0f, 0.75f, 0.45f);
	}
	if (debugMode == kVirtualGeometryDebugModeError)
	{
		if (failOpen != 0u)
		{
			return float3(1.0f, 0.0f, 0.15f);
		}
		// Error mode: clean selections are cyan, while page pressure turns orange.
		return missingPageCount > 0u ? float3(1.0f, 0.55f, 0.05f) : float3(0.2f, 0.9f, 0.9f);
	}
	if (debugMode == kVirtualGeometryDebugModeCluster)
	{
		// Cluster mode: selected cluster identity and the authored primitive order
		// drive a stable hash color without requiring a second live color target.
		const uint clusterSeed = clusterHashSeed + selectedClusterCount * 101u + stablePrimitiveId;
		return BuildVirtualGeometryHashColor(clusterSeed + 73u);
	}
	return float3(0.0f, 0.0f, 0.0f);
}

// Conventional Scene/Game/capture pipelines compile this explicit Vulkan-only
// variant when their render pass owns the matching integer identity target.
// Shadow pipelines deliberately omit the define and retain their one-target
// ABI, so a shadow pass can never bind an unpaired visibility attachment.
#if defined(TERRAENGINE_VULKAN_CONVENTIONAL_VISIBILITY_IDENTITY)
struct ConventionalVisibilityOutput
{
	float4 color : SV_Target0;
	// These lanes identify the exact fragment producer; they are typed integer
	// attachment data, never a debug-color encoding that presentation may alter.
	uint4 identity : SV_Target1;
};

ConventionalVisibilityOutput MakeConventionalVisibilityOutput(float4 color, uint stablePrimitiveId)
{
	ConventionalVisibilityOutput output;
	output.color = color;
	output.identity = uint4(
		stablePrimitiveId,
		SceneUniforms.meshDataIndex,
		SceneUniforms.virtualGeometryPacketDescriptorIndex,
		SceneUniforms.statsSlotIndex);
	return output;
}

#define TERRAENGINE_CONVENTIONAL_OUTPUT ConventionalVisibilityOutput
#define TERRAENGINE_CONVENTIONAL_SEMANTIC
#define TERRAENGINE_CONVENTIONAL_RETURN(colorValue, primitiveId) return MakeConventionalVisibilityOutput(colorValue, primitiveId)
#else
#define TERRAENGINE_CONVENTIONAL_OUTPUT float4
#define TERRAENGINE_CONVENTIONAL_SEMANTIC : SV_Target0
#define TERRAENGINE_CONVENTIONAL_RETURN(colorValue, primitiveId) return colorValue
#endif

TERRAENGINE_CONVENTIONAL_OUTPUT main(
	PSIn input,
	nointerpolation uint stablePrimitiveId : TEXCOORD6)
	TERRAENGINE_CONVENTIONAL_SEMANTIC
{
	const MaterialData mat = allMaterials[SceneUniforms.materialIndex];
	const float2 uv = input.uv * mat.UVTransform.xy + mat.UVTransform.zw;

	float4 baseColorSample = globalTextures[NonUniformResourceIndex(min(mat.BaseColorTextureIndex, kMaxBindlessTextureCount - 1u))].Sample(globalSamplers[NonUniformResourceIndex(min(mat.BaseColorTextureIndex, kMaxBindlessTextureCount - 1u))], uv);
	if (!all(isfinite(baseColorSample)))
	{
		baseColorSample = float4(1.0f, 1.0f, 1.0f, 1.0f);
	}
	const float4 finalColor = baseColorSample * mat.BaseColorFactor;
	if (mat.AlphaMode == 1u)
	{
		clip(finalColor.a - mat.AlphaCutoff);
	}
	if (SceneUniforms.vrsActive == 2u)
	{
		// Shadow-map rendering only needs alpha testing and a depth write. Return
		// before material lighting so the shadow pass does not sample the same
		// shadow maps it is currently updating.
		TERRAENGINE_CONVENTIONAL_RETURN(float4(0.0f, 0.0f, 0.0f, 1.0f), stablePrimitiveId);
	}

	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frame = frameCullingData[statsIndex];
	const uint virtualGeometryDebugMode = (SceneUniforms.virtualGeometryFlags >> 8u) & 0xFFu;
	if (virtualGeometryDebugMode != 0u)
	{
		// Debug visualization is allowed to render even when virtual geometry
		// has failed open or is disabled. That makes the overlay useful for
		// diagnosing why a mesh is still taking the conservative fallback path,
		// instead of silently looking identical to normal material rendering.
		TERRAENGINE_CONVENTIONAL_RETURN(float4(ApplyEditorTint(BuildVirtualGeometryDebugColor(virtualGeometryDebugMode, input, frame, stablePrimitiveId)), finalColor.a), stablePrimitiveId);
	}

	float3 sampledNormal = globalTextures[NonUniformResourceIndex(min(mat.NormalTextureIndex, kMaxBindlessTextureCount - 1u))].Sample(globalSamplers[NonUniformResourceIndex(min(mat.NormalTextureIndex, kMaxBindlessTextureCount - 1u))], uv).rgb;
	if (!all(isfinite(sampledNormal)))
	{
		sampledNormal = float3(0.5f, 0.5f, 1.0f);
	}
	float4 maskSample = globalTextures[NonUniformResourceIndex(min(mat.MaskTextureIndex, kMaxBindlessTextureCount - 1u))].Sample(globalSamplers[NonUniformResourceIndex(min(mat.MaskTextureIndex, kMaxBindlessTextureCount - 1u))], uv);
	if (!all(isfinite(maskSample)))
	{
		maskSample = float4(1.0f, 1.0f, 0.0f, 1.0f);
	}
	const float roughness = max(saturate(maskSample.g * mat.RoughnessFactor), kMinimumPerceptualRoughness);
	const float metallic = saturate(maskSample.b * mat.MetallicFactor);
	const float authoredOcclusion = lerp(1.0f, maskSample.r, saturate(mat.OcclusionStrength));
	float3 emissiveSample = globalTextures[NonUniformResourceIndex(min(mat.EmissiveTextureIndex, kMaxBindlessTextureCount - 1u))].Sample(globalSamplers[NonUniformResourceIndex(min(mat.EmissiveTextureIndex, kMaxBindlessTextureCount - 1u))], uv).rgb;
	if (!all(isfinite(emissiveSample)))
	{
		emissiveSample = float3(0.0f, 0.0f, 0.0f);
	}
	const float3 emissive = emissiveSample * mat.EmissiveFactor.rgb;

	if (mat.ShadingModel == kShadingModel_Unlit)
	{
		TERRAENGINE_CONVENTIONAL_RETURN(float4(ApplyEditorTint(finalColor.rgb + emissive), finalColor.a), stablePrimitiveId);
	}

	const uint lightCount = frame.clusterGridAndLightCount.w;
	const float3 normal = BuildLightingNormal(input.normal, input.worldPosition, uv, sampledNormal, mat.NormalScale, input.tangent);
	const float3 viewDir = SafeNormalize(frame.cameraPositionAndEpsilon.xyz - input.worldPosition, float3(0.0f, 0.0f, 1.0f));
	const uint clusterIndex = ComputeClusterIndex(input, frame);
	ClusterData cluster;
	cluster.offset = 0u;
	cluster.count = 0u;
	cluster.padding = uint2(0u, 0u);
	cluster.minBounds = float3(0.0f, 0.0f, 0.0f);
	cluster.minBoundsPadding = 0.0f;
	cluster.maxBounds = float3(0.0f, 0.0f, 0.0f);
	cluster.maxBoundsPadding = 0.0f;
	bool clusteredLightingAvailable = frame.clusteredLightingValid > 0u && frame.clusteredLightingOverflowed == 0u;
	// Clustered lighting is an optimization. Validate the computed range before
	// sampling the flat light-index buffer so any stale or partially written
	// cluster metadata falls back to the full light list instead of corrupting
	// isolated pixels.
	if (clusteredLightingAvailable)
	{
		cluster = clusterGrid[SceneUniforms.clusterDataOffset + clusterIndex];
		const uint3 clusterGridDim = max(frame.clusterGridAndLightCount.xyz, uint3(1u, 1u, 1u));
		const uint clusterCount = clusterGridDim.x * clusterGridDim.y * clusterGridDim.z;
		const uint clusterLightRangeLimit = clusterCount * lightCount;
		const uint remainingClusterLights = cluster.offset <= clusterLightRangeLimit
			? (clusterLightRangeLimit - cluster.offset)
			: 0u;
		const bool clusterRangeValid = cluster.count <= lightCount
			&& cluster.offset <= clusterLightRangeLimit
			&& cluster.count <= remainingClusterLights;
		clusteredLightingAvailable = clusterRangeValid;
	}

	float3 lighting = float3(0.0, 0.0, 0.0);
	uint evaluatedLightCount = 0u;

	if (clusteredLightingAvailable)
	{
		[loop]
		for (uint lane = 0u; lane < cluster.count; ++lane)
		{
			const uint lightIndex = lightIndices[SceneUniforms.lightIndexOffset + cluster.offset + lane];
			if (lightIndex == 0xFFFFFFFFu || lightIndex >= lightCount)
			{
				continue;
			}
			lighting += EvaluateSingleLight(lightIndex, input, frame, normal, viewDir, finalColor.rgb, metallic, roughness);
			evaluatedLightCount += 1u;
		}
	}

	// Cluster culling is an optimization, not a correctness boundary. If a
	// visible fragment ends up with an empty or invalid cluster light list while
	// the frame still has active retained lights, fall back to the shared light
	// list so lighting does not incorrectly snap to black.
	const bool needsGlobalLightFallback = (evaluatedLightCount == 0u) && (lightCount > 0u);
	if (!clusteredLightingAvailable || needsGlobalLightFallback)
	{
		if (needsGlobalLightFallback)
		{
			lighting = float3(0.0, 0.0, 0.0);
			evaluatedLightCount = 0u;
		}

		// Fallback keeps shading stable while the compute culling path warms up
		// or when clustered assignment misses a fragment that should still see a
		// retained light.
		[loop]
		for (uint lightIndex = 0u; lightIndex < lightCount; ++lightIndex)
		{
			lighting += EvaluateSingleLight(lightIndex, input, frame, normal, viewDir, finalColor.rgb, metallic, roughness);
			evaluatedLightCount += 1u;
		}
	}

	// Debug heatmap mode: set task-culling push constant to 2 from CPU tools.
	if (SceneUniforms.taskCullingEnabled > 1u)
	{
		const float normalizedCount = (float)evaluatedLightCount / max((float)lightCount, 1.0f);
		TERRAENGINE_CONVENTIONAL_RETURN(float4(BuildHeatmapColor(normalizedCount), 1.0), stablePrimitiveId);
	}

	float3 finalLighting = lighting;
	if (frame.renderSettings1.x > 0u)
	{
		const float aoStrength = saturate(frame.renderSettings2.y / 4.0f) * 4.0f;
		const float aoQualityEnabled = frame.renderSettings2.z > 0.5f ? 1.0f : 0.0f;
		// Use a branch instead of a zero-weight lerp so disabled/first-frame AO
		// never samples an image whose contents are intentionally being ignored.
		float screenOcclusion = 1.0f;
		if (aoQualityEnabled > 0.5f)
		{
			screenOcclusion = SampleScreenAmbientOcclusion(input, frame);
		}
		const float indirectOcclusion = lerp(1.0f, authoredOcclusion * screenOcclusion, saturate(aoStrength) * aoQualityEnabled);
		finalLighting += EvaluateIBL(frame, normal, viewDir, finalColor.rgb, metallic, roughness, indirectOcclusion);
	}

	float3 outputColor = finalLighting + emissive;
	if (!all(isfinite(outputColor)))
	{
		outputColor = finalColor.rgb;
	}
	TERRAENGINE_CONVENTIONAL_RETURN(float4(ApplyEditorTint(max(outputColor, float3(0.0f, 0.0f, 0.0f))), finalColor.a), stablePrimitiveId);
}
