// The native motion pass reuses current opaque depth with an EQUAL test.
// This stage evaluates history from the interpolated positions emitted by
// the mesh shader, including any deformation in its authored source.
#include "MotionVectors.hlsli"

struct MeshTemporalVertex
{
	float4 position : SV_Position;
	float4 previousRasterClip : TEXCOORD0;
	float invalidity : TEXCOORD2;
};

[[vk::binding(0, 2)]] StructuredBuffer<MotionViewData> meshTemporalViews;

float4 main(MeshTemporalVertex input) : SV_Target0
{
	uint count, stride;
	meshTemporalViews.GetDimensions(count, stride);
	if(count != 1u || stride != 192u) return float4(0, 0, 0, 0);
	return MotionEvaluateRaster(meshTemporalViews[0], input.position.xy,
		input.previousRasterClip, input.invalidity);
}
