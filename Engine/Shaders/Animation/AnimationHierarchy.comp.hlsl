// AnimationHierarchy.comp.hlsl - Bounded topology-aware local-to-model evaluation.
#include "AnimationCodec.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint joint;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_HIERARCHY, dispatch, packet, joint)) return;
	if (!TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_HIERARCHY) || joint >= packet.jointCount) return;
	if ((packet.flags & TE_ANIMATION_GPU_WORK_SUSPEND_VISUAL_EVALUATION) != 0u) return;
	uint skeletonBytes; gAnimationSkeleton.GetDimensions(skeletonBytes);
	// A skeleton directory is four uints per resource: parent-table byte offset,
	// per-joint depth-table byte offset, level count, and palette-table offset.
	// Parents are int32 and depths are uint32.
	uint directory = 0u;
	TeAnimationAffine result = (TeAnimationAffine)0;
	bool valid = packet.skeletonResourceIndex != TE_ANIMATION_INVALID_RESOURCE
		&& packet.skeletonResourceIndex <= 0xffffffffu / 16u;
	uint parentTable = 0u; uint levelTable = 0u; uint levelCount = 0u;
	if (valid)
	{
		directory = packet.skeletonResourceIndex * 16u;
		valid = TeRangeValid(skeletonBytes, directory, 16u);
	}
	if (valid)
	{
		parentTable = gAnimationSkeleton.Load(directory);
		levelTable = gAnimationSkeleton.Load(directory + 4u);
		levelCount = gAnimationSkeleton.Load(directory + 8u);
		valid = levelCount != 0u
			&& levelCount <= TE_ANIMATION_MAX_PARENT_DEPTH
			&& packet.jointCount <= 0xffffffffu / 4u
			&& TeRangeValid(skeletonBytes, parentTable, packet.jointCount * 4u)
			&& TeRangeValid(skeletonBytes, levelTable, packet.jointCount * 4u);
	}
	// An explicit depth per joint supports depth-first and breadth-first imported
	// layouts. Parent indices remain topological, while decreasing sibling depths
	// no longer reject an otherwise valid branched rig.
	uint jointLevel = valid ? gAnimationSkeleton.Load(levelTable + joint * 4u) : 0xffffffffu;
	if (jointLevel >= levelCount) valid = false;
	TeAnimationPose local; if (!TeLoadLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, local)) valid = false;
	result = TeAffineFromPose(local);
	uint current = joint;
	[loop] for (uint depth = 0u; depth < TE_ANIMATION_MAX_PARENT_DEPTH; ++depth)
	{
		if (!valid) break;
		const int parent = asint(gAnimationSkeleton.Load(parentTable + current * 4u));
		if (parent < 0)
		{
			// A root must be in depth zero. This closes the parent walk with an
			// explicit topology proof instead of trusting a malformed depth table.
			if (jointLevel != 0u) valid = false;
			break;
		}
		if (uint(parent) >= packet.jointCount) { valid = false; break; }
		// Parent indices must be topological and exactly one depth earlier. This
		// rejects cycles and malformed shortcuts without assuming contiguous levels.
		const uint parentLevel = gAnimationSkeleton.Load(
			levelTable + uint(parent) * 4u);
		if (uint(parent) >= current || jointLevel == 0u
			|| parentLevel + 1u != jointLevel) { valid = false; break; }
		TeAnimationPose parentLocal;
		if (!TeLoadLocalPose(packet.localPoseOffsetBytes.x + uint(parent) * 48u, parentLocal)) { valid = false; break; }
		result = TeMultiplyAffine(TeAffineFromPose(parentLocal), result);
		current = uint(parent);
		jointLevel = parentLevel;
		if (depth + 1u == TE_ANIMATION_MAX_PARENT_DEPTH) valid = false;
	}
	if (!valid || !TeFinite4(result.row0) || !TeFinite4(result.row1) || !TeFinite4(result.row2))
	{
		result = TeAffineFromPose(TeIdentityPose());
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, joint, levelCount);
	}
	if (!TeStoreModel(packet.modelPoseOffsetBytes.x + joint * 48u, result)) TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, 0u);
}
