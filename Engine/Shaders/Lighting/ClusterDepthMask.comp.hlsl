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
	uint4 renderSettings0; // x: shadowRes, y: shadowFilter, z: retainedLightBudget, w: enableLTC
	uint4 renderSettings1; // x: enableIBL, y: aniso, z: shadowLightBudget, w: uploadedLightBudget
	float4 renderSettings2; // x: indirectStrength, y: aoStrength, z: aoQuality, w: hiZPyramidAvailable
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};
[[vk::binding(12, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(13, 0)]] RWStructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;

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

// This shader computes the min/max depth of pixels in each tile.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const FrameCullingData frame = frameCullingData[clusterPush.cullingSlotIndex];
    const float2 viewportSize = frame.clusterDepthAndViewport.zw;
    const uint3 gridDim = frame.clusterGridAndLightCount.xyz;
    
    if (dispatchThreadId.x >= gridDim.x || dispatchThreadId.y >= gridDim.y) return;

    // Calculate tile size in pixels
    float2 tileSize = viewportSize / float2(gridDim.xy);
    uint2 tileStart = uint2(dispatchThreadId.xy * tileSize);
    uint2 tileEnd = uint2((dispatchThreadId.xy + 1) * tileSize);
    
    // Clamp tileEnd to viewport
    tileEnd = min(tileEnd, uint2(viewportSize));

    // Standard Z: 0.0 is Near, 1.0 is Far (Clear value is 1.0)
    float minZ = 1.0; 
    float maxZ = 0.0;
    bool anyPixel = false;

    // Sample every pixel in the tile. A previous 2x2 stride was cheaper, but
    // thin silhouettes and distant meshes could slip between the samples and
    // incorrectly mark the whole tile column as empty, which then removed all
    // clustered lights for still-visible geometry.
    for (uint y = tileStart.y; y < tileEnd.y; ++y) {
        for (uint x = tileStart.x; x < tileEnd.x; ++x) {
            float d = depthBuffer.Load(uint3(x, y, 0));
            if (d < 1.0) { // Valid pixel (not far plane/clear)
                minZ = min(minZ, d); // Nearer (smaller value)
                maxZ = max(maxZ, d); // Further (larger value)
                anyPixel = true;
            }
        }
    }

    uint tileIndex = dispatchThreadId.x + dispatchThreadId.y * gridDim.x;
    
    if (!anyPixel) {
        // No geometry in this tile column
        clusterDepthBounds[clusterPush.clusterDepthOffset + tileIndex].minDepth = 1.0;
        clusterDepthBounds[clusterPush.clusterDepthOffset + tileIndex].maxDepth = 1.0;
    } else {
        clusterDepthBounds[clusterPush.clusterDepthOffset + tileIndex].minDepth = minZ;
        clusterDepthBounds[clusterPush.clusterDepthOffset + tileIndex].maxDepth = maxZ;
    }
}
