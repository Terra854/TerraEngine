// MotionVectors.hlsli - explicit indexed-raster motion producer contract.
// This include performs no resource access and advertises no renderer capability.
// Native preparation must retain exact submitted camera, mesh, transform and
// palette owners. A producer tick or recycled descriptor is not previous state.
#ifndef TERRAENGINE_POST_PROCESS_MOTION_VECTORS_HLSLI
#define TERRAENGINE_POST_PROCESS_MOTION_VECTORS_HLSLI

static const uint kMotionInvalidSelector = 0xffffffffu;
static const uint kMotionStatic = 0u;
static const uint kMotionOwnerPalette = 1u;
static const uint kMotionViewHistoryValid = 1u;
static const uint kMotionCurrentOwnerQualified = 1u;
static const uint kMotionPreviousOwnerQualified = 2u;

// StructuredBuffer stride192; column-major matrices occupy64 bytes each.
// All UVs are normalized against the complete source image, top-left origin,
// positiveY down. Projections already include Vulkan Y convention and jitter.
struct MotionViewData
{
	column_major float4x4 currentRasterViewProjection; // Offset0.
	column_major float4x4 previousRasterViewProjection; // Offset64.
	float4 jitterUV; // Offset128: current.xy, previous.zw; actual raster shifts.
	uint4 extents; // Offset144: internalWH, outputWH; full-image positive viewport.
	uint4 recordingTokens; // Offset160: current uint64 low/high, previous low/high.
	uint4 state; // Offset176: xflags, yzwreserved0. Native exact-view qualification.
};

// StructuredBuffer stride160. Model matrices localize each canonical pose
// against its corresponding view reference frame; do not subtract float-world
// positions to recover a prior origin. Native scope/signature resets guard cuts.
struct MotionDrawData
{
	column_major float4x4 currentModel; // Offset0: current pose/current reference.
	column_major float4x4 previousModel; // Offset64: previous pose/previous reference.
	uint4 deformationSelectors; // Offset128: current bridge,row; previous bridge,row.
	uint4 deformationState; // Offset144: currentkind, previouskind, ownerflags, reserved0.
};

// Optional demand-driven camera-only packet, stride64. This is the CURRENT
// canonical object pose localized against the PREVIOUS camera reference frame,
// not the previous object pose. It keeps camera and object motion separable.
struct MotionCameraOnlyData
{
	column_major float4x4 currentModelInPreviousReference;
};

struct MotionVertexData
{
	float4 currentRasterClip;
	float4 previousRasterClip;
	uint valid;
};

bool MotionFiniteClip(float4 clip)
{
	return all(isfinite(clip)) && clip.w > 1e-7f;
}
bool MotionViewIsQualified(MotionViewData view)
{
	return (view.state.x & kMotionViewHistoryValid) != 0u
		&& all(view.extents > 0u) && all(isfinite(view.jitterUV))
		&& any(view.recordingTokens.xy != 0u) && any(view.recordingTokens.zw != 0u);
}
bool MotionSelectorsAreQualified(uint kind, uint2 selectors)
{
	if(kind == kMotionStatic) return all(selectors == kMotionInvalidSelector);
	if(kind == kMotionOwnerPalette) return all(selectors != kMotionInvalidSelector);
	return false;
}

// Call the same qualified morph-before-skin helper separately with the current
// and previous selectors before this function. Pass each helper's success; a
// failed palette lookup must not silently turn into a valid bind-pose velocity.
// Current static geometry uses identical local positions, while rigid motion
// still differs through the independently retained current/previous model.
MotionVertexData MotionTransformVertex(MotionViewData view, MotionDrawData draw,
	float3 currentLocalPosition, float3 previousLocalPosition,
	bool currentDeformationSucceeded, bool previousDeformationSucceeded)
{
	MotionVertexData result = (MotionVertexData)0;
	result.currentRasterClip = mul(view.currentRasterViewProjection,
		mul(draw.currentModel, float4(currentLocalPosition, 1.0f)));
	result.previousRasterClip = mul(view.previousRasterViewProjection,
		mul(draw.previousModel, float4(previousLocalPosition, 1.0f)));
	uint owners = kMotionCurrentOwnerQualified | kMotionPreviousOwnerQualified;
	result.valid = MotionViewIsQualified(view)
		&& (draw.deformationState.z & owners) == owners
		&& MotionSelectorsAreQualified(draw.deformationState.x, draw.deformationSelectors.xy)
		&& MotionSelectorsAreQualified(draw.deformationState.y, draw.deformationSelectors.zw)
		&& currentDeformationSucceeded && previousDeformationSucceeded
		&& all(isfinite(currentLocalPosition)) && all(isfinite(previousLocalPosition))
		&& MotionFiniteClip(result.currentRasterClip) && MotionFiniteClip(result.previousRasterClip);
	return result;
}

