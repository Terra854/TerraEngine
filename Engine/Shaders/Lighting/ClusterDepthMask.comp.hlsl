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
	uint globalTaskCullingEnabled;
	uint taskShaderSupported;
	uint clusteredLightingValid;
	uint clusteredLightingOverflowed;
};
[[vk::binding(12, 0)]] Texture2D<float> depthBuffer;
[[vk::binding(13, 0)]] RWStructuredBuffer<ClusterDepth> clusterDepthBounds;
[[vk::binding(2, 0)]] StructuredBuffer<FrameCullingData> frameCullingData;

// This shader computes the min/max depth of pixels in each tile.
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const FrameCullingData frame = frameCullingData[0];
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

    // Sample pixels in this tile to find depth range
    for (uint y = tileStart.y; y < tileEnd.y; y += 2) {
        for (uint x = tileStart.x; x < tileEnd.x; x += 2) {
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
        clusterDepthBounds[tileIndex].minDepth = 1.0;
        clusterDepthBounds[tileIndex].maxDepth = 1.0;
    } else {
        clusterDepthBounds[tileIndex].minDepth = minZ;
        clusterDepthBounds[tileIndex].maxDepth = maxZ;
    }
}
