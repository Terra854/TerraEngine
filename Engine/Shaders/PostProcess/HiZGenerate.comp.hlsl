#pragma pack_matrix(column_major)

// Using binding 12 for input (can be the D32 depth buffer OR an R32F Hi-Z mip)
[[vk::binding(12, 0)]] Texture2D<float> inputDepth;
// Using binding 16 for output
[[vk::binding(16, 0)]] RWTexture2D<float> outputMip;

struct PushConstants
{
	uint2 inputSize;
    uint isLevel0; // 1 if converting from D32 to R32F, 0 if reducing R32F
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 outputCoords = dispatchThreadId.xy;
    
    if (config.isLevel0 != 0) {
        // Level 0: Direct copy/conversion from depth buffer
        if (all(outputCoords < config.inputSize)) {
            float d = inputDepth.Load(uint3(outputCoords, 0));
            outputMip[outputCoords] = d;
        }
        return;
    }

    // Mip reduction: Sample 2x2 area of previous level
	uint2 inputCoords = outputCoords * 2;

	// For conservative occlusion culling in Standard Z (0=Near, 1=Far):
	// We want the furthest depth in the region to test against.
	// Furthest depth in Standard Z is the MAXIMUM value.
	float d0 = inputDepth.Load(uint3(inputCoords + uint2(0, 0), 0));
	float d1 = inputDepth.Load(uint3(inputCoords + uint2(1, 0), 0));
	float d2 = inputDepth.Load(uint3(inputCoords + uint2(0, 1), 0));
	float d3 = inputDepth.Load(uint3(inputCoords + uint2(1, 1), 0));

	float furthestDepth = max(max(d0, d1), max(d2, d3));

	outputMip[outputCoords] = furthestDepth;
}