// Pass previousRasterClip through the ordinary perspective-correct interpolator.
// Carry float invalidity =1-valid through an ordinary perspective-correct
// interpolator (not flat/nointerpolation). Nonnegative invalidity must be zero
// at every contributing corner; one good provoking vertex cannot hide failure.
// A native draw proven wholly valid may instead pass constant zero.
// Evaluate in the fragment stage using actual SV_Position.xy, not an interpolated
// current vertex UV. This includes actual camera, rigid and deformation motion.
float4 MotionEvaluateRaster(MotionViewData view, float2 rasterPixelPosition,
	float4 interpolatedPreviousRasterClip, float interpolatedInvalidity)
{
	if(!isfinite(interpolatedInvalidity) || interpolatedInvalidity != 0.0f || !MotionViewIsQualified(view)
		|| !MotionFiniteClip(interpolatedPreviousRasterClip)
		|| !all(isfinite(rasterPixelPosition))) return float4(0, 0, 0, 0);
	float2 currentRasterUV = rasterPixelPosition / float2(view.extents.xy);
	float3 previousNdc = interpolatedPreviousRasterClip.xyz / interpolatedPreviousRasterClip.w;
	// The actual projection already flips Y for positive-height Vulkan viewport.
	// Applying another image-space flip here would invert vertical motion.
	float2 previousRasterUV = previousNdc.xy * 0.5f + 0.5f;
	if(!all(isfinite(previousNdc)) || any(currentRasterUV < 0) || any(currentRasterUV > 1)
		|| any(previousRasterUV < 0) || any(previousRasterUV > 1)
		|| previousNdc.z < 0 || previousNdc.z > 1) return float4(0, 0, 0, 0);
	// z is the expected PREVIOUS Vulkan normalized depth, including reversedZ
	// when that is the actual projection. The resolve compares the same domain.
	return float4(previousRasterUV - currentRasterUV, previousNdc.z, 1.0f);
}

// Optional camera-only blur motion: use CURRENT deformed local position against
// CURRENT canonical object transform expressed in PREVIOUS camera reference.
// This excludes object/skin displacement but includes actual camera projection.
// The caller uses the same fragment evaluator and native validity as total motion.
float4 MotionCameraOnlyPreviousClip(MotionViewData view, MotionCameraOnlyData cameraOnly,
	float3 currentDeformedLocalPosition)
{
	return mul(view.previousRasterViewProjection,
		mul(cameraOnly.currentModelInPreviousReference, float4(currentDeformedLocalPosition, 1.0f)));
}

// Camera-only validity needs current geometry and a prior accepted camera, but
// does not require a previous object palette. A newly spawned object can still
// have meaningful camera blur if the native current-owner proof is complete.
MotionVertexData MotionTransformCameraOnlyVertex(MotionViewData view, MotionDrawData draw,
	MotionCameraOnlyData cameraOnly, float3 currentDeformedLocalPosition,
	bool currentDeformationSucceeded)
{
	MotionVertexData result = (MotionVertexData)0;
	result.currentRasterClip = mul(view.currentRasterViewProjection,
		mul(draw.currentModel, float4(currentDeformedLocalPosition, 1.0f)));
	result.previousRasterClip = MotionCameraOnlyPreviousClip(view, cameraOnly, currentDeformedLocalPosition);
	result.valid = MotionViewIsQualified(view)
		&& (draw.deformationState.z & kMotionCurrentOwnerQualified) != 0u
		&& MotionSelectorsAreQualified(draw.deformationState.x, draw.deformationSelectors.xy)
		&& currentDeformationSucceeded && all(isfinite(currentDeformedLocalPosition))
		&& MotionFiniteClip(result.currentRasterClip) && MotionFiniteClip(result.previousRasterClip);
	return result;
}

// Blur may want physical motion without raster jitter. TemporalResolve consumes
// the original raster payload and must NOT call this helper: it already removes
// previous jitter once when mapping into its unjittered history image.
float4 MotionWithoutRasterJitter(float4 rasterMotion, MotionViewData view)
{
	if(rasterMotion.w <= 0 || !all(isfinite(rasterMotion)) || !all(isfinite(view.jitterUV))) return float4(0, 0, 0, 0);
	rasterMotion.xy += view.jitterUV.xy - view.jitterUV.zw;
	return rasterMotion;
}
#endif
