#pragma pack_matrix(column_major)

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
	uint4 renderSettings0; // x: shadowRes, y: shadowFilter, z: maxLights, w: enableLTC
	uint4 renderSettings1; // x: enableIBL, y: aniso, zw: reserved
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

	float zNear = zSliceMin;
	float zFar = zSliceMax;

	// Project 8 corners of the tile
	float2 p[4] = { tileMin, float2(tileMax.x, tileMin.y), tileMax, float2(tileMin.x, tileMax.y) };
	float3 minB = float3(1e10, 1e10, 1e10);
	float3 maxB = float3(-1e10, -1e10, -1e10);

	for(int i=0; i<4; ++i) {
		float2 ndcXY = p[i] * 2.0 - 1.0;
		// NDC y is typically inverted in Vulkan (top is -1, bottom is 1), 
		// but our tile math follows screen space (top is 0, bottom is H).
		// So we map p[i].y (0 to 1) to (1 to -1).
		ndcXY.y = 1.0 - p[i].y * 2.0;

		float4 nearClip = float4(ndcXY, zNear, 1.0);
		float4 nearView = mul(frame.invProjection, nearClip);
		float3 nearPoint = nearView.xyz / nearView.w;

		float4 farClip = float4(ndcXY, zFar, 1.0);
		float4 farView = mul(frame.invProjection, farClip);
		float3 farPoint = farView.xyz / farView.w;

		minB = min(minB, min(nearPoint, farPoint));
		maxB = max(maxB, max(nearPoint, farPoint));
	}

	ClusterData cluster;
	cluster.offset = clusterIndex * kMaxLightsPerCluster;
	cluster.count = 0u;
	cluster.minBounds = minB;
	cluster.maxBounds = maxB;
	clusterGrid[clusterIndex] = cluster;

	// Reset this cluster's light-index lane so stale values are never consumed.
	for (uint lane = 0u; lane < kMaxLightsPerCluster; ++lane)
	{
		lightIndices[cluster.offset + lane] = 0xFFFFFFFFu;
	}
}
