#pragma pack_matrix(column_major)

struct ClusterData
{
	uint offset;
	uint count;
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

static const uint kMaxLightsPerCluster = 64u;

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(5, 0)]] RWStructuredBuffer<ClusterData> clusterGrid;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> lightIndices;

// Builds a dense 3D cluster table where each cluster reserves a contiguous
// segment in the flat light-index list. This pass currently stores metadata
// only and is intentionally conservative for correctness-first integration.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const FrameCullingData frame = frameCullingData[0];
	const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
	if (dispatchThreadId.x >= gridDim.x || dispatchThreadId.y >= gridDim.y || dispatchThreadId.z >= gridDim.z)
	{
		return;
	}

	const uint clusterIndex = dispatchThreadId.x
		+ dispatchThreadId.y * gridDim.x
		+ dispatchThreadId.z * (gridDim.x * gridDim.y);

	// The normalized tile/depth math is kept for future AABB reconstruction.
	const float2 screenDim = max(frame.clusterDepthAndViewport.zw, float2(1.0, 1.0));
	const float2 tileSize = screenDim / max(float2(gridDim.xy), float2(1.0, 1.0));
	const float2 tileMin = dispatchThreadId.xy * tileSize / screenDim;
	const float2 tileMax = (dispatchThreadId.xy + 1.0) * tileSize / screenDim;
	const float nearPlane = frame.clusterDepthAndViewport.x;
	const float farPlane = max(frame.clusterDepthAndViewport.y, nearPlane + 0.001);
	const float zSliceMin = nearPlane + (farPlane - nearPlane) * (float(dispatchThreadId.z) / max(float(gridDim.z), 1.0));
	const float zSliceMax = nearPlane + (farPlane - nearPlane) * (float(dispatchThreadId.z + 1u) / max(float(gridDim.z), 1.0));
	(void)tileMin;
	(void)tileMax;
	(void)zSliceMin;
	(void)zSliceMax;

	ClusterData cluster;
	cluster.offset = clusterIndex * kMaxLightsPerCluster;
	cluster.count = 0u;
	clusterGrid[clusterIndex] = cluster;

	// Reset this cluster's light-index lane so stale values are never consumed.
	for (uint lane = 0u; lane < kMaxLightsPerCluster; ++lane)
	{
		lightIndices[cluster.offset + lane] = 0xFFFFFFFFu;
	}
}
