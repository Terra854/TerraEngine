// Raw motion/validity attachments only, before temporal resolve or camera blur.
// The native pipeline must use the actual current depth attachment with EQUAL
// testing and no writes; passing that test ties each fragment to scene coverage.
// Invalid motion is explicit zero validity, never a fabricated camera fallback.
#include "IndexedMotionPrivate.hlsli"
struct IndexedMotionFragmentOutput
{
	float4 motion : SV_Target0;
#if INDEXED_MOTION_CAMERA_ONLY
	float4 cameraOnly : SV_Target1;
#endif
};
IndexedMotionFragmentOutput main(IndexedMotionVertexOutput input)
{
	IndexedMotionFragmentOutput output = (IndexedMotionFragmentOutput)0;
	uint count, stride; indexedMotionViews.GetDimensions(count, stride);
	if(count != 1u || stride != 192u) return output;
	MotionViewData view = indexedMotionViews[0];
	output.motion = MotionEvaluateRaster(view, input.position.xy, input.previousRasterClip, input.invalidity);
#if INDEXED_MOTION_CAMERA_ONLY
	output.cameraOnly = MotionEvaluateRaster(view, input.position.xy, input.cameraPreviousRasterClip, input.cameraInvalidity);
#endif
	// Both attachments retain raster jitter. The temporal consumer removes it
	// once when addressing history; optional blur removes it through the common
	// MotionWithoutRasterJitter helper, independently for total/camera motion.
	return output;
}
