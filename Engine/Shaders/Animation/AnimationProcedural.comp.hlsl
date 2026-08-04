// AnimationProcedural.comp.hlsl - Bounded visual-only procedural pose adjustment.
//
// The CPU evaluator remains authoritative for gameplay, physics, recording,
// networking, and rollback. This stage consumes only compact immutable records,
// writes no readback resource, and leaves the hierarchy pose untouched whenever
// the whole table cannot be validated before any operation is selected.

#include "AnimationCodec.hlsli"

#define TE_ANIMATION_PROCEDURAL_VERSION 1u
#define TE_ANIMATION_PROCEDURAL_RECORD_BYTES 96u
#define TE_ANIMATION_PROCEDURAL_MAX_RECORDS 64u
#define TE_ANIMATION_PROCEDURAL_TWO_BONE 0u
#define TE_ANIMATION_PROCEDURAL_AIM 1u
#define TE_ANIMATION_PROCEDURAL_COPY_MODEL 2u
#define TE_ANIMATION_PROCEDURAL_OFFSET_MODEL 3u
#define TE_ANIMATION_PROCEDURAL_RETARGET_MODEL 4u
#define TE_ANIMATION_PROCEDURAL_RELEVANT_FLAGS (TE_ANIMATION_GPU_WORK_HYBRID_CPU_AUTHORITY | TE_ANIMATION_GPU_WORK_DETERMINISTIC_REFERENCE)

float3 TeProceduralTranslation(TeAnimationAffine value)
{
	return float3(value.row0.w, value.row1.w, value.row2.w);
}

void TeProceduralSetTranslation(inout TeAnimationAffine value, float3 translation)
{
	value.row0.w = translation.x;
	value.row1.w = translation.y;
	value.row2.w = translation.z;
}

bool TeProceduralReasonable3(float3 value)
{
	return TeFinite3(value) && all(abs(value) <= float3(1000000.0f, 1000000.0f, 1000000.0f));
}

bool TeProceduralTryNormalize(float3 value, out float3 normalized)
{
	normalized = float3(1.0f, 0.0f, 0.0f);
	const float lengthSquared = dot(value, value);
	if (!TeFinite3(value) || !TeFinite(lengthSquared) || lengthSquared <= 1.0e-12f) return false;
	normalized = value * rsqrt(lengthSquared);
	return TeFinite3(normalized);
}

float3 TeProceduralNormalizeOr(float3 value, float3 fallback)
{
	float3 normalized;
	if (TeProceduralTryNormalize(value, normalized)) return normalized;
	if (TeProceduralTryNormalize(fallback, normalized)) return normalized;
	return float3(1.0f, 0.0f, 0.0f);
}

float3 TeProceduralPerpendicular(float3 axis)
{
	const float3 normalized = TeProceduralNormalizeOr(axis, float3(1.0f, 0.0f, 0.0f));
	const float3 cardinal = abs(normalized.x) <= abs(normalized.y) && abs(normalized.x) <= abs(normalized.z)
		? float3(1.0f, 0.0f, 0.0f)
		: (abs(normalized.y) <= abs(normalized.z)
			? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f));
	return TeProceduralNormalizeOr(cross(normalized, cardinal), float3(0.0f, 1.0f, 0.0f));
}

bool TeProceduralTouches(uint recordOffset, uint joint)
{
	const uint kind = gAnimationProgram.Load(recordOffset);
	if (gAnimationProgram.Load(recordOffset + 4u) == joint) return true;
	return kind == TE_ANIMATION_PROCEDURAL_TWO_BONE
		&& (gAnimationProgram.Load(recordOffset + 8u) == joint
			|| gAnimationProgram.Load(recordOffset + 12u) == joint);
}

bool TeProceduralRecordsOverlap(uint leftOffset, uint rightOffset)
{
	const uint rightKind = gAnimationProgram.Load(rightOffset);
	if (TeProceduralTouches(leftOffset, gAnimationProgram.Load(rightOffset + 4u))) return true;
	return rightKind == TE_ANIMATION_PROCEDURAL_TWO_BONE
		&& (TeProceduralTouches(leftOffset, gAnimationProgram.Load(rightOffset + 8u))
			|| TeProceduralTouches(leftOffset, gAnimationProgram.Load(rightOffset + 12u)));
}

