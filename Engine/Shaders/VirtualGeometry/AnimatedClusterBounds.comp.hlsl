// AnimatedClusterBounds.comp.hlsl - exact current deformed-page bounds.
//
// One workgroup reduces one page. Each lane walks a deterministic strided
// subset, then the fixed binary reduction produces the same result regardless
// of view count or CPU worker ordering.

#include "AnimatedVirtualGeometryAbi.hlsli"

[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_WORK, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedDeformationWorkRecord> deformationWork;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_CURRENT_VERTICES, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedDeformedVertex> currentVertices;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_MESHLET_BOUNDS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<TeAnimatedDynamicBounds> meshletBounds;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_CLUSTER_BOUNDS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<TeAnimatedDynamicBounds> clusterBounds;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_DIAGNOSTICS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<uint> diagnostics;

[[vk::push_constant]]
ConstantBuffer<TeAnimatedBoundsPushConstants> boundsPush;

groupshared float3 sharedMinimum[64];
groupshared float3 sharedMaximum[64];
groupshared uint sharedValid[64];

bool Finite3(float3 value)
{
	return all(!isnan(value)) && all(!isinf(value));
}

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID)
{
	if(boundsPush.abiVersion != TE_ANIMATED_GEOMETRY_ABI_VERSION
		|| boundsPush.workRecordIndex >= boundsPush.workRecordCount)
	{
		return;
	}
	const TeAnimatedDeformationWorkRecord work =
		deformationWork[boundsPush.workRecordIndex];
	float3 minimum = float3(3.402823466e+38F, 3.402823466e+38F, 3.402823466e+38F);
	float3 maximum = -minimum;
	uint valid = 1u;
	for(uint vertex = lane; vertex < work.vertexCount; vertex += 64u)
	{
		const float3 position =
			currentVertices[work.currentOutputOffset + vertex].currentPosition.xyz;
		if(!Finite3(position))
		{
			valid = 0u;
			continue;
		}
		minimum = min(minimum, position);
		maximum = max(maximum, position);
	}
	sharedMinimum[lane] = minimum;
	sharedMaximum[lane] = maximum;
	sharedValid[lane] = valid;
	GroupMemoryBarrierWithGroupSync();

	for(uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if(lane < stride)
		{
			sharedMinimum[lane] = min(
				sharedMinimum[lane], sharedMinimum[lane + stride]);
			sharedMaximum[lane] = max(
				sharedMaximum[lane], sharedMaximum[lane + stride]);
			sharedValid[lane] &= sharedValid[lane + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if(lane != 0u)
	{
		return;
	}
	if(sharedValid[0] == 0u || work.vertexCount == 0u)
	{
		InterlockedOr(diagnostics[0],
			TE_ANIMATED_GEOMETRY_DIAGNOSTIC_NONFINITE_OUTPUT);
		return;
	}
	const float3 center = (sharedMinimum[0] + sharedMaximum[0]) * 0.5F;
	float radius = 0.0F;
	for(uint vertex = 0u; vertex < work.vertexCount; ++vertex)
	{
		radius = max(radius, distance(
			center,
			currentVertices[work.currentOutputOffset + vertex].currentPosition.xyz));
	}
	TeAnimatedDynamicBounds output;
	output.minimum = float4(sharedMinimum[0], 0.0F);
	output.maximum = float4(sharedMaximum[0], 0.0F);
	output.sphere = float4(center, radius);
	output.identity = uint4(
		boundsPush.identity,
		boundsPush.workRecordIndex,
		TE_ANIMATED_GEOMETRY_ABI_VERSION,
		TE_ANIMATED_GEOMETRY_VALID_CURRENT
			| TE_ANIMATED_GEOMETRY_VALID_BOUNDS
			| TE_ANIMATED_GEOMETRY_VALID_HIZ_PROOF);
	// Version one publishes one conservative page bound to both tables. Later
	// meshlet range metadata can subdivide it without weakening correctness.
	meshletBounds[boundsPush.workRecordIndex] = output;
	clusterBounds[boundsPush.workRecordIndex] = output;
}
