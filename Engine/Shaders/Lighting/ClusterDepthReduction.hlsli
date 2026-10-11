// Shared numerical implementation for production and isolated validation.
// Variant defines select execution shape and descriptor layout, never policy.
#pragma pack_matrix(column_major)

struct ClusterDepth
{
	float minDepth;
	float maxDepth;
};
struct FrameCullingData
{
	float4 frustumPlanes[6];
	float4 cameraPositionAndEpsilon;
	uint4 clusterGridAndLightCount;
	float4 clusterDepthAndViewport;
	column_major float4x4 viewMatrix;
	column_major float4x4 invProjection;
	uint4 renderSettings0;
	uint4 renderSettings1;
	float4 renderSettings2;
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};
#if LIGHTING_DEPTH_ORACLE
[[vk::binding(0, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(1, 0)]] RWStructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> tileCoverage;
#else
[[vk::binding(12, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(13, 0)]] RWStructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;
#endif

// All variants retain the existing 64-byte push layout.
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
static const uint2 tileGrid = uint2(16, 9);

uint4 TileRectangle(uint2 tile, float2 viewport)
{
	// Float-derived truncation preserves empty rectangles on tiny viewports.
	const float2 tileSize = viewport / float2(tileGrid);
	const uint2 begin = uint2(float2(tile) * tileSize);
	const uint2 end = min(uint2(float2(tile + 1) * tileSize), uint2(viewport));
	return uint4(begin, end);
}
void AccumulatePixel(uint2 pixel, inout float2 extrema, inout uint2 coverage)
{
	const float depth = depthBuffer.Load(uint3(pixel, 0));
	++coverage.x; // One increment for each actual texture load.
	if (isfinite(depth) && depth >= 0.0 && depth < 1.0)
	{
		extrema.x = min(extrema.x, depth);
		extrema.y = max(extrema.y, depth);
		++coverage.y;
	}
}
void PublishTile(uint2 tile, float2 extrema, uint2 coverage)
{
	const uint index = clusterPush.clusterDepthOffset + tile.x + tile.y * tileGrid.x;
	ClusterDepth bounds;
	bounds.minDepth = coverage.y != 0 ? extrema.x : 1.0;
	bounds.maxDepth = coverage.y != 0 ? extrema.y : 1.0;
	clusterDepthBounds[index] = bounds;
#if LIGHTING_DEPTH_ORACLE
	tileCoverage[index] = coverage;
#endif
}

#if LIGHTING_DEPTH_COOPERATIVE
// At most 1024 bytes of shared storage; no hardware subgroup width assumed.
groupshared float2 laneExtrema[64];
groupshared uint2 laneCoverage[64];
[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
	// Uniform group exit precedes every barrier.
	if (any(groupId.xy >= tileGrid) || groupId.z != 0) return;
	const FrameCullingData frame = frameCullingData[clusterPush.cullingSlotIndex];
	const uint4 rect = TileRectangle(groupId.xy, frame.clusterDepthAndViewport.zw);
	const uint width = rect.z - rect.x;
	const uint count = width * (rect.w - rect.y);
	float2 extrema = float2(1.0, 0.0);
	uint2 coverage = uint2(0, 0);
	// Empty tiles execute no loads, so zero width never reaches division.
	for (uint local = lane; local < count; local += 64)
	{
		const uint2 pixel = rect.xy + uint2(local % width, local / width);
		AccumulatePixel(pixel, extrema, coverage);
	}
	laneExtrema[lane] = extrema;
	laneCoverage[lane] = coverage;
	GroupMemoryBarrierWithGroupSync();
	// Even lanes without pixels participate in every reduction barrier.
	[unroll]
	for (uint stride = 32; stride != 0; stride >>= 1)
	{
		if (lane < stride)
		{
			laneExtrema[lane] = float2(min(laneExtrema[lane].x, laneExtrema[lane + stride].x), max(laneExtrema[lane].y, laneExtrema[lane + stride].y));
			laneCoverage[lane] += laneCoverage[lane + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (lane == 0) PublishTile(groupId.xy, laneExtrema[0], laneCoverage[0]);
}
#else
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	if (any(dispatchThreadId.xy >= tileGrid) || dispatchThreadId.z != 0) return;
	const FrameCullingData frame = frameCullingData[clusterPush.cullingSlotIndex];
	const uint4 rect = TileRectangle(dispatchThreadId.xy, frame.clusterDepthAndViewport.zw);
	float2 extrema = float2(1.0, 0.0);
	uint2 coverage = uint2(0, 0);
	// Independent full scan is the reference for cooperative execution.
	for (uint y = rect.y; y < rect.w; ++y)
	{
		for (uint x = rect.x; x < rect.z; ++x) AccumulatePixel(uint2(x, y), extrema, coverage);
	}
	PublishTile(dispatchThreadId.xy, extrema, coverage);
}
#endif
