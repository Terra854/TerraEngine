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

// The cluster grid is small (3456 entries), so a single deterministic scan is
// cheap and keeps offset generation stable across repeated validation runs.
[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (dispatchThreadId.x > 0u || dispatchThreadId.y > 0u || dispatchThreadId.z > 0u)
	{
		return;
	}

	const FrameCullingData frame = frameCullingData[clusterPush.cullingSlotIndex];
	const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
	const uint clusterCount = gridDim.x * gridDim.y * gridDim.z;

	uint runningOffset = 0u;
	for (uint clusterIndex = 0u; clusterIndex < clusterCount; ++clusterIndex)
	{
		ClusterData cluster = clusterGrid[clusterPush.clusterDataOffset + clusterIndex];
		cluster.offset = runningOffset;
		runningOffset += cluster.count;
		clusterGrid[clusterPush.clusterDataOffset + clusterIndex] = cluster;
	}
}
