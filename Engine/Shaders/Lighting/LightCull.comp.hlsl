#pragma pack_matrix(column_major)

struct GPULight
{
	float4 positionAndType;
	float4 directionAndRange;
	float4 colorAndIntensity;
	float4 coneAnglesAndArea;
	float4 shadowAndPadding;
	float4 extraParams;
};

struct ClusterData
{
	uint offset;
	uint count;
	float3 minBounds;
	float3 maxBounds;
};

struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	column_major float4x4 viewMatrix;
	column_major float4x4 invProjection;
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint reserved0;
	uint reserved1;
};

struct ClusterDepth
{
    float minDepth;
    float maxDepth;
};

static const uint kMaxLightsPerCluster = 64u;
static const uint kCullThreadCount = 64u;
static const uint kLightTypeDirectional = 0u;
static const uint kLightTypePoint = 1u;
static const uint kLightTypeSpot = 2u;
static const uint kLightTypeAreaRect = 3u;

[[vk::binding(12, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(13, 0)]] StructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(4, 0)]] StructuredBuffer<GPULight> globalLights;
[[vk::binding(5, 0)]] RWStructuredBuffer<ClusterData> clusterGrid;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> lightIndices;

groupshared uint gVisibleCount;
groupshared uint gSharedIndices[kMaxLightsPerCluster];

bool SphereIntersectsAABB(float3 center, float radius, float3 minB, float3 maxB)
{
    float3 closest = max(minB, min(center, maxB));
    float3 distV = closest - center;
    return dot(distV, distV) <= (radius * radius);
}

// Per-cluster light culling using group-shared memory for cooperative writes.
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

    // Depth Masking Optimization
    const uint xyIndex = clusterIndex % (gridDim.x * gridDim.y);
    const uint zIndex = clusterIndex / (gridDim.x * gridDim.y);
    const ClusterDepth tileDepth = clusterDepthBounds[xyIndex];
    
    // If no geometry in this tile column, skip all clusters in it
    if (tileDepth.minDepth >= 1.0) {
        if (groupThreadId.x == 0u) {
            clusterGrid[clusterIndex].count = 0u;
        }
        return;
    }

    // Convert cluster Z range to NDC for comparison with tile depth
    // Note: This depends on how cluster Z is partitioned.
    // Usually clusters are exponential or linear in view space.
    // For now, we assume the cluster bounds in ClusterData are already in view space.
    // Actually, LightCull uses view space AABBs.
    
    // Check if cluster Z range overlaps with tile depth range
    // Since tileDepth is in NDC (0..1), we need to compare appropriately.
    // If we want to be simple, we just use the tile-based masking for now.

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
		const uint lightType = (uint)(light.positionAndType.w + 0.5);

		bool intersects = false;
		if (lightType == kLightTypeDirectional)
		{
			intersects = true;
		}
		else if (lightType == kLightTypePoint || lightType == kLightTypeSpot)
		{
			float3 lightPosView = mul(frame.viewMatrix, float4(light.positionAndType.xyz, 1.0)).xyz;
			float range = max(light.directionAndRange.w, 0.001);
			intersects = SphereIntersectsAABB(lightPosView, range, currentCluster.minBounds, currentCluster.maxBounds);
		}
		else if (lightType == kLightTypeAreaRect)
		{
			float3 lightPosView = mul(frame.viewMatrix, float4(light.positionAndType.xyz, 1.0)).xyz;
			float extent = max(light.coneAnglesAndArea.z, light.coneAnglesAndArea.w);
			float range = max(light.directionAndRange.w, extent);
			intersects = SphereIntersectsAABB(lightPosView, range, currentCluster.minBounds, currentCluster.maxBounds);
		}

		if (intersects) {
			uint slot = 0u;
			InterlockedAdd(gVisibleCount, 1u, slot);
			if (slot < kMaxLightsPerCluster)
			{
				gSharedIndices[slot] = lightIndex;
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();

	const uint visibleCount = min(gVisibleCount, kMaxLightsPerCluster);
	const uint outputOffset = clusterIndex * kMaxLightsPerCluster;

	if (groupThreadId.x == 0u)
	{
		clusterGrid[clusterIndex].count = visibleCount;
	}

	if (groupThreadId.x < visibleCount)
	{
		lightIndices[outputOffset + groupThreadId.x] = gSharedIndices[groupThreadId.x];
	}
}