bool TeValidateProceduralRecord(
	uint recordOffset,
	uint programBytes,
	TeAnimationGpuWorkPacket packet,
	TeAnimationGpuGenerationRecord generation)
{
	if (!TeRangeValid(programBytes, recordOffset, TE_ANIMATION_PROCEDURAL_RECORD_BYTES)) return false;
	const uint kind = gAnimationProgram.Load(recordOffset);
	const uint ownerJoint = gAnimationProgram.Load(recordOffset + 4u);
	const uint middleJoint = gAnimationProgram.Load(recordOffset + 8u);
	const uint endJoint = gAnimationProgram.Load(recordOffset + 12u);
	const uint programGeneration = gAnimationProgram.Load(recordOffset + 16u);
	const uint skeletonGeneration = gAnimationProgram.Load(recordOffset + 20u);
	const uint componentGeneration = gAnimationProgram.Load(recordOffset + 24u);
	const uint flags = gAnimationProgram.Load(recordOffset + 28u);
	const float4 data0 = asfloat(gAnimationProgram.Load4(recordOffset + 32u));
	const float4 data1 = asfloat(gAnimationProgram.Load4(recordOffset + 48u));
	const float4 data2 = asfloat(gAnimationProgram.Load4(recordOffset + 64u));
	const float4 data3 = asfloat(gAnimationProgram.Load4(recordOffset + 80u));
	if (kind > TE_ANIMATION_PROCEDURAL_RETARGET_MODEL || ownerJoint >= packet.jointCount
		|| programGeneration != generation.programGeneration
		|| skeletonGeneration != generation.skeletonGeneration
		|| componentGeneration != generation.componentGeneration || flags != 0u
		|| !TeFinite4(data0) || !TeFinite4(data1) || !TeFinite4(data2) || !TeFinite4(data3)
		|| data3.w < 0.0f || data3.w > 1.0f) return false;
	if (kind == TE_ANIMATION_PROCEDURAL_TWO_BONE)
	{
		return middleJoint < packet.jointCount && endJoint < packet.jointCount
			&& ownerJoint != middleJoint && ownerJoint != endJoint && middleJoint != endJoint
			&& TeProceduralReasonable3(data0.xyz) && TeProceduralReasonable3(data1.xyz)
			&& data2.x >= 0.0f && data2.y >= data2.x && data2.y <= 3.141592654f
			&& abs(data2.z) <= 3.141592654f && data2.w >= 0.0f && data2.w <= 10.0f
			&& data3.x >= 1.0f && data3.x <= 16.0f
			&& (data3.y == 0.0f || data3.y == 1.0f);
	}
	if (kind == TE_ANIMATION_PROCEDURAL_AIM)
	{
		return TeProceduralReasonable3(data0.xyz) && TeProceduralReasonable3(data1.xyz)
			&& dot(data1.xyz, data1.xyz) > 1.0e-12f
			&& data1.w >= 0.0f && data1.w <= 3.141592654f;
	}
	if (kind == TE_ANIMATION_PROCEDURAL_COPY_MODEL)
	{
		return TeProceduralReasonable3(data0.xyz) && TeProceduralReasonable3(data1.xyz)
			&& TeProceduralReasonable3(data2.xyz)
			&& TeProceduralReasonable3(float3(data0.w, data1.w, data2.w));
	}
	if (kind == TE_ANIMATION_PROCEDURAL_RETARGET_MODEL)
	{
		// Retarget lowering supplies a target translation, an explicit proportion
		// scale, and an orthogonal target basis. Reject a corrupt basis before any
		// lane writes so a visual pass cannot publish a partially adjusted rig.
		const float3 basisX = data1.xyz;
		const float3 basisY = data2.xyz;
		const float3 basisZ = data3.xyz;
		return TeProceduralReasonable3(data0.xyz) && data0.w > 0.0f && data0.w <= 16.0f
			&& TeProceduralReasonable3(basisX) && TeProceduralReasonable3(basisY)
			&& TeProceduralReasonable3(basisZ)
			&& dot(basisX, basisX) > 1.0e-8f && dot(basisY, basisY) > 1.0e-8f
			&& dot(basisZ, basisZ) > 1.0e-8f;
	}
	return TeProceduralReasonable3(data0.xyz);
}

