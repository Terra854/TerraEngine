// AnimationPalette.comp.hlsl - Checked palette compaction and generation provenance.
#include "AnimationCodec.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint entry;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_PALETTE, dispatch, packet, entry)) return;
	if (!TeValidatePacket(dispatch, packet, entry, TE_ANIMATION_GPU_STAGE_PALETTE) || entry >= packet.paletteEntryCount) return;
	if ((packet.flags & TE_ANIMATION_GPU_WORK_SUSPEND_VISUAL_EVALUATION) != 0u) return;
	uint paletteBytes; gAnimationPalette.GetDimensions(paletteBytes);
	uint skeletonBytes; gAnimationSkeleton.GetDimensions(skeletonBytes);
	uint outputOffset = 0u;
	uint directory = 0u;
	bool outputAddressValid = entry <= (0xffffffffu - packet.paletteOffsetBytes.x)
		/ TE_ANIMATION_GPU_PALETTE_STRIDE_BYTES;
	if (outputAddressValid)
	{
		outputOffset = packet.paletteOffsetBytes.x
			+ entry * TE_ANIMATION_GPU_PALETTE_STRIDE_BYTES;
		outputAddressValid = TeRangeValid(paletteBytes, outputOffset, 48u);
	}
	bool valid = outputAddressValid
		&& packet.palettePartitionCount == 1u
		&& packet.skeletonResourceIndex <= 0xffffffffu / 16u;
	if (valid)
	{
		directory = packet.skeletonResourceIndex * 16u;
		valid = TeRangeValid(skeletonBytes, directory, 16u);
	}
	TeAnimationAffine matrix = TeAffineFromPose(TeIdentityPose());
	uint sourceJoint = 0u;
	uint paletteRecord = 0u;
	if (valid)
	{
		const uint paletteTable = gAnimationSkeleton.Load(directory + 12u);
		valid = entry <= (0xffffffffu - paletteTable) / 64u;
		if (valid)
		{
			paletteRecord = paletteTable + entry * 64u;
			valid = TeRangeValid(skeletonBytes, paletteRecord, 64u);
		}
	}
	TeAnimationAffine inverseBind = TeAffineFromPose(TeIdentityPose());
	if (valid)
	{
		sourceJoint = gAnimationSkeleton.Load(paletteRecord);
		// Reserved lanes are frozen zero. Rejecting non-zero values makes a future
		// palette format require an explicit version change instead of being
		// accidentally interpreted by this shader.
		valid = sourceJoint < packet.jointCount
			&& gAnimationSkeleton.Load(paletteRecord + 4u) == 0u
			&& gAnimationSkeleton.Load(paletteRecord + 8u) == 0u
			&& gAnimationSkeleton.Load(paletteRecord + 12u) == 0u;
		if (valid)
		{
			inverseBind.row0 = asfloat(gAnimationSkeleton.Load4(paletteRecord + 16u));
			inverseBind.row1 = asfloat(gAnimationSkeleton.Load4(paletteRecord + 32u));
			inverseBind.row2 = asfloat(gAnimationSkeleton.Load4(paletteRecord + 48u));
			valid = TeFinite4(inverseBind.row0)
				&& TeFinite4(inverseBind.row1)
				&& TeFinite4(inverseBind.row2);
		}
	}
	if (valid)
	{
		uint modelOffset = 0u;
		valid = sourceJoint <= (0xffffffffu - packet.modelPoseOffsetBytes.x) / 48u;
		if (valid)
		{
			modelOffset = packet.modelPoseOffsetBytes.x + sourceJoint * 48u;
			valid = TeLoadModel(modelOffset, matrix);
		}
		if (valid) matrix = TeMultiplyAffine(matrix, inverseBind);
	}
	if (!valid)
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_PALETTE_OVERFLOW | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, entry, sourceJoint);
		matrix = TeAffineFromPose(TeIdentityPose());
	}
	if (outputAddressValid)
	{
		gAnimationPalette.Store4(outputOffset, asuint(matrix.row0));
		gAnimationPalette.Store4(outputOffset + 16u, asuint(matrix.row1));
		gAnimationPalette.Store4(outputOffset + 32u, asuint(matrix.row2));
	}
	else TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, entry, outputOffset);
}
