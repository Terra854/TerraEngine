// AnimationRagdoll.comp.hlsl - bounded visual blend from copied physics poses.
//
// Physics remains authoritative. The animation coordinator copies one immutable,
// generation-stamped table into the frame's program-word upload; this shader
// never follows a physics pointer and never reads a result back to the CPU.
#include "AnimationCodec.hlsli"

#define TE_ANIMATION_RAGDOLL_MAGIC 0x31474152u
#define TE_ANIMATION_RAGDOLL_VERSION 1u
#define TE_ANIMATION_RAGDOLL_HEADER_BYTES 32u
#define TE_ANIMATION_RAGDOLL_RECORD_BYTES 96u
#define TE_ANIMATION_RAGDOLL_MAX_RECORDS 256u
#define TE_ANIMATION_RAGDOLL_ENABLED 0x01u

bool TeRagdollReasonableAffine(uint recordOffset)
{
	const float4 row0 = asfloat(gAnimationProgram.Load4(recordOffset + 40u));
	const float4 row1 = asfloat(gAnimationProgram.Load4(recordOffset + 56u));
	const float4 row2 = asfloat(gAnimationProgram.Load4(recordOffset + 72u));
	return TeFinite4(row0) && TeFinite4(row1) && TeFinite4(row2)
		&& all(abs(row0) <= 1.0e8f) && all(abs(row1) <= 1.0e8f)
		&& all(abs(row2) <= 1.0e8f);
}

bool TeResolveRagdollTable(
	TeAnimationGpuWorkPacket packet,
	out uint recordsOffset,
	out uint recordCount)
{
	recordsOffset = 0u;
	recordCount = 0u;
	uint programBytes;
	gAnimationProgram.GetDimensions(programBytes);
	const TeAnimationGpuGenerationRecord generation =
		gAnimationGenerations[packet.generationRecordIndex];
	const uint headerOffset = generation.reserved;
	if (headerOffset == 0u || (headerOffset & 15u) != 0u
		|| !TeRangeValid(programBytes, headerOffset, TE_ANIMATION_RAGDOLL_HEADER_BYTES)) return false;
	const uint magic = gAnimationProgram.Load(headerOffset);
	const uint versionAndStride = gAnimationProgram.Load(headerOffset + 4u);
	recordCount = gAnimationProgram.Load(headerOffset + 8u);
	const uint flags = gAnimationProgram.Load(headerOffset + 12u);
	const uint physicsGeneration = gAnimationProgram.Load(headerOffset + 16u);
	const uint skeletonGeneration = gAnimationProgram.Load(headerOffset + 20u);
	const uint componentGeneration = gAnimationProgram.Load(headerOffset + 24u);
	const uint version = versionAndStride & 0xffffu;
	const uint stride = versionAndStride >> 16u;
	if (magic != TE_ANIMATION_RAGDOLL_MAGIC || version != TE_ANIMATION_RAGDOLL_VERSION
		|| stride != TE_ANIMATION_RAGDOLL_RECORD_BYTES
		|| flags != TE_ANIMATION_RAGDOLL_ENABLED || recordCount == 0u
		|| recordCount > TE_ANIMATION_RAGDOLL_MAX_RECORDS
		|| physicsGeneration != generation.resourceGeneration
		|| skeletonGeneration != generation.skeletonGeneration
		|| componentGeneration != generation.componentGeneration) return false;
	recordsOffset = headerOffset + TE_ANIMATION_RAGDOLL_HEADER_BYTES;
	return recordsOffset >= headerOffset
		&& recordCount <= (programBytes - recordsOffset) / TE_ANIMATION_RAGDOLL_RECORD_BYTES;
}

