// AnimatedRefinement.comp.hlsl - current-generation fine page selection.

#include "AnimatedVirtualGeometryAbi.hlsli"

[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_CLUSTER_BOUNDS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
StructuredBuffer<TeAnimatedDynamicBounds> clusterBounds;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_REFINEMENT, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<TeAnimatedRefinementRecord> refinementRecords;
[[vk::binding(TE_ANIMATED_GEOMETRY_BINDING_DIAGNOSTICS, TE_ANIMATED_GEOMETRY_DESCRIPTOR_SET)]]
RWStructuredBuffer<uint> diagnostics;

[[vk::push_constant]]
ConstantBuffer<TeAnimatedBoundsPushConstants> refinementPush;

static const uint kRefinementChildrenResident = 0x01u;
static const uint kRefinementBoundsCurrent = 0x02u;
static const uint kRefinementRequestMissingChildren = 0x04u;

[numthreads(64, 1, 1)]
void main(uint index : SV_DispatchThreadID)
{
	if(refinementPush.abiVersion != TE_ANIMATED_GEOMETRY_ABI_VERSION
		|| index >= refinementPush.workRecordCount)
	{
		return;
	}
	TeAnimatedRefinementRecord record = refinementRecords[index];
	const TeAnimatedDynamicBounds bounds = clusterBounds[index];
	const bool currentProof =
		(bounds.identity.w & (TE_ANIMATED_GEOMETRY_VALID_CURRENT
			| TE_ANIMATED_GEOMETRY_VALID_BOUNDS))
		== (TE_ANIMATED_GEOMETRY_VALID_CURRENT
			| TE_ANIMATED_GEOMETRY_VALID_BOUNDS);
	const float effectiveError =
		max(0.0F, record.geometricError)
		+ max(0.0F, record.deformationError);
	const float projectedError = effectiveError * max(0.0F, record.projectedScale);
	if(!currentProof)
	{
		// Invalid proof fails open to the already covering parent and asks the
		// streamer for missing detail rather than rejecting visible geometry.
		record.selectedWorkIndex = record.parentWorkIndex;
		record.flags |= kRefinementRequestMissingChildren;
		InterlockedOr(diagnostics[0],
			TE_ANIMATED_GEOMETRY_DIAGNOSTIC_STALE_GENERATION);
	}
	else if(projectedError > record.thresholdPixels
		&& record.childCount != 0u
		&& (record.flags & kRefinementChildrenResident) != 0u)
	{
		record.selectedWorkIndex = record.firstChildWorkIndex;
		record.flags |= kRefinementBoundsCurrent;
	}
	else
	{
		record.selectedWorkIndex = record.parentWorkIndex;
		if(projectedError > record.thresholdPixels)
		{
			record.flags |= kRefinementRequestMissingChildren;
		}
	}
	refinementRecords[index] = record;
}
