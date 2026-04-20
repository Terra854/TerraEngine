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
	uint Padding[1];
};

static const uint kShadingModel_Unlit = 0u;
static const uint kShadingModel_MetallicRoughness = 1u;
static const uint kShadingModel_SpecularGlossiness = 2u;

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
	float4 shadowAndPadding;    // x: castsShadows, y: shadowTex, z: ltcAmp, w: ltcInv
	float4 extraParams;         // x: shadowStrength, y: shadowBias, z: indirectMultiplier, w: authoredPriority
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
[[vk::binding(7, 0)]] TextureCube pointShadowCube;
[[vk::binding(8, 0)]] SamplerState pointShadowSampler;
[[vk::binding(9, 0)]] StructuredBuffer<uint> globalBuffers[];

#pragma pack_matrix(column_major)

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
	float padding;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
};

static const uint kLightTypeDirectional = 0u;
static const uint kLightTypePoint = 1u;
static const uint kLightTypeSpot = 2u;
static const uint kLightTypeAreaRect = 3u;
static const uint kMaxStatsSlots = 16u;
static const float kPi = 3.14159265f;
static const float kDirectionalShadowNear = 0.1f;
static const float kDirectionalShadowFar = 180.0f;
static const float kDirectionalShadowExtent = 40.0f;
static const float kDirectionalShadowBias = 0.0015f;
static const float kSpotShadowBias = 0.0020f;
static const float kPointShadowBias = 0.0050f;
static const float kAreaShadowBias = 0.0020f;
// Match the sky system rule: once the sun is roughly ten degrees below the
// horizon, direct sunlight should no longer illuminate scene geometry.
static const float kDirectionalLightNightHeight = -0.17364818f;
// Point lights with zero height relative to a surface have almost no Lambert
// term on grazing faces. This small wrapped term approximates nearby bounce and
// finite light size until full global illumination/area-light sampling exists.
static const float kLocalIndirectWrap = 0.55f;
static const float kLocalIndirectStrength = 0.24f;
static const float kLocalIndirectMaxMultiplier = 16.0f;
static const float kLocalIndirectBounceFloor = 0.12f;

float3 BuildLightingNormal(float3 vertexNormal, float3 worldPosition, float2 uv, float3 sampledNormal, float normalScale, float4 bakedTangent)
{
	const float3 n = normalize(vertexNormal);
	const float3 t = normalize(bakedTangent.xyz);
	const float3 b = normalize(cross(n, t) * bakedTangent.w);
	const float3x3 tbn = float3x3(t, b, n);

	const float3 unpackedNormal = sampledNormal * 2.0f - 1.0f;
	const float3 scaledNormal = normalize(lerp(float3(0.0, 0.0, 1.0), unpackedNormal, saturate(normalScale)));
	return normalize(mul(scaledNormal, tbn));
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

float3 SafeNormalize(float3 value, float3 fallback)
{
	const float lenSq = dot(value, value);
	if (lenSq <= 1e-8f)
	{
		return fallback;
	}
	return value * rsqrt(lenSq);
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
	if (filterSize <= 1u)
	{
		const float shadowDepth = shadowMap.Sample(shadowSampler, uv).r;
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
			visibility += (depthNdc - bias) <= tapDepth ? 1.0f : 0.0f;
			weight += 1.0f;
		}
	}
	return visibility / weight;
}

float SampleDirectionalShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(shadowTextureIndex);
	const float3 lightDir = SafeNormalize(-light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float3 upHint = (abs(dot(lightDir, float3(0.0, 1.0, 0.0))) > 0.95f) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
	const float3 right = SafeNormalize(cross(upHint, lightDir), float3(1.0, 0.0, 0.0));
	const float3 up = SafeNormalize(cross(lightDir, right), float3(0.0, 1.0, 0.0));
	const float3 lightPosition = frame.cameraPositionAndEpsilon.xyz - lightDir * 60.0f;
	const float3 toPoint = worldPosition - lightPosition;

	const float2 uv = float2(
		dot(toPoint, right) / (2.0f * kDirectionalShadowExtent) + 0.5f,
		dot(toPoint, up) / (2.0f * kDirectionalShadowExtent) + 0.5f);
	if (any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
	{
		return 1.0f;
	}

	const float linearDepth = dot(toPoint, lightDir);
	const float depthNdc = saturate((linearDepth - kDirectionalShadowNear) / (kDirectionalShadowFar - kDirectionalShadowNear));
	
	const float res = (float)frame.renderSettings0.x;
	const float2 texelSize = 1.0f / max(res, 512.0f);
	return SampleShadowPCF(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], uv, depthNdc, kDirectionalShadowBias, frame.renderSettings0.y, texelSize);
}

float SampleSpotShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(shadowTextureIndex);
	const float3 lightPos = light.positionAndType.xyz;
	const float3 forward = SafeNormalize(light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float3 upHint = (abs(dot(forward, float3(0.0, 1.0, 0.0))) > 0.95f) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
	const float3 right = SafeNormalize(cross(upHint, forward), float3(1.0, 0.0, 0.0));
	const float3 up = SafeNormalize(cross(forward, right), float3(0.0, 1.0, 0.0));
	const float3 toPoint = worldPosition - lightPos;
	const float3 lightSpace = float3(dot(toPoint, right), dot(toPoint, up), dot(toPoint, forward));
	if (lightSpace.z <= 0.0f)
	{
		return 1.0f;
	}

	const float outerCos = clamp(light.coneAnglesAndArea.y, -0.999f, 0.999f);
	const float tanHalfAngle = sqrt(saturate(1.0f - outerCos * outerCos)) / max(outerCos, 0.001f);
	const float2 uv = (lightSpace.xy / max(lightSpace.z, 1e-4f)) / max(tanHalfAngle, 1e-3f) * 0.5f + 0.5f;
	if (any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
	{
		return 1.0f;
	}

	const float range = max(light.directionAndRange.w, 0.1f);
	const float depthNdc = saturate(lightSpace.z / range);
	
	const float res = (float)frame.renderSettings0.x;
	const float2 texelSize = 1.0f / max(res, 512.0f);
	return SampleShadowPCF(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], uv, depthNdc, kSpotShadowBias, frame.renderSettings0.y, texelSize);
}

float SamplePointShadow(GPULight light, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f)
	{
		return 1.0f;
	}

	const float3 toPoint = worldPosition - light.positionAndType.xyz;
	const float3 sampleDir = SafeNormalize(toPoint, float3(0.0, 0.0, 1.0));
	const float sampledDepth = pointShadowCube.Sample(pointShadowSampler, sampleDir).r;
	const float distanceToLight = length(toPoint);
	const float range = max(light.directionAndRange.w, 0.1f);
	const float depthNdc = saturate(distanceToLight / range);
	return (depthNdc - kPointShadowBias) <= sampledDepth ? 1.0f : 0.0f;
}

