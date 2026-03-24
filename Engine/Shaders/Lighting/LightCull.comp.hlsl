#pragma pack_matrix(column_major)

struct GPULight
{
	float4 positionAndType;
	float4 directionAndRange;
	float4 colorAndIntensity;
	float4 coneAnglesAndArea;
	float4 shadowAndPadding;
	float4 extraParams;         // x = shadowStrength, y = shadowBias, z = indirectMultiplier, w = authoredPriority
};

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

struct GPULightAnalysisRecord
{
	float gpuVisibilityScore;
	float gpuRangeCoverage;
	uint flags;
	uint reserved;
};

struct AnalysisPushConstants
{
	uint lightCount;
	uint outputOffset;
	uint padding0;
	uint padding1;
	uint4 paddingBlock0;
	uint4 paddingBlock1;
	uint4 paddingBlock2;
};

static const uint kAnalysisThreadCount = 64u;
static const uint kLightTypeDirectional = 0u;
static const uint kLightTypeAreaRect = 3u;
static const uint kGpuLightAnalysisFlagVisible = 1u << 0;
static const uint kGpuLightAnalysisFlagDirectional = 1u << 1;

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(17, 0)]] RWStructuredBuffer<GPULightAnalysisRecord> lightAnalysisBuffer;
[[vk::binding(18, 0)]] StructuredBuffer<GPULight> analysisLights;
[[vk::push_constant]] ConstantBuffer<AnalysisPushConstants> analysisPush;

bool SphereIntersectsFrustum(float3 center, float radius, float4 planes[6])
{
	[unroll]
	for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
	{
		const float4 plane = planes[planeIndex];
		if (dot(plane.xyz, center) + plane.w < -radius)
		{
			return false;
		}
	}
	return true;
}

// This pass gives the CPU a lightweight next-frame hint about which uploaded
// lights were plausibly relevant to the current view. It intentionally avoids
// heavy cluster readback and only writes one compact record per uploaded light.
[numthreads(kAnalysisThreadCount, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint lightIndex = dispatchThreadId.x;
	if (lightIndex >= analysisPush.lightCount)
	{
		return;
	}

	const FrameCullingData frame = frameCullingData[0];
	const GPULight light = analysisLights[lightIndex];
	const uint lightType = (uint)(light.positionAndType.w + 0.5f);
	const float intensity = max(light.colorAndIntensity.w, 0.0f);

	GPULightAnalysisRecord record;
	record.gpuVisibilityScore = 0.0f;
	record.gpuRangeCoverage = 0.0f;
	record.flags = 0u;
	record.reserved = 0u;

	if (lightType == kLightTypeDirectional)
	{
		record.gpuVisibilityScore = max(intensity, 1.0f);
		record.gpuRangeCoverage = 1.0f;
		record.flags = kGpuLightAnalysisFlagVisible | kGpuLightAnalysisFlagDirectional;
		lightAnalysisBuffer[analysisPush.outputOffset + lightIndex] = record;
		return;
	}

	float range = max(light.directionAndRange.w, 0.001f);
	if (lightType == kLightTypeAreaRect)
	{
		const float extent = max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
		range = max(range, extent);
	}

	const float3 cameraPosition = frame.cameraPositionAndEpsilon.xyz;
	const float3 lightPosition = light.positionAndType.xyz;
	const float distanceToCamera = length(lightPosition - cameraPosition);
	const float rangeCoverage = saturate((range - distanceToCamera) / max(range, 0.001f));

	record.gpuRangeCoverage = rangeCoverage;
	if (SphereIntersectsFrustum(lightPosition, range, frame.frustumPlanes))
	{
		record.flags |= kGpuLightAnalysisFlagVisible;
		record.gpuVisibilityScore = (1.0f + intensity) * max(rangeCoverage, 0.05f);
	}

	lightAnalysisBuffer[analysisPush.outputOffset + lightIndex] = record;
}
