// AnimatedVirtualGeometryAbi.hlsli - Shared compute/graphics ABI.
//
// Keep this file algorithm-free. C++ planning and every shader stage use these
// constants and layouts to prevent animation/page generations from being
// inferred from transient offsets.

#ifndef TERRAENGINE_ANIMATED_VIRTUAL_GEOMETRY_ABI_HLSLI
#define TERRAENGINE_ANIMATED_VIRTUAL_GEOMETRY_ABI_HLSLI

#define TE_ANIMATED_GEOMETRY_ABI_VERSION 1u
#define TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET 2u
#define TE_ANIMATED_GEOMETRY_BINDING_GENERATIONS 0u
#define TE_ANIMATED_GEOMETRY_BINDING_WORK 1u
#define TE_ANIMATED_GEOMETRY_BINDING_STATIC_PAGE_BYTES 2u
#define TE_ANIMATED_GEOMETRY_BINDING_PALETTES 3u
#define TE_ANIMATED_GEOMETRY_BINDING_MORPH_WEIGHTS 4u
#define TE_ANIMATED_GEOMETRY_BINDING_CURRENT_VERTICES 5u
#define TE_ANIMATED_GEOMETRY_BINDING_PREVIOUS_VERTICES 6u
#define TE_ANIMATED_GEOMETRY_BINDING_MESHLET_BOUNDS 7u
#define TE_ANIMATED_GEOMETRY_BINDING_CLUSTER_BOUNDS 8u
#define TE_ANIMATED_GEOMETRY_BINDING_REFINEMENT 9u
#define TE_ANIMATED_GEOMETRY_BINDING_DIAGNOSTICS 10u
#define TE_ANIMATED_GEOMETRY_BINDING_COUNT 11u

#define TE_ANIMATED_GEOMETRY_VERTEX_BYTES 64u
#define TE_ANIMATED_GEOMETRY_BOUNDS_BYTES 64u
#define TE_ANIMATED_GEOMETRY_INVALID_INDEX 0xffffffffu

#define TE_ANIMATED_GEOMETRY_VALID_CURRENT 0x01u
#define TE_ANIMATED_GEOMETRY_VALID_HISTORY 0x02u
#define TE_ANIMATED_GEOMETRY_VALID_BOUNDS 0x04u
#define TE_ANIMATED_GEOMETRY_VALID_HIZ_PROOF 0x08u
#define TE_ANIMATED_GEOMETRY_VALID_CONE_PROOF 0x10u

#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_INVALID_RECORD 0x001u
#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_STALE_GENERATION 0x002u
#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_RANGE_OVERFLOW 0x004u
#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_NONFINITE_OUTPUT 0x008u
#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_MISSING_PAGE 0x010u
#define TE_ANIMATED_GEOMETRY_DIAGNOSTIC_BUDGET_PRESSURE 0x020u

struct TeAnimatedDeformedVertex
{
	float4 currentPosition;
	float4 previousPosition;
	float4 normal;
	float4 tangentAndSign;
};

struct TeAnimatedDynamicBounds
{
	float4 minimum;
	float4 maximum;
	float4 sphere;
	uint4 identity;
};

struct TeAnimatedGenerationRecord
{
	uint2 stableInstanceId;
	uint2 animationPublicationGeneration;
	uint2 pageContentGeneration;
	uint2 residencyGeneration;
	uint2 rendererGeneration;
	uint2 sceneGeneration;
	uint2 meshPayloadGeneration;
	uint2 reserved;
};

// One work record describes a self-contained physical page. Byte offsets are
// relative to the bound page arena; output offsets are element indices.
struct TeAnimatedDeformationWorkRecord
{
	uint generationRecordIndex;
	uint inputVertexByteOffset;
	uint skinRecordByteOffset;
	uint morphDeltaByteOffset;
	uint vertexCount;
	uint jointCount;
	uint morphTargetCount;
	uint paletteRowOffset;
	uint morphWeightOffset;
	uint currentOutputOffset;
	uint previousOutputOffset;
	uint jointPaletteByteOffset;
	uint flags;
	uint2 reserved;
	float4 positionOffset;
	float4 positionScale;
};

struct TeAnimatedDeformationPushConstants
{
	uint workRecordIndex;
	uint workRecordCount;
	uint abiVersion;
	uint reserved;
};

struct TeAnimatedRefinementRecord
{
	uint parentWorkIndex;
	uint firstChildWorkIndex;
	uint childCount;
	uint selectedWorkIndex;
	float geometricError;
	float deformationError;
	float projectedScale;
	float thresholdPixels;
	uint flags;
	uint requestFeedbackIndex;
	uint2 reserved;
};

struct TeAnimatedBoundsPushConstants
{
	uint workRecordIndex;
	uint workRecordCount;
	uint abiVersion;
	uint identity;
};

#endif
