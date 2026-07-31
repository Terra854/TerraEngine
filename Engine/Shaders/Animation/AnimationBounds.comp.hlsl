// AnimationBounds.comp.hlsl - One bounded conservative deformation-bounds reduction per packet.
#include "AnimationCodec.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint element;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_BOUNDS, dispatch, packet, element)) return;
	if (!TeValidatePacket(dispatch, packet, element, TE_ANIMATION_GPU_STAGE_BOUNDS) || element != 0u) return;
	uint inputBytes; gAnimationMorphInputs.GetDimensions(inputBytes); uint outputBytes; gAnimationBounds.GetDimensions(outputBytes);
	const uint outputOffset = packet.boundsOutputIndex * 32u;
	const uint tableOffset = packet.morphResourceIndex * 16u;
	bool valid = TeRangeValid(outputBytes, outputOffset, 32u) && packet.morphResourceIndex != TE_ANIMATION_INVALID_RESOURCE;
	float3 minimum = float3(-1.0e20f, -1.0e20f, -1.0e20f);
	float3 maximum = float3(1.0e20f, 1.0e20f, 1.0e20f);
	if (valid && TeRangeValid(
		inputBytes,
		tableOffset,
		TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
			+ TE_ANIMATION_MORPH_BOUNDS_BYTES))
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
			&& tableCount <= (0xffffffffu
				- tableOffset
				- TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
				- TE_ANIMATION_MORPH_BOUNDS_BYTES)
					/ TE_ANIMATION_MORPH_RECORD_BYTES
			&& TeRangeValid(
				inputBytes,
				tableOffset,
				TE_ANIMATION_MORPH_TABLE_HEADER_BYTES
					+ TE_ANIMATION_MORPH_BOUNDS_BYTES
					+ tableCount * TE_ANIMATION_MORPH_RECORD_BYTES);
		// Canonical TEMF v1 stores one whole-animation envelope. It includes every
		// skin and morph excursion, so publishing it directly is conservative
		// for all weights and avoids inventing unavailable per-target bounds.
		minimum = asfloat(gAnimationMorphInputs.Load3(
			tableOffset + TE_ANIMATION_MORPH_TABLE_HEADER_BYTES));
		maximum = asfloat(gAnimationMorphInputs.Load3(
			tableOffset + TE_ANIMATION_MORPH_TABLE_HEADER_BYTES + 16u));
		valid = valid && TeFinite3(minimum)
			&& TeFinite3(maximum) && all(minimum <= maximum);
	}
	if (!valid || !TeFinite3(minimum) || !TeFinite3(maximum) || any(minimum > maximum))
	{
		minimum = float3(-1.0e20f, -1.0e20f, -1.0e20f); maximum = float3(1.0e20f, 1.0e20f, 1.0e20f);
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, packet.morphCount, outputOffset);
	}
	gAnimationBounds.Store4(outputOffset, asuint(float4(minimum, valid ? 1.0f : 0.0f)));
	gAnimationBounds.Store4(outputOffset + 16u, asuint(float4(maximum, valid ? 0.0f : 1.0f)));
}
