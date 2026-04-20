#pragma pack_matrix(column_major)

struct GPULight
{
	float4 positionAndType;
	float4 directionAndRange;
	float4 colorAndIntensity;
	float4 coneAnglesAndArea;
	float4 shadowAndPadding;
	float4 extraParams;         // x = shadowStrength, y = shadowBias, z = indirectMultiplier, w = authoredPriority
	float4 shadowMatrixC0;
	float4 shadowMatrixC1;
	float4 shadowMatrixC2;
	float4 shadowMatrixC3;
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

struct ClusterDepth
{
	float minDepth;
	float maxDepth;
};

static const uint kCullThreadCount = 64u;

[[vk::binding(12, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(13, 0)]] StructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> globalLights;
[[vk::binding(5, 0)]] RWStructuredBuffer<ClusterData> clusterGrid;

groupshared uint gVisibleCount;

bool SphereIntersectsAABB(float3 center, float radius, float3 minB, float3 maxB)
{
	float3 closest = max(minB, min(center, maxB));
	float3 distV = closest - center;
	return dot(distV, distV) <= (radius * radius);
}

// Pass 1 only counts the final retained-light list chosen for shading. Offsets
// are resolved later so dense clusters are no longer forced into a fixed lane,
// and any wider uploaded candidate set has already been reduced upstream.
[numthreads(kCullThreadCount, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
	const FrameCullingData frame = frameCullingData[0];
	const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
	const uint clusterCount = gridDim.x * gridDim.y * gridDim.z;
	const uint clusterIndex = groupId.x;
	if (clusterIndex >= clusterCount)
	{
		return;
	}

	// Do not early-out from the tile-depth hint here. Thin silhouettes and
	// far-depth quantization can mark a still-visible tile as empty, which then
	// drops every clustered light for that screen region. The hint remains bound
	// for future optimization work, but correctness wins here.

	if (groupThreadId.x == 0u)
	{
		gVisibleCount = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	const ClusterData currentCluster = clusterGrid[clusterIndex];
	const uint lightCount = frame.clusterGridAndLightCount.w;

	for (uint lightIndex = groupThreadId.x; lightIndex < lightCount; lightIndex += kCullThreadCount)
	{
		const GPULight light = globalLights[lightIndex];
		const uint lightType = (uint)(light.positionAndType.w + 0.5f);

		bool intersects = false;
		if (lightType == 0u)
		{
			intersects = true;
		}
		else if (lightType == 1u || lightType == 2u)
		{
			float3 lightPosView = mul(frame.viewMatrix, float4(light.positionAndType.xyz, 1.0f)).xyz;
			float range = max(light.directionAndRange.w, 0.001f);
			intersects = SphereIntersectsAABB(lightPosView, range, currentCluster.minBounds, currentCluster.maxBounds);
		}
		else if (lightType == 3u)
		{
			float3 lightPosView = mul(frame.viewMatrix, float4(light.positionAndType.xyz, 1.0f)).xyz;
			float extent = max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
			float range = max(light.directionAndRange.w, extent);
			intersects = SphereIntersectsAABB(lightPosView, range, currentCluster.minBounds, currentCluster.maxBounds);
		}

		if (intersects)
		{
			InterlockedAdd(gVisibleCount, 1u);
		}
	}

	GroupMemoryBarrierWithGroupSync();

	if (groupThreadId.x == 0u)
	{
		clusterGrid[clusterIndex].offset = 0u;
		clusterGrid[clusterIndex].count = gVisibleCount;
		clusterGrid[clusterIndex].padding = uint2(0u, 0u);
	}
}
