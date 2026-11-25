#pragma pack_matrix(column_major)
// Hi-Z coordinates belong to one translated view and one origin generation.
#include "../Geometry/TerraWorldReferenceFrame.hlsli"

// The Hi-Z pass owns a compact descriptor set instead of rewriting global
// renderer bindings while a command buffer is being recorded. Binding 0 is the
// source depth/mip, and binding 1 is the destination mip being generated.
[[vk::binding(0, 0)]] Texture2D<float> inputDepth;
[[vk::binding(1, 0)]] RWTexture2D<float> outputMip;

struct PushConstants
{
	uint2 inputSize; // Source depth/mip size for this pass.
	uint2 outputSize; // Destination mip size; threads outside this area do no work.
	uint occlusionState; // 1 when the next-frame task shader may use this pyramid for occlusion.
	uint invalidReason; // CPU-authored reason code when occlusionState is 0; generation still runs.
	uint2 reserved;
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
		// Level 0 always copies depth, even when occlusionState is 0. The CPU
		// may publish a fail-open reason for the current frame while still
		// warming this pyramid for a later same-view/same-target validation.
		float d = inputDepth.Load(uint3(outputCoords, 0));
		outputMip[outputCoords] = d;
		return;
	}

	// Consumers address mips by normalized UV. In an odd-sized source, each
	// destination texel overlaps a third source texel on that axis. Reducing
	// only a 2x2 block would miss coverage at those boundaries and at the edge.
	const uint2 first = outputCoords * 2u;
	const uint2 lastInput = config.inputSize - 1u;
	const bool oddWidth = config.inputSize.x > 1u && (config.inputSize.x & 1u) != 0u;
	const bool oddHeight = config.inputSize.y > 1u && (config.inputSize.y & 1u) != 0u;
	const uint2 last = min(first + uint2(oddWidth ? 2u : 1u, oddHeight ? 2u : 1u), lastInput);

	// Standard Z uses 1 for the far plane and for uncovered depth. Carry
	// the maximum forward so one uncovered pixel keeps the region visible.
	float furthestDepth = 0.0f;
	[unroll]
	for (uint y = 0u; y < 3u; ++y) {
		[unroll]
		for (uint x = 0u; x < 3u; ++x) {
			const uint2 inputCoords = first + uint2(x, y);
			if (all(inputCoords <= last)) {
				furthestDepth = max(furthestDepth, inputDepth.Load(uint3(inputCoords, 0)));
			}
		}
	}

	outputMip[outputCoords] = furthestDepth;
}
