// AnimationBlend.comp.hlsl - Bounded blend/layer/mask accumulation for local poses.
#include "AnimationCodec.hlsli"

TeAnimationPose TeBlendPose(TeAnimationPose basePose, TeAnimationPose source, TeAnimationPose reference, float weight, bool additive)
{
	weight = saturate(weight);
	if (!additive) { basePose.translation.xyz = lerp(basePose.translation.xyz, source.translation.xyz, weight); basePose.scale.xyz = lerp(basePose.scale.xyz, source.scale.xyz, weight); basePose.rotation = TeNormalizeQuaternion(lerp(basePose.rotation, dot(basePose.rotation, source.rotation) < 0.0f ? -source.rotation : source.rotation, weight)); return basePose; }
	basePose.translation.xyz += (source.translation.xyz - reference.translation.xyz) * weight;
	basePose.scale.xyz *= lerp(float3(1.0f, 1.0f, 1.0f), source.scale.xyz / max(abs(reference.scale.xyz), float3(1.0e-6f, 1.0e-6f, 1.0e-6f)), weight);
	const float4 delta = TeNormalizeQuaternion(source.rotation * float4(-reference.rotation.xyz, reference.rotation.w));
	basePose.rotation = TeNormalizeQuaternion(basePose.rotation * TeNormalizeQuaternion(lerp(float4(0.0f, 0.0f, 0.0f, 1.0f), delta, weight)));
	return basePose;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint joint;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_BLEND, dispatch, packet, joint)) return;
	if (!TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_BLEND) || joint >= packet.jointCount) return;
	TeAnimationPose pose; if (!TeLoadLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, pose)) { pose = TeIdentityPose(); TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, joint, 0u); }
	uint programBytes; gAnimationProgram.GetDimensions(programBytes); const uint header = packet.programResourceIndex * 64u + 16u;
	if (!TeRangeValid(programBytes, header, 4u)) { TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_MISSING_RESOURCE, joint, 0u); TeStoreLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, pose); return; }
	const uint layerCount = min(gAnimationProgram.Load(header), 8u);
	[unroll] for (uint layer = 0u; layer < 8u; ++layer)
	{
		if (layer >= layerCount) break;
		const uint layerOffset = header + 4u + layer * 16u;
		if (!TeRangeValid(programBytes, layerOffset, 16u)) { TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_RANGE_OVERFLOW, joint, layer); break; }
		TeAnimationPose source; TeAnimationPose reference;
		const uint sourceOffset = gAnimationProgram.Load(layerOffset) + joint * 48u;
		const uint referenceOffset = gAnimationProgram.Load(layerOffset + 4u) + joint * 48u;
		const float weight = asfloat(gAnimationProgram.Load(layerOffset + 8u)); const uint flags = gAnimationProgram.Load(layerOffset + 12u);
		if (!TeLoadLocalPose(sourceOffset, source) || !TeLoadLocalPose(referenceOffset, reference) || !TeFinite(weight)) { TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_NONFINITE_OUTPUT, joint, layer); continue; }
		float mask = 1.0f; uint maskBytes; gAnimationMasks.GetDimensions(maskBytes); const uint maskOffset = packet.maskResourceIndex * 4u + joint * 4u;
		if (packet.maskResourceIndex != TE_ANIMATION_INVALID_RESOURCE && TeRangeValid(maskBytes, maskOffset, 4u)) mask = saturate(asfloat(gAnimationMasks.Load(maskOffset)));
		pose = TeBlendPose(pose, source, reference, weight * mask, (flags & 1u) != 0u);
	}
	if (!TeFinite4(pose.translation) || !TeFinite4(pose.rotation) || !TeFinite4(pose.scale)) { pose = TeIdentityPose(); TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_NONFINITE_OUTPUT | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, joint, 0u); }
	if (!TeStoreLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, pose)) TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, 0u);
}
