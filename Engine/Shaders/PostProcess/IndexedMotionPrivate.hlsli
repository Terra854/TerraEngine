// IndexedMotionPrivate.hlsli - immutable private motion-upload ABI.
#ifndef TERRAENGINE_INDEXED_MOTION_PRIVATE_HLSLI
#define TERRAENGINE_INDEXED_MOTION_PRIVATE_HLSLI
#include "MotionVectors.hlsli"
#ifndef INDEXED_MOTION_CAMERA_ONLY
#define INDEXED_MOTION_CAMERA_ONLY 1
#endif

// Exact native upload records:32-byte pose row and48-byte affine palette row.
// Offsets are ELEMENT indices, never byte offsets. Both accepted previous and
// current values live in the same immutable recording owner, with no old frame
// slot descriptors or global animation buffers borrowed by these shaders.
struct IndexedMotionPose
{
	uint paletteOffset;
	uint paletteCount;
	uint morphOffset;
	uint morphCount;
	uint jointCount;
	uint validMarker;
	uint reserved0;
	uint reserved1;
};
struct IndexedMotionPalette
{
	float4 row0;
	float4 row1;
	float4 row2;
};
[[vk::binding(0,2)]] StructuredBuffer<MotionViewData> indexedMotionViews;
[[vk::binding(1,2)]] StructuredBuffer<MotionDrawData> indexedMotionDraws;
[[vk::binding(2,2)]] StructuredBuffer<MotionCameraOnlyData> indexedMotionCameras;
[[vk::binding(3,2)]] StructuredBuffer<IndexedMotionPose> indexedMotionPoses;
[[vk::binding(4,2)]] StructuredBuffer<IndexedMotionPalette> indexedMotionPalettes;
[[vk::binding(5,2)]] StructuredBuffer<float> indexedMotionMorphWeights;

static const uint kIndexedMotionCameraReferenceQualified = 4u;
struct IndexedMotionVertexOutput
{
	float4 position : SV_Position;
	float4 previousRasterClip : TEXCOORD0;
#if INDEXED_MOTION_CAMERA_ONLY
	float4 cameraPreviousRasterClip : TEXCOORD1;
#endif
	// Smooth nonnegative invalidity: a failed neighboring corner cannot be
	// concealed by the provoking vertex of an otherwise covered triangle.
	float invalidity : TEXCOORD2;
#if INDEXED_MOTION_CAMERA_ONLY
	float cameraInvalidity : TEXCOORD3;
#endif
};
#endif
