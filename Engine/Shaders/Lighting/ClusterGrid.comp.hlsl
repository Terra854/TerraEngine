#pragma pack_matrix(column_major)

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
	float4 renderSettings2; // x: indirectStrength, y: aoStrength, z: aoQuality, w: hiZHistoryValid
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};

[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(5, 0)]] RWStructuredBuffer<ClusterData> clusterGrid;

struct ClusterBuildPushConstants
{
	uint cullingSlotIndex;
	uint lightDataOffset;
	uint clusterDataOffset;
	uint lightIndexOffset;
	uint clusterDepthOffset;
	uint3 padding0;
	uint4 padding1;
	uint4 padding2;
};

[[vk::push_constant]] ConstantBuffer<ClusterBuildPushConstants> clusterPush;

// Builds a dense 3D cluster table where each cluster reserves a contiguous
// segment in the flat light-index list. This pass currently stores metadata
// only and is intentionally conservative for correctness-first integration.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const FrameCullingData frame = frameCullingData[clusterPush.cullingSlotIndex];
	const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
	if (dispatchThreadId.x >= gridDim.x || dispatchThreadId.y >= gridDim.y || dispatchThreadId.z >= gridDim.z)
	{
		return;
	}

	const uint clusterIndex = dispatchThreadId.x
		+ dispatchThreadId.y * gridDim.x
		+ dispatchThreadId.z * (gridDim.x * gridDim.y);

	// Cluster reconstruction must match the projection conventions used by the
	// rest of the renderer. The CPU already applies the Vulkan Y flip to the
	// projection matrix, so screen-space UVs map back to NDC with a direct
	// [-1, 1] remap. Depth slices also live in standard ZO depth because the
	// fragment shader picks clusters from SV_Position.z in that same domain.
	const float2 screenDim = max(frame.clusterDepthAndViewport.zw, float2(1.0, 1.0));
	const float2 tileSize = screenDim / max(float2(gridDim.xy), float2(1.0, 1.0));
	const float2 tileMin = dispatchThreadId.xy * tileSize / screenDim;
	const float2 tileMax = (dispatchThreadId.xy + 1.0) * tileSize / screenDim;
	const float zNear = float(dispatchThreadId.z) / max(float(gridDim.z), 1.0);
	const float zFar = float(dispatchThreadId.z + 1u) / max(float(gridDim.z), 1.0);

	// Project 8 corners of the tile
	float2 p[4] = { tileMin, float2(tileMax.x, tileMin.y), tileMax, float2(tileMin.x, tileMax.y) };
	float3 minB = float3(1e10, 1e10, 1e10);
	float3 maxB = float3(-1e10, -1e10, -1e10);

	for(int i=0; i<4; ++i) {
		float2 ndcXY = p[i] * 2.0 - 1.0;

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
	cluster.offset = 0u;
	cluster.count = 0u;
	cluster.padding = uint2(0u, 0u);
	cluster.minBounds = minB;
	cluster.minBoundsPadding = 0.0;
	cluster.maxBounds = maxB;
	cluster.maxBoundsPadding = 0.0;
	clusterGrid[clusterPush.clusterDataOffset + clusterIndex] = cluster;
}
