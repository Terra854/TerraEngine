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
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> lightIndices;

groupshared uint gWriteCount;

bool SphereIntersectsAABB(float3 center, float radius, float3 minB, float3 maxB)
{
	float3 closest = max(minB, min(center, maxB));
	float3 distV = closest - center;
	return dot(distV, distV) <= (radius * radius);
}

// Pass 3 writes each cluster's contiguous range for the final retained-light
// list after the upstream budgeting and GPU-analysis stages have finished.
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

	// Keep filling cluster light lists even if the tile-depth hint appears
	// empty. Visibility at far distances can quantize to the clear-depth value
	// and would otherwise make valid geometry lose all local lights.

	if (groupThreadId.x == 0u)
	{
		gWriteCount = 0u;
	}
	GroupMemoryBarrierWithGroupSync();

	const ClusterData currentCluster = clusterGrid[clusterIndex];
	const uint lightCount = frame.clusterGridAndLightCount.w;
	if (currentCluster.count == 0u)
	{
		return;
	}

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
			uint slot = 0u;
			InterlockedAdd(gWriteCount, 1u, slot);
			if (slot < currentCluster.count)
			{
				lightIndices[currentCluster.offset + slot] = lightIndex;
			}
		}
	}
}
