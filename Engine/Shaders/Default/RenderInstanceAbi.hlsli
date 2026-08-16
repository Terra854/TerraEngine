// RenderInstanceAbi.hlsli - CPU/GPU layout for grouped renderer instances.
//
// The record is deliberately a plain 176-byte StructuredBuffer element. The
// Vulkan owner binds it only after every requested record is initialized, and
// shaders validate the selector before consuming per-instance state.

#ifndef TERRAENGINE_RENDER_INSTANCE_ABI_HLSLI
#define TERRAENGINE_RENDER_INSTANCE_ABI_HLSLI
#include "../Geometry/TerraWorldReferenceFrame.hlsli"

static const uint kRenderInstanceRecordBytes = 176u;
static const uint kRenderInstanceRecordWords = 44u;
static const uint kRenderInstanceInvalidBufferIndex = 0xffffffffu;
static const uint kRenderInstanceFlagFallback = 0x1u;
static const uint kRenderInstanceFlagFailOpen = 0x2u;

struct RenderInstanceRecord
{
	column_major float4x4 modelMatrix;
	float4 editorTint;
	uint4 objectIdentity;
	uint4 materialAndTexture;
	uint4 animationSelectors;
	uint4 geometrySelectors;
	uint4 virtualGeometrySelectors;
	uint4 virtualGeometryPayloadSelectors;
};

// The default bindless heap is exposed as uint words. This decoder keeps the
// byte-range proof next to the load so a stale push selector fails open before
// it can reach another instance's transform or animation bridge.
bool TryLoadRenderInstance(
	StructuredBuffer<uint> instanceWords,
	uint instanceIndex,
	uint instanceCount,
	out RenderInstanceRecord record)
{
	record = (RenderInstanceRecord)0;
	if (instanceIndex >= instanceCount
		|| instanceIndex > (0xffffffffu - kRenderInstanceRecordWords)
			/ kRenderInstanceRecordWords)
	{
		return false;
	}
	uint elementCount = 0u;
	uint stride = 0u;
	instanceWords.GetDimensions(elementCount, stride);
	const uint wordOffset = instanceIndex * kRenderInstanceRecordWords;
	if (wordOffset > elementCount || kRenderInstanceRecordWords > elementCount - wordOffset)
	{
		return false;
	}
	// GLM writes the CPU matrix as four contiguous columns, while this HLSL
	// matrix index addresses one logical row. Reconstructing each HLSL row from
	// the matching component of every CPU column preserves affine translation in
	// `.w`; assigning a packed CPU column directly would transpose the matrix
	// and turn translation into a projective bottom row.
	[unroll]
	for (uint row = 0u; row < 4u; ++row)
	{
		record.modelMatrix[row] = asfloat(uint4(
			instanceWords[wordOffset + 0u + row],
			instanceWords[wordOffset + 4u + row],
			instanceWords[wordOffset + 8u + row],
			instanceWords[wordOffset + 12u + row]));
	}
	record.editorTint = asfloat(uint4(
		instanceWords[wordOffset + 16u], instanceWords[wordOffset + 17u],
		instanceWords[wordOffset + 18u], instanceWords[wordOffset + 19u]));
	record.objectIdentity = uint4(instanceWords[wordOffset + 20u], instanceWords[wordOffset + 21u],
		instanceWords[wordOffset + 22u], instanceWords[wordOffset + 23u]);
	record.materialAndTexture = uint4(instanceWords[wordOffset + 24u], instanceWords[wordOffset + 25u],
		instanceWords[wordOffset + 26u], instanceWords[wordOffset + 27u]);
	record.animationSelectors = uint4(instanceWords[wordOffset + 28u], instanceWords[wordOffset + 29u],
		instanceWords[wordOffset + 30u], instanceWords[wordOffset + 31u]);
	record.geometrySelectors = uint4(instanceWords[wordOffset + 32u], instanceWords[wordOffset + 33u],
		instanceWords[wordOffset + 34u], instanceWords[wordOffset + 35u]);
	record.virtualGeometrySelectors = uint4(instanceWords[wordOffset + 36u], instanceWords[wordOffset + 37u],
		instanceWords[wordOffset + 38u], instanceWords[wordOffset + 39u]);
	record.virtualGeometryPayloadSelectors = uint4(instanceWords[wordOffset + 40u], instanceWords[wordOffset + 41u],
		instanceWords[wordOffset + 42u], instanceWords[wordOffset + 43u]);
	return true;
}

#endif
