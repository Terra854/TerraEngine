// AnimationMorph.comp.hlsl - Checked morph accumulation with bind-safe output.
#include "AnimationCodec.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint morph;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_MORPH, dispatch, packet, morph)) return;
	if (!TeValidatePacket(dispatch, packet, morph, TE_ANIMATION_GPU_STAGE_MORPH) || morph >= packet.morphCount) return;
	uint inputBytes; gAnimationMorphInputs.GetDimensions(inputBytes); uint outputBytes; gAnimationMorphOutputs.GetDimensions(outputBytes);
	const uint outputOffset = packet.morphOutputOffsetBytes.x + morph * 4u;
	float weight = 0.0f;
	const uint tableOffset = packet.morphResourceIndex * 16u;
	bool valid = packet.morphResourceIndex != TE_ANIMATION_INVALID_RESOURCE
		&& TeRangeValid(inputBytes, tableOffset, TE_ANIMATION_MORPH_TABLE_HEADER_BYTES)
		&& TeRangeValid(outputBytes, outputOffset, 4u);
	if (valid)
	{
		const uint magic = gAnimationMorphInputs.Load(tableOffset);
		const uint versionAndStride = gAnimationMorphInputs.Load(tableOffset + 4u);
		const uint tableCount = gAnimationMorphInputs.Load(tableOffset + 8u);
		const uint reserved = gAnimationMorphInputs.Load(tableOffset + 12u);
		const uint version = versionAndStride & 0xffffu;
		const uint stride = versionAndStride >> 16u;
		valid = magic == TE_ANIMATION_MORPH_TABLE_MAGIC
			&& version == TE_ANIMATION_MORPH_TABLE_VERSION
			&& stride == TE_ANIMATION_MORPH_RECORD_BYTES
			&& tableCount == packet.morphCount
			&& tableCount <= TE_ANIMATION_MAX_MORPHS
			&& reserved == 0u
			&& tableOffset <= 0xffffffffu
				- TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
				- TE_ANIMATION_MORPH_BOUNDS_BYTES
			&& morph <= (0xffffffffu
				- tableOffset
				- TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
				- TE_ANIMATION_MORPH_BOUNDS_BYTES)
					/ TE_ANIMATION_MORPH_RECORD_BYTES;
		const uint inputOffset = tableOffset
			+ TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
			+ TE_ANIMATION_MORPH_BOUNDS_BYTES
			+ morph * TE_ANIMATION_MORPH_RECORD_BYTES;
		valid = valid
			&& TeRangeValid(inputBytes, inputOffset, TE_ANIMATION_MORPH_RECORD_BYTES);
		if (valid)
		{
			const uint4 record = gAnimationMorphInputs.Load4(inputOffset);
			float3 ignoredVector;
			float4 ignoredRotation;
			valid = all(record.yzw == 0u)
				&& TeDecodeTrack(
					record.x,
					packet.sampleTick,
					ignoredVector,
					ignoredRotation,
					weight)
				&& TeFinite(weight);
		}
	}
	if (!valid)
	{
		weight = 0.0f;
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, morph, tableOffset);
	}
	gAnimationMorphOutputs.Store(outputOffset, asuint(weight));
}