bool TeResolveProceduralTable(
	TeAnimationGpuWorkPacket packet,
	out uint tableOffset,
	out uint recordCount)
{
	tableOffset = 0u;
	recordCount = 0u;
	uint programBytes;
	gAnimationProgram.GetDimensions(programBytes);
	const uint directoryOffset = packet.programResourceIndex * 64u;
	if (packet.programResourceIndex == TE_ANIMATION_INVALID_RESOURCE
		|| !TeRangeValid(programBytes, directoryOffset, 16u)) return false;
	tableOffset = gAnimationProgram.Load(directoryOffset + 4u);
	recordCount = gAnimationProgram.Load(directoryOffset + 8u);
	const uint versionAndStride = gAnimationProgram.Load(directoryOffset + 12u);
	const uint version = versionAndStride & 0xffffu;
	const uint stride = versionAndStride >> 16u;
	if (version != TE_ANIMATION_PROCEDURAL_VERSION || stride != TE_ANIMATION_PROCEDURAL_RECORD_BYTES
		|| recordCount > TE_ANIMATION_PROCEDURAL_MAX_RECORDS || (tableOffset & 15u) != 0u) return false;
	return recordCount == 0u || (tableOffset <= programBytes
		&& recordCount <= (programBytes - tableOffset) / TE_ANIMATION_PROCEDURAL_RECORD_BYTES);
}

bool TeValidateProceduralTable(
	TeAnimationGpuWorkPacket packet,
	uint tableOffset,
	uint recordCount)
{
	uint programBytes;
	gAnimationProgram.GetDimensions(programBytes);
	const TeAnimationGpuGenerationRecord generation = gAnimationGenerations[packet.generationRecordIndex];
	[loop]
	for (uint record = 0u; record < TE_ANIMATION_PROCEDURAL_MAX_RECORDS; ++record)
	{
		if (record >= recordCount) break;
		const uint recordOffset = tableOffset + record * TE_ANIMATION_PROCEDURAL_RECORD_BYTES;
		if (!TeValidateProceduralRecord(recordOffset, programBytes, packet, generation)) return false;
		[loop]
		for (uint previous = 0u; previous < TE_ANIMATION_PROCEDURAL_MAX_RECORDS; ++previous)
		{
			if (previous >= record) break;
			const uint previousOffset = tableOffset + previous * TE_ANIMATION_PROCEDURAL_RECORD_BYTES;
			// Concurrent writes to one joint would make the result dispatch-order
			// dependent. The lowering must split such operations into later stages.
			if (TeProceduralRecordsOverlap(previousOffset, recordOffset)) return false;
		}
	}
	return true;
}