float SampleAreaShadow(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	if (light.shadowAndPadding.x <= 0.5f || light.shadowAndPadding.y < 0.0f)
	{
		return 1.0f;
	}

	const uint shadowTextureIndex = (uint)(light.shadowAndPadding.y + 0.5f);
	const uint nonUniformIndex = NonUniformResourceIndex(shadowTextureIndex);
	const float3 lightPos = light.positionAndType.xyz;
	const float3 forward = SafeNormalize(light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float3 upHint = (abs(dot(forward, float3(0.0, 1.0, 0.0))) > 0.95f) ? float3(1.0, 0.0, 0.0) : float3(0.0, 1.0, 0.0);
	const float3 right = SafeNormalize(cross(upHint, forward), float3(1.0, 0.0, 0.0));
	const float3 up = SafeNormalize(cross(forward, right), float3(0.0, 1.0, 0.0));
	const float3 toPoint = worldPosition - lightPos;
	const float projectedDepth = dot(toPoint, forward);
	if (projectedDepth <= 0.0f)
	{
		return 1.0f;
	}

	const float range = max(light.directionAndRange.w, 0.1f);
	const float width = max(light.coneAnglesAndArea.z, 0.1f);
	const float height = max(light.coneAnglesAndArea.w, 0.1f);
	const float2 uv = float2(dot(toPoint, right) / (width * range), dot(toPoint, up) / (height * range)) * 0.5f + 0.5f;
	if (any(uv < float2(0.0, 0.0)) || any(uv > float2(1.0, 1.0)))
	{
		return 1.0f;
	}

	const float depthNdc = saturate(projectedDepth / range);
	const float res = (float)frame.renderSettings0.x;
	const float2 texel = 1.0f / max(res, 512.0f);
	const float kernelScale = saturate(max(width, height) / max(range, 0.1f)) * 2.0f;
	
	return SampleShadowPCF(globalTextures[nonUniformIndex], globalSamplers[nonUniformIndex], uv, depthNdc, kAreaShadowBias, frame.renderSettings0.y, texel * kernelScale);
}

float ComputeShadowFactor(GPULight light, FrameCullingData frame, float3 worldPosition)
{
	const uint lightType = (uint)(light.positionAndType.w + 0.5);
	if (lightType == kLightTypeDirectional)
	{
		return SampleDirectionalShadow(light, frame, worldPosition);
	}
	if (lightType == kLightTypePoint)
	{
		return SamplePointShadow(light, worldPosition);
	}
	if (lightType == kLightTypeSpot)
	{
		return SampleSpotShadow(light, frame, worldPosition);
	}
	if (lightType == kLightTypeAreaRect)
	{
		return SampleAreaShadow(light, frame, worldPosition);
	}
	return 1.0f;
}

float D_GGX(float nDotH, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float d = (nDotH * a2 - nDotH) * nDotH + 1.0;
	return a2 / (kPi * d * d);
}

float V_SmithJointSchlick(float nDotL, float nDotV, float roughness)
{
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

float3 EvaluateLocalIndirectDiffuse(GPULight light, float rawNDotL, float attenuation, float3 albedo, float metallic, float roughness)
{
	// Artists can deliberately push this above 1 for editor/debug setups with
	// low lights. Clamp only the upper bound so accidental huge values do not
	// flood the scene with distance-attenuated fill.
	const float indirectMultiplier = clamp(light.extraParams.z, 0.0f, kLocalIndirectMaxMultiplier);
	if (indirectMultiplier <= 0.0f || attenuation <= 0.0f)
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
	return diffuse * radiance * attenuation * bounceVisibility * diffuseMaterialWeight * indirectMultiplier * kLocalIndirectStrength;
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

	float3 lighting = EvaluateLocalIndirectDiffuse(light, rawNDotL, attenuation, albedo, metallic, roughness);
	const float nDotL = saturate(rawNDotL);
	if (nDotL <= 0.0f) return lighting;

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
	return lighting + (diffuse + specular) * radiance * nDotL * attenuation;
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
	const GPULight light = globalLights[lightIndex];
	const uint lightType = (uint)(light.positionAndType.w + 0.5);
	const float shadowFactor = ComputeShadowFactor(light, frame, input.worldPosition);
	if (lightType == kLightTypeDirectional)
	{
		return EvaluateDirectionalLight(light, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
	}
	if (lightType == kLightTypePoint)
	{
		return EvaluatePointLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
	}
	if (lightType == kLightTypeSpot)
	{
		return EvaluateSpotLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
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
			return EvaluatePointLight(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
		}
	}
	return float3(0.0, 0.0, 0.0);
}

float3 BuildHeatmapColor(float normalizedLightCount)
{
	const float t = saturate(normalizedLightCount);
	return lerp(float3(0.0, 0.2, 1.0), float3(1.0, 0.2, 0.0), t);
}

float4 main(PSIn input) : SV_Target0
{
	const MaterialData mat = allMaterials[SceneUniforms.materialIndex];
	const float2 uv = input.uv * mat.UVTransform.xy + mat.UVTransform.zw;

	const float4 baseColorSample = globalTextures[NonUniformResourceIndex(mat.BaseColorTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.BaseColorTextureIndex)], uv);
	const float4 finalColor = baseColorSample * mat.BaseColorFactor;
	if (mat.AlphaMode == 1u)
	{
		clip(finalColor.a - mat.AlphaCutoff);
	}

	const float3 sampledNormal = globalTextures[NonUniformResourceIndex(mat.NormalTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.NormalTextureIndex)], uv).rgb;
	const float4 maskSample = globalTextures[NonUniformResourceIndex(mat.MaskTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.MaskTextureIndex)], uv);
	const float roughness = saturate(maskSample.g * mat.RoughnessFactor);
	const float metallic = saturate(maskSample.b * mat.MetallicFactor);
	const float3 emissive = globalTextures[NonUniformResourceIndex(mat.EmissiveTextureIndex)].Sample(globalSamplers[NonUniformResourceIndex(mat.EmissiveTextureIndex)], uv).rgb * mat.EmissiveFactor.rgb;

	if (mat.ShadingModel == kShadingModel_Unlit)
	{
		return float4(finalColor.rgb + emissive, finalColor.a);
	}

	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frame = frameCullingData[statsIndex];
	const uint lightCount = frame.clusterGridAndLightCount.w;
	const float3 normal = BuildLightingNormal(input.normal, input.worldPosition, uv, sampledNormal, mat.NormalScale, input.tangent);
	const float3 viewDir = normalize(frame.cameraPositionAndEpsilon.xyz - input.worldPosition);
	const uint clusterIndex = ComputeClusterIndex(input, frame);
	const ClusterData cluster = clusterGrid[clusterIndex];
	const bool clusteredLightingAvailable = frame.clusteredLightingValid > 0u && frame.clusteredLightingOverflowed == 0u;

	float3 lighting = float3(0.0, 0.0, 0.0);
	uint evaluatedLightCount = 0u;

	if (clusteredLightingAvailable)
	{
		[loop]
		for (uint lane = 0u; lane < cluster.count; ++lane)
		{
			const uint lightIndex = lightIndices[cluster.offset + lane];
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
		return float4(BuildHeatmapColor(normalizedCount), 1.0);
	}

	float3 finalLighting = lighting;
	if (frame.renderSettings1.x > 0u)
	{
		// Placeholder for IBL evaluation (Ultra tier)
		// finalLighting += EvaluateIBL(normal, viewDir, finalColor.rgb, metallic, roughness);
	}

	return float4(finalLighting + emissive, finalColor.a);
}
