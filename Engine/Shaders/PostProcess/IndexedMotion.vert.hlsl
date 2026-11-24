// Actual opaque indexed motion raster, using the same retained TEMF owner
// streams and morph-before-skin arithmetic as DefaultIndexed.vert.hlsl.
// Native requirements: vertexOffset0, instanceCount1, firstInstance=private draw
// row; exact source viewport/projection, current depth LOAD/read-only/EQUAL and
// matching source cull/front-face state. Alpha-mask/transparent/page/grouped/GPU
// authority routes are unqualified until separately implemented and tested.
#pragma pack_matrix(column_major)
#include "IndexedMotionPrivate.hlsli"
#include "IndexedMotionGeometry.hlsli"
#include "IndexedMotionDeformation.hlsli"

IndexedMotionVertexOutput MotionRejectVertex()
{
	IndexedMotionVertexOutput output = (IndexedMotionVertexOutput)0;
	output.position = float4(0, 0, -1, 1); output.invalidity = 1.0f;
	output.previousRasterClip = float4(0, 0, 0, 1);
#if INDEXED_MOTION_CAMERA_ONLY
	output.cameraInvalidity = 1.0f; output.cameraPreviousRasterClip = float4(0, 0, 0, 1);
#endif
	return output;
}
IndexedMotionVertexOutput main(uint cornerToken : SV_VertexID, uint drawIndex : SV_InstanceID)
{
	IndexedMotionVertexOutput rejected = MotionRejectVertex();
	uint viewCount, viewStride, drawCount, drawStride;
	indexedMotionViews.GetDimensions(viewCount, viewStride); indexedMotionDraws.GetDimensions(drawCount, drawStride);
	if(viewCount != 1u || viewStride != 192u || drawIndex >= drawCount || drawStride != 160u) return rejected;
	MotionViewData view = indexedMotionViews[0]; MotionDrawData draw = indexedMotionDraws[drawIndex];
	if(any(view.state.yzw != 0u) || (view.state.x & ~1u) != 0u || draw.deformationState.w != 0u
		|| (draw.deformationState.z & ~7u) != 0u || (draw.deformationState.z & kMotionCurrentOwnerQualified) == 0u) return rejected;
	IndexedMotionDecode decode; uint owner; TeDecodedPackedVertex vertex;
	if(!MotionLoadOwnerVertex(cornerToken, decode, owner, vertex)) return rejected;
	float3 basePosition = decode.positionOffset.xyz + vertex.normalizedPosition * decode.positionScale.xyz;
	float3 currentPosition, previousPosition = basePosition;
	bool currentOkay = MotionDeformPosition(draw.deformationState.x, draw.deformationSelectors.xy,
		decode, owner, basePosition, vertex.normal, vertex.tangent, currentPosition);
	if(!currentOkay) return rejected; // Never substitute a cooked bind pose.
	bool previousOkay = false;
	if((draw.deformationState.z & kMotionPreviousOwnerQualified) != 0u)
		previousOkay = MotionDeformPosition(draw.deformationState.y, draw.deformationSelectors.zw,
			decode, owner, basePosition, vertex.normal, vertex.tangent, previousPosition);
	MotionVertexData motion = MotionTransformVertex(view, draw, currentPosition, previousPosition, currentOkay, previousOkay);
	if(!all(isfinite(motion.currentRasterClip))) return rejected;
	IndexedMotionVertexOutput output = (IndexedMotionVertexOutput)0;
	output.position = motion.currentRasterClip;
	output.previousRasterClip = motion.valid != 0u ? motion.previousRasterClip : float4(0, 0, 0, 1);
	output.invalidity = 1.0f - float(motion.valid);
#if INDEXED_MOTION_CAMERA_ONLY
	output.cameraPreviousRasterClip = float4(0, 0, 0, 1); output.cameraInvalidity = 1.0f;
	uint cameraCount, cameraStride; indexedMotionCameras.GetDimensions(cameraCount, cameraStride);
	if(drawIndex < cameraCount && cameraStride == 64u && (draw.deformationState.z & kIndexedMotionCameraReferenceQualified) != 0u) {
		MotionVertexData camera = MotionTransformCameraOnlyVertex(view, draw, indexedMotionCameras[drawIndex], currentPosition, currentOkay);
		output.cameraPreviousRasterClip = camera.valid != 0u ? camera.previousRasterClip : float4(0, 0, 0, 1);
		output.cameraInvalidity = 1.0f - float(camera.valid);
	}
#endif
	return output;
}