bool TeValidateRagdollTable(
	TeAnimationGpuWorkPacket packet,
	uint recordsOffset,
	uint recordCount)
{
	uint programBytes;
	gAnimationProgram.GetDimensions(programBytes);
	const TeAnimationGpuGenerationRecord generation =
		gAnimationGenerations[packet.generationRecordIndex];
	uint previousJoint = 0u;
	[loop]
	for (uint record = 0u; record < TE_ANIMATION_RAGDOLL_MAX_RECORDS; ++record)
	{
		if (record >= recordCount) break;
		const uint offset = recordsOffset + record * TE_ANIMATION_RAGDOLL_RECORD_BYTES;
		if (!TeRangeValid(programBytes, offset, TE_ANIMATION_RAGDOLL_RECORD_BYTES)) return false;
		const uint joint = gAnimationProgram.Load(offset);
		const uint bodyGeneration = gAnimationProgram.Load(offset + 4u);
		const uint physicsGeneration = gAnimationProgram.Load(offset + 8u);
		const uint skeletonGeneration = gAnimationProgram.Load(offset + 12u);
		const uint componentGeneration = gAnimationProgram.Load(offset + 16u);
		const uint flags = gAnimationProgram.Load(offset + 20u);
		const float profileWeight = asfloat(gAnimationProgram.Load(offset + 24u));
		const float activationWeight = asfloat(gAnimationProgram.Load(offset + 28u));
		const float bodyWeight = asfloat(gAnimationProgram.Load(offset + 32u));
		// Sorted unique joints remove write races and let every invocation prove the
		// complete table in one bounded linear pass before any output is changed.
		if (joint >= packet.jointCount || (record != 0u && joint <= previousJoint)
			|| bodyGeneration == 0u || flags != 0u
			|| physicsGeneration != generation.resourceGeneration
			|| skeletonGeneration != generation.skeletonGeneration
			|| componentGeneration != generation.componentGeneration
			|| !TeFinite(profileWeight) || !TeFinite(activationWeight) || !TeFinite(bodyWeight)
			|| profileWeight < 0.0f || profileWeight > 1.0f
			|| activationWeight < 0.0f || activationWeight > 1.0f
			|| bodyWeight < 0.0f || bodyWeight > 1.0f
			|| !TeRagdollReasonableAffine(offset)) return false;
		previousJoint = joint;
	}
	return true;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch;
	TeAnimationGpuWorkPacket packet;
	uint joint;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_RAGDOLL, dispatch, packet, joint)) return;
	if (!TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_RAGDOLL)
		|| joint >= packet.jointCount) return;
	if ((packet.flags & TE_ANIMATION_GPU_WORK_SUSPEND_VISUAL_EVALUATION) != 0u) return;
	if ((packet.flags & TE_ANIMATION_GPU_WORK_ALLOW_RAGDOLL_VISUAL) == 0u
		|| (packet.flags & TE_ANIMATION_GPU_WORK_DETERMINISTIC_REFERENCE) != 0u) return;

	uint recordsOffset;
	uint recordCount;
	if (!TeResolveRagdollTable(packet, recordsOffset, recordCount)
		|| !TeValidateRagdollTable(packet, recordsOffset, recordCount))
	{
		if (joint == 0u) TeReport(dispatch, packet,
			TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD, recordsOffset, recordCount);
		return;
	}

	uint ownedRecord = 0xffffffffu;
	[loop]
	for (uint record = 0u; record < TE_ANIMATION_RAGDOLL_MAX_RECORDS; ++record)
	{
		if (record >= recordCount) break;
		const uint offset = recordsOffset + record * TE_ANIMATION_RAGDOLL_RECORD_BYTES;
		const uint candidateJoint = gAnimationProgram.Load(offset);
		if (candidateJoint == joint) { ownedRecord = offset; break; }
		if (candidateJoint > joint) break;
	}
	if (ownedRecord == 0xffffffffu) return;

	TeAnimationAffine animationMatrix;
	if (!TeLoadModel(packet.modelPoseOffsetBytes.x + joint * 48u, animationMatrix))
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, 0u);
		return;
	}
	const float weight = saturate(asfloat(gAnimationProgram.Load(ownedRecord + 24u)))
		* saturate(asfloat(gAnimationProgram.Load(ownedRecord + 28u)))
		* saturate(asfloat(gAnimationProgram.Load(ownedRecord + 32u)));
	TeAnimationAffine physicsMatrix;
	physicsMatrix.row0 = asfloat(gAnimationProgram.Load4(ownedRecord + 40u));
	physicsMatrix.row1 = asfloat(gAnimationProgram.Load4(ownedRecord + 56u));
	physicsMatrix.row2 = asfloat(gAnimationProgram.Load4(ownedRecord + 72u));
	animationMatrix.row0 = lerp(animationMatrix.row0, physicsMatrix.row0, weight);
	animationMatrix.row1 = lerp(animationMatrix.row1, physicsMatrix.row1, weight);
	animationMatrix.row2 = lerp(animationMatrix.row2, physicsMatrix.row2, weight);
	if (!TeFinite4(animationMatrix.row0) || !TeFinite4(animationMatrix.row1)
		|| !TeFinite4(animationMatrix.row2))
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_NONFINITE_OUTPUT, joint, 0u);
		return;
	}
	if (!TeStoreModel(packet.modelPoseOffsetBytes.x + joint * 48u, animationMatrix))
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, 0u);
}
