// DefaultDeformation.hlsli - Immutable graphics bridge shared by default mesh stages.
//
// The bridge is intentionally separate from the frozen 256-byte push block.
// meshDataIndex selects its bindless buffer and textureIndex selects one record;
// invalid values preserve the dormant static/bind-pose path.

#ifndef TERRAENGINE_DEFAULT_DEFORMATION_HLSLI
#define TERRAENGINE_DEFAULT_DEFORMATION_HLSLI

static const uint kInvalidAnimationGraphicsBufferIndex = 0xFFFFFFFFu;
static const uint kAnimationGraphicsRecordBytes = 64u;
static const uint kAnimationPaletteAffineBytes = 48u;
static const uint kAnimationMorphDeltaBytes = 48u;
static const uint kAnimationMaximumPaletteJoints = 256u;
static const uint kAnimationGraphicsRecordValidMarker = 1u;

struct DefaultGpuAnimationGraphicsRecord
{
	uint planGeneration;
	uint rendererGeneration;
	uint workPacketIndex;
	uint generationRecordIndex;
	uint paletteBufferIndex;
	uint morphWeightBufferIndex;
	uint2 paletteOffsetBytes;
	uint2 morphWeightOffsetBytes;
	uint jointCount;
	uint morphCount;
	uint paletteEntryCount;
	uint workFlags;
	uint stageMask;
	uint validMarker;
};

bool DefaultGpuAnimationRecordHeaderIsValid(DefaultGpuAnimationGraphicsRecord record, uint selectedRecordIndex)
{
	return record.validMarker == kAnimationGraphicsRecordValidMarker
		&& record.planGeneration != 0u
		&& record.rendererGeneration != 0u
		&& record.workPacketIndex == selectedRecordIndex
		&& record.stageMask != 0u
		&& record.jointCount <= kAnimationMaximumPaletteJoints
		&& record.paletteEntryCount <= kAnimationMaximumPaletteJoints;
}

bool DefaultGpuAnimationFinite(float value)
{
	return !isnan(value) && !isinf(value);
}

bool DefaultGpuAnimationFinite3(float3 value)
{
	return DefaultGpuAnimationFinite(value.x)
		&& DefaultGpuAnimationFinite(value.y)
		&& DefaultGpuAnimationFinite(value.z);
}

#endif // TERRAENGINE_DEFAULT_DEFORMATION_HLSLI