bool TeSolveVisualTwoBone(
	uint recordOffset,
	TeAnimationGpuWorkPacket packet,
	out TeAnimationAffine rootMatrix,
	out TeAnimationAffine middleMatrix,
	out TeAnimationAffine endMatrix)
{
	const uint rootJoint = gAnimationProgram.Load(recordOffset + 4u);
	const uint middleJoint = gAnimationProgram.Load(recordOffset + 8u);
	const uint endJoint = gAnimationProgram.Load(recordOffset + 12u);
	if (!TeLoadModel(packet.modelPoseOffsetBytes.x + rootJoint * 48u, rootMatrix)
		|| !TeLoadModel(packet.modelPoseOffsetBytes.x + middleJoint * 48u, middleMatrix)
		|| !TeLoadModel(packet.modelPoseOffsetBytes.x + endJoint * 48u, endMatrix)) return false;
	const float3 originalRoot = TeProceduralTranslation(rootMatrix);
	const float3 originalMiddle = TeProceduralTranslation(middleMatrix);
	const float3 originalEnd = TeProceduralTranslation(endMatrix);
	const float3 target = asfloat(gAnimationProgram.Load4(recordOffset + 32u)).xyz;
	const float3 pole = asfloat(gAnimationProgram.Load4(recordOffset + 48u)).xyz;
	const float4 limits = asfloat(gAnimationProgram.Load4(recordOffset + 64u));
	const float4 controls = asfloat(gAnimationProgram.Load4(recordOffset + 80u));
	const float upperLength = length(originalMiddle - originalRoot);
	const float lowerLength = length(originalEnd - originalMiddle);
	if (!TeFinite(upperLength) || !TeFinite(lowerLength)
		|| upperLength <= 1.0e-6f || lowerLength <= 1.0e-6f) return false;
	const float requestedDistance = length(target - originalRoot);
	const float3 targetDirection = TeProceduralNormalizeOr(target - originalRoot, originalEnd - originalRoot);
	const float baseReach = upperLength + lowerLength;
	const float requestedStretch = requestedDistance > baseReach ? requestedDistance / baseReach : 1.0f;
	const float stretch = controls.y != 0.0f ? clamp(requestedStretch, 1.0f, controls.x) : 1.0f;
	const float stretchedUpper = upperLength * stretch;
	const float stretchedLower = lowerLength * stretch;
	const float minimumReach = abs(stretchedUpper - stretchedLower);
	const float maximumReach = stretchedUpper + stretchedLower;
	float solvedDistance = clamp(requestedDistance, minimumReach, maximumReach);
	if (solvedDistance <= 1.0e-6f) solvedDistance = min(maximumReach, 1.0e-6f);
	const float bendNumerator = stretchedUpper * stretchedUpper + stretchedLower * stretchedLower
		- solvedDistance * solvedDistance;
	const float bendDenominator = 2.0f * stretchedUpper * stretchedLower;
	float bend = 3.141592654f - acos(clamp(bendNumerator / bendDenominator, -1.0f, 1.0f));
	bend = clamp(bend, limits.x, limits.y);
	const float limitedDistanceSquared = stretchedUpper * stretchedUpper + stretchedLower * stretchedLower
		+ 2.0f * stretchedUpper * stretchedLower * cos(bend);
	solvedDistance = sqrt(max(0.0f, limitedDistanceSquared));
	float3 bendDirection;
	const float3 poleOffset = pole - originalRoot;
	const float3 projectedPole = poleOffset - targetDirection * dot(poleOffset, targetDirection);
	bendDirection = TeProceduralNormalizeOr(projectedPole,
		TeProceduralPerpendicular(targetDirection));
	const float rootCosine = solvedDistance <= 1.0e-6f ? 1.0f
		: clamp((stretchedUpper * stretchedUpper + solvedDistance * solvedDistance
			- stretchedLower * stretchedLower) / (2.0f * stretchedUpper * solvedDistance), -1.0f, 1.0f);
	const float rootSine = sqrt(max(0.0f, 1.0f - rootCosine * rootCosine));
	const float3 solvedMiddle = originalRoot + targetDirection * (stretchedUpper * rootCosine)
		+ bendDirection * (stretchedUpper * rootSine);
	const float3 solvedEnd = originalRoot + targetDirection * solvedDistance;
	TeProceduralSetTranslation(middleMatrix, lerp(originalMiddle, solvedMiddle, controls.w));
	TeProceduralSetTranslation(endMatrix, lerp(originalEnd, solvedEnd, controls.w));
	return TeFinite4(rootMatrix.row0) && TeFinite4(rootMatrix.row1) && TeFinite4(rootMatrix.row2)
		&& TeFinite4(middleMatrix.row0) && TeFinite4(middleMatrix.row1) && TeFinite4(middleMatrix.row2)
		&& TeFinite4(endMatrix.row0) && TeFinite4(endMatrix.row1) && TeFinite4(endMatrix.row2);
}

