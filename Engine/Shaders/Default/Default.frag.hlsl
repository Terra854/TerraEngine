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
	uint Flags;
	uint BaseColorTextureIndex;
	uint NormalTextureIndex;
	uint MaskTextureIndex;
	uint OcclusionTextureIndex;
	uint EmissiveTextureIndex;
	uint Padding[2];
};

struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint reserved0;
	uint reserved1;
};

struct GPULight
{
	float4 positionAndType;
	float4 directionAndRange;
	float4 colorAndIntensity;
	float4 coneAnglesAndArea;
	float4 shadowAndPadding;
};

struct ClusterData
{
	uint offset;
	uint count;
};

[[vk::binding(1, 0)]] StructuredBuffer<MaterialData> allMaterials;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> globalLights;
[[vk::binding(5, 0)]] StructuredBuffer<ClusterData> clusterGrid;
[[vk::binding(6, 0)]] StructuredBuffer<uint> lightIndices;
[[vk::binding(7, 0)]] TextureCube pointShadowCube;
[[vk::binding(8, 0)]] SamplerState pointShadowSampler;

#pragma pack_matrix(column_major)

struct PushConstants
{
	column_major float4x4 viewProj;
	column_major float4x4 model;
	uint textureIndex;
	uint materialIndex;
	uint taskCullingEnabled;
	uint statsSlotIndex;
	float cullingEpsilon;
	float padding0;
	float padding1;
	float padding2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> SceneUniforms;

struct PSIn
{
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	float3 worldPosition : TEXCOORD1;
	float3 normal : NORMAL;
};

static const uint kLightTypeDirectional = 0u;
static const uint kLightTypePoint = 1u;
static const uint kLightTypeSpot = 2u;
static const uint kLightTypeAreaRect = 3u;
static const uint kMaxLightsPerCluster = 64u;
static const uint kMaxStatsSlots = 16u;
static const float kPi = 3.14159265f;
static const float kDirectionalShadowNear = 0.1f;
static const float kDirectionalShadowFar = 180.0f;
static const float kDirectionalShadowExtent = 40.0f;
static const float kDirectionalShadowBias = 0.0015f;
static const float kSpotShadowBias = 0.0020f;
static const float kPointShadowBias = 0.0050f;
static const float kAreaShadowBias = 0.0020f;

float3 BuildLightingNormal(float3 vertexNormal, float3 sampledNormal, float normalScale)
{
	const float3 unpackedNormal = sampledNormal * 2.0f - 1.0f;
	const float blendFactor = saturate(normalScale);
	return normalize(lerp(normalize(vertexNormal), unpackedNormal, blendFactor));
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
	const float distanceToLight = length(toLight);
	const float range = max(light.directionAndRange.w, 0.001);
	const float rangeFactor = saturate(1.0 - (distanceToLight / range));
	return rangeFactor * rangeFactor;
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
	const float shadowDepth = globalTextures[nonUniformIndex].Sample(globalSamplers[nonUniformIndex], uv).r;
	return (depthNdc - kDirectionalShadowBias) <= shadowDepth ? 1.0f : 0.0f;
}

float SampleSpotShadow(GPULight light, float3 worldPosition)
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
	const float shadowDepth = globalTextures[nonUniformIndex].Sample(globalSamplers[nonUniformIndex], uv).r;
	return (depthNdc - kSpotShadowBias) <= shadowDepth ? 1.0f : 0.0f;
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
	const float2 texel = 1.0f / max(float2(256.0f, 256.0f), frame.clusterDepthAndViewport.zw * 0.25f);
	const float kernelScale = saturate(max(width, height) / max(range, 0.1f)) * 2.0f;
	float visibility = 0.0f;
	float weight = 0.0f;
	[unroll]
	for (int y = -1; y <= 1; ++y)
	{
		[unroll]
		for (int x = -1; x <= 1; ++x)
		{
			const float2 offset = float2((float)x, (float)y) * texel * kernelScale;
			const float tapDepth = globalTextures[nonUniformIndex].Sample(globalSamplers[nonUniformIndex], uv + offset).r;
			const float tapVisibility = (depthNdc - kAreaShadowBias) <= tapDepth ? 1.0f : 0.0f;
			visibility += tapVisibility;
			weight += 1.0f;
		}
	}
	return weight > 0.0f ? (visibility / weight) : 1.0f;
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
		return SampleSpotShadow(light, worldPosition);
	}
	if (lightType == kLightTypeAreaRect)
	{
		return SampleAreaShadow(light, frame, worldPosition);
	}
	return 1.0f;
}

float3 EvaluateDirectionalLight(GPULight light, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 lightDir = SafeNormalize(-light.directionAndRange.xyz, float3(0.0, -1.0, 0.0));
	const float nDotL = saturate(dot(normal, lightDir));
	const float3 halfVector = SafeNormalize(lightDir + viewDir, lightDir);
	const float specPower = lerp(128.0, 8.0, saturate(roughness));
	const float specularTerm = pow(saturate(dot(normal, halfVector)), specPower);
	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, saturate(metallic));
	const float3 diffuse = albedo * (1.0 - saturate(metallic)) / kPi;
	const float3 specular = f0 * specularTerm;
	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	return (diffuse + specular) * radiance * nDotL;
}

float3 EvaluatePointLight(GPULight light, float3 worldPosition, float3 normal, float3 viewDir, float3 albedo, float metallic, float roughness)
{
	const float3 toLight = light.positionAndType.xyz - worldPosition;
	const float3 lightDir = SafeNormalize(toLight, float3(0.0, -1.0, 0.0));
	const float nDotL = saturate(dot(normal, lightDir));
	const float attenuation = ComputePointAttenuation(light, worldPosition);
	const float3 halfVector = SafeNormalize(lightDir + viewDir, lightDir);
	const float specPower = lerp(128.0, 8.0, saturate(roughness));
	const float specularTerm = pow(saturate(dot(normal, halfVector)), specPower);
	const float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, saturate(metallic));
	const float3 diffuse = albedo * (1.0 - saturate(metallic)) / kPi;
	const float3 specular = f0 * specularTerm;
	const float3 radiance = light.colorAndIntensity.rgb * light.colorAndIntensity.w;
	return (diffuse + specular) * radiance * nDotL * attenuation;
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
		return EvaluateAreaLightLTC(light, input.worldPosition, normal, viewDir, albedo, metallic, roughness) * shadowFactor;
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

	const uint statsIndex = min(SceneUniforms.statsSlotIndex, kMaxStatsSlots - 1u);
	const FrameCullingData frame = frameCullingData[statsIndex];
	const uint lightCount = frame.clusterGridAndLightCount.w;
	const float3 normal = BuildLightingNormal(input.normal, sampledNormal, mat.NormalScale);
	const float3 viewDir = normalize(frame.cameraPositionAndEpsilon.xyz - input.worldPosition);
	const uint clusterIndex = ComputeClusterIndex(input, frame);
	const ClusterData cluster = clusterGrid[clusterIndex];

	float3 lighting = float3(0.0, 0.0, 0.0);
	uint evaluatedLightCount = 0u;

	if (cluster.count > 0u)
	{
		const uint clusterLightCount = min(cluster.count, kMaxLightsPerCluster);
		[loop]
		for (uint lane = 0u; lane < clusterLightCount; ++lane)
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
	else
	{
		// Fallback keeps shading stable while the compute culling path warms up.
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
		const float normalizedCount = (float)evaluatedLightCount / float(kMaxLightsPerCluster);
		return float4(BuildHeatmapColor(normalizedCount), 1.0);
	}

	return float4(lighting + emissive, finalColor.a);
}
