#pragma pack_matrix(column_major)

// The Hi-Z pass owns a compact descriptor set instead of rewriting global
// renderer bindings while a command buffer is being recorded. Binding 0 is the
// source depth/mip, and binding 1 is the destination mip being generated.
[[vk::binding(0, 0)]] Texture2D<float> inputDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> outputMip;

struct PushConstants
{
	uint2 inputSize; // Source depth/mip size for this pass.
	uint2 outputSize; // Destination mip size; threads outside this area do no work.
};

[[vk::push_constant]] ConstantBuffer<PushConstants> config;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	uint2 outputCoords = dispatchThreadId.xy;
	if (any(outputCoords >= config.outputSize)) {
		return;
	}

	if (all(config.inputSize == config.outputSize)) {
		// Level 0: Direct copy/conversion from depth buffer
		float d = inputDepth.Load(uint3(outputCoords, 0));
		outputMip[outputCoords] = d;
		return;
	}

	// Mip reduction: sample a 2x2 area of the previous level. Odd-sized mips
	// need clamping so the final texel never reads outside the source image.
	uint2 inputCoords = outputCoords * 2;
	uint2 maxInputCoords = max(config.inputSize, uint2(1, 1)) - uint2(1, 1);

	// For conservative occlusion culling in Standard Z (0=Near, 1=Far):
	// We want the furthest depth in the region to test against.
	// Furthest depth in Standard Z is the MAXIMUM value.
	float d0 = inputDepth.Load(uint3(min(inputCoords + uint2(0, 0), maxInputCoords), 0));
	float d1 = inputDepth.Load(uint3(min(inputCoords + uint2(1, 0), maxInputCoords), 0));
	float d2 = inputDepth.Load(uint3(min(inputCoords + uint2(0, 1), maxInputCoords), 0));
	float d3 = inputDepth.Load(uint3(min(inputCoords + uint2(1, 1), maxInputCoords), 0));

	float furthestDepth = max(max(d0, d1), max(d2, d3));

	outputMip[outputCoords] = furthestDepth;
}