bool TeSolveVisualAim(uint recordOffset, inout TeAnimationAffine matrix)
{
	const float4 targetAndUnused = asfloat(gAnimationProgram.Load4(recordOffset + 32u));
	const float4 upAndMaximumAngle = asfloat(gAnimationProgram.Load4(recordOffset + 48u));
	const float weight = asfloat(gAnimationProgram.Load4(recordOffset + 80u)).w;
	const float3 translation = TeProceduralTranslation(matrix);
	const float3 originalRight = float3(matrix.row0.x, matrix.row1.x, matrix.row2.x);
	const float3 originalUp = float3(matrix.row0.y, matrix.row1.y, matrix.row2.y);
	const float3 originalForward = float3(matrix.row0.z, matrix.row1.z, matrix.row2.z);
	const float rightScale = length(originalRight);
	const float upScale = length(originalUp);
	const float forwardScale = length(originalForward);
	if (min(rightScale, min(upScale, forwardScale)) <= 1.0e-6f) return false;
	const float3 currentForward = originalForward / forwardScale;
	float3 desiredForward = TeProceduralNormalizeOr(targetAndUnused.xyz - translation, currentForward);
	const float angle = acos(clamp(dot(currentForward, desiredForward), -1.0f, 1.0f));
	if (angle > upAndMaximumAngle.w && angle > 1.0e-6f)
	{
		desiredForward = TeProceduralNormalizeOr(
			lerp(currentForward, desiredForward, upAndMaximumAngle.w / angle), currentForward);
	}
	desiredForward = TeProceduralNormalizeOr(lerp(currentForward, desiredForward, weight), currentForward);
	float3 desiredRight = TeProceduralNormalizeOr(cross(upAndMaximumAngle.xyz, desiredForward),
		TeProceduralPerpendicular(desiredForward));
	float3 desiredUp = TeProceduralNormalizeOr(cross(desiredForward, desiredRight), originalUp);
	desiredRight = TeProceduralNormalizeOr(cross(desiredUp, desiredForward), desiredRight);
	matrix.row0.xyz = float3(desiredRight.x * rightScale, desiredUp.x * upScale, desiredForward.x * forwardScale);
	matrix.row1.xyz = float3(desiredRight.y * rightScale, desiredUp.y * upScale, desiredForward.y * forwardScale);
	matrix.row2.xyz = float3(desiredRight.z * rightScale, desiredUp.z * upScale, desiredForward.z * forwardScale);
	return TeFinite4(matrix.row0) && TeFinite4(matrix.row1) && TeFinite4(matrix.row2);
}

