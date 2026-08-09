// AnimationSample.comp.hlsl - Checked segmented clip decompression into local poses.
#include "AnimationCodec.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	TeAnimationGpuDispatchRecord dispatch; TeAnimationGpuWorkPacket packet; uint joint;
	if (!TeResolveWork(id.x, TE_ANIMATION_GPU_STAGE_SAMPLE, dispatch, packet, joint)) return;
	if (!TeValidatePacket(dispatch, packet, joint, TE_ANIMATION_GPU_STAGE_SAMPLE) || joint >= packet.jointCount) return;
	if (TeVisualEvaluationSuspended(packet)) return;
	TeAnimationPose pose = TeIdentityPose();
	// Program records are 16 bytes per joint: clip offsets for T/R/S plus a reserved lane.
	uint programBytes; gAnimationProgram.GetDimensions(programBytes);
	const uint recordOffset = packet.programResourceIndex * 64u + 64u + joint * 16u;
	bool valid = packet.clipResourceIndex != TE_ANIMATION_INVALID_RESOURCE && packet.programResourceIndex != TE_ANIMATION_INVALID_RESOURCE
		&& TeRangeValid(programBytes, recordOffset, 16u);
	// Looping and reverse playback are resolved by the immutable frame snapshot.
	// The packet lane is therefore the exact clip-local tick for this instance.
	const uint tick = packet.sampleTick;
	float3 vectorValue; float4 rotationValue; float scalarValue;
	if (valid && TeDecodeTrack(gAnimationProgram.Load(recordOffset), tick, vectorValue, rotationValue, scalarValue)) pose.translation.xyz = vectorValue; else valid = false;
	if (valid && TeDecodeTrack(gAnimationProgram.Load(recordOffset + 4u), tick, vectorValue, rotationValue, scalarValue)) pose.rotation = rotationValue; else valid = false;
	if (valid && TeDecodeTrack(gAnimationProgram.Load(recordOffset + 8u), tick, vectorValue, rotationValue, scalarValue)) pose.scale.xyz = vectorValue; else valid = false;
	if (!valid || !TeFinite4(pose.translation) || !TeFinite4(pose.scale))
	{
		TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_INVALID_RECORD | TE_ANIMATION_GPU_DIAGNOSTIC_BIND_POSE_FALLBACK, joint, recordOffset);
		// Bind is stateless; hold-last is honored only when the previous local pose is safe.
		if ((packet.flags & TE_ANIMATION_GPU_WORK_HOLD_LAST_FALLBACK) != 0u && TeLoadLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, pose))
			TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_HOLD_LAST_FALLBACK, joint, 0u);
	}
	if (!TeStoreLocalPose(packet.localPoseOffsetBytes.x + joint * 48u, pose)) TeReport(dispatch, packet, TE_ANIMATION_GPU_DIAGNOSTIC_OUTPUT_OVERFLOW, joint, 0u);
}