bool TeSolveVisualRetarget(uint recordOffset, inout TeAnimationAffine matrix)
{
	const float4 translationAndScale = asfloat(gAnimationProgram.Load4(recordOffset + 32u));
	const float3 authoredX = asfloat(gAnimationProgram.Load4(recordOffset + 48u)).xyz;
	const float3 authoredY = asfloat(gAnimationProgram.Load4(recordOffset + 64u)).xyz;
	const float4 authoredZAndWeight = asfloat(gAnimationProgram.Load4(recordOffset + 80u));
	const float weight = authoredZAndWeight.w;
	const float3 currentX = float3(matrix.row0.x, matrix.row1.x, matrix.row2.x);
	const float3 currentY = float3(matrix.row0.y, matrix.row1.y, matrix.row2.y);
	const float3 currentZ = float3(matrix.row0.z, matrix.row1.z, matrix.row2.z);
	const float3 axisScale = float3(length(currentX), length(currentY), length(currentZ));
	if (min(axisScale.x, min(axisScale.y, axisScale.z)) <= 1.0e-6f) return false;

	const float3 targetX = TeProceduralNormalizeOr(authoredX, currentX);
	const float3 targetYSeed = authoredY - targetX * dot(authoredY, targetX);
	const float3 targetY = TeProceduralNormalizeOr(targetYSeed, currentY);
	const float3 targetZ = TeProceduralNormalizeOr(cross(targetX, targetY), authoredZAndWeight.xyz);
	if (dot(targetZ, authoredZAndWeight.xyz) < 0.0f) return false;
	const float3 blendedX = TeProceduralNormalizeOr(lerp(currentX / axisScale.x, targetX, weight), targetX);
	const float3 blendedY = TeProceduralNormalizeOr(lerp(currentY / axisScale.y, targetY, weight), targetY);
	const float3 blendedZ = TeProceduralNormalizeOr(lerp(currentZ / axisScale.z, targetZ, weight), targetZ);
	matrix.row0.xyz = float3(blendedX.x * axisScale.x, blendedY.x * axisScale.y, blendedZ.x * axisScale.z);
	matrix.row1.xyz = float3(blendedX.y * axisScale.x, blendedY.y * axisScale.y, blendedZ.y * axisScale.z);
	matrix.row2.xyz = float3(blendedX.z * axisScale.x, blendedY.z * axisScale.y, blendedZ.z * axisScale.z);
	const float3 targetTranslation = translationAndScale.xyz * translationAndScale.w;
	TeProceduralSetTranslation(matrix,
		lerp(TeProceduralTranslation(matrix), targetTranslation, weight));
	return TeFinite4(matrix.row0) && TeFinite4(matrix.row1) && TeFinite4(matrix.row2);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch;
	TeAnimationGpuWorkPacket packet;
	uint joint;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_PROCEDURAL, dispatch, packet, joint)) return;
	if (!TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_PROCEDURAL)
		|| joint >= packet.jointCount) return;
	if ((packet.flags & TE_ANIMATION_GPU_WORK_SUSPEND_VISUAL_EVALUATION) != 0u) return;
	// A deterministic or CPU-authoritative packet already contains the protected
	// pose selected by the CPU evaluator. Do not layer a visual-only result over it.
	if ((packet.flags & TE_ANIMATION_PROCEDURAL_RELEVANT_FLAGS) != 0u
		|| (packet.flags & TE_ANIMATION_GPU_WORK_ALLOW_VISUAL_PROCEDURAL) == 0u) return;
	uint tableOffset;
	uint recordCount;
	if (!TeResolveProceduralTable(packet, tableOffset, recordCount)
		|| !TeValidateProceduralTable(packet, tableOffset, recordCount))
	{
		if (joint == 0u) TeReport(dispatch, packet,
			TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD, tableOffset, recordCount);
		return;
	}
	uint ownedRecord = 0xffffffffu;
	[loop]
	for (uint record = 0u; record < TE_ANIMATION_PROCEDURAL_MAX_RECORDS; ++record)
	{
		if (record >= recordCount) break;
		const uint recordOffset = tableOffset + record * TE_ANIMATION_PROCEDURAL_RECORD_BYTES;
		if (gAnimationProgram.Load(recordOffset + 4u) == joint)
		{
			ownedRecord = recordOffset;
			break;
		}
	}
	if (ownedRecord == 0xffffffffu) return;
	const uint kind = gAnimationProgram.Load(ownedRecord);
	if (kind == TE_ANIMATION_PROCEDURAL_TWO_BONE)
	{
		TeAnimationAffine rootMatrix;
		TeAnimationAffine middleMatrix;
		TeAnimationAffine endMatrix;
		if (!TeSolveVisualTwoBone(ownedRecord, packet, rootMatrix, middleMatrix, endMatrix))
		{
			TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_NONFINITE_OUTPUT, joint, kind);
			return;
		}
		const uint middleJoint = gAnimationProgram.Load(ownedRecord + 8u);
		const uint endJoint = gAnimationProgram.Load(ownedRecord + 12u);
		if (!TeStoreModel(packet.modelPoseOffsetBytes.x + joint * 48u, rootMatrix)
			|| !TeStoreModel(packet.modelPoseOffsetBytes.x + middleJoint * 48u, middleMatrix)
			|| !TeStoreModel(packet.modelPoseOffsetBytes.x + endJoint * 48u, endMatrix))
			TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, kind);
		return;
	}

	TeAnimationAffine matrix;
	if (!TeLoadModel(packet.modelPoseOffsetBytes.x + joint * 48u, matrix))
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, kind);
		return;
	}
	bool valid = true;
	if (kind == TE_ANIMATION_PROCEDURAL_AIM)
	{
		valid = TeSolveVisualAim(ownedRecord, matrix);
	}
	else if (kind == TE_ANIMATION_PROCEDURAL_RETARGET_MODEL)
	{
		valid = TeSolveVisualRetarget(ownedRecord, matrix);
	}
	else if (kind == TE_ANIMATION_PROCEDURAL_COPY_MODEL)
	{
		const float weight = asfloat(gAnimationProgram.Load4(ownedRecord + 80u)).w;
		matrix.row0 = lerp(matrix.row0, asfloat(gAnimationProgram.Load4(ownedRecord + 32u)), weight);
		matrix.row1 = lerp(matrix.row1, asfloat(gAnimationProgram.Load4(ownedRecord + 48u)), weight);
		matrix.row2 = lerp(matrix.row2, asfloat(gAnimationProgram.Load4(ownedRecord + 64u)), weight);
	}
	else
	{
		const float4 offset = asfloat(gAnimationProgram.Load4(ownedRecord + 32u));
		const float weight = asfloat(gAnimationProgram.Load4(ownedRecord + 80u)).w;
		TeProceduralSetTranslation(matrix, TeProceduralTranslation(matrix) + offset.xyz * weight);
	}
	valid = valid && TeFinite4(matrix.row0) && TeFinite4(matrix.row1) && TeFinite4(matrix.row2);
	if (!valid)
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_NONFINITE_OUTPUT, joint, kind);
		return;
	}
	if (!TeStoreModel(packet.modelPoseOffsetBytes.x + joint * 48u, matrix))
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, kind);
}
