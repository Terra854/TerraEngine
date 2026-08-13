// AudioComponentCapture.cpp - protected-boundary value capture helpers.

#include "AudioComponentCapture.h"

namespace TerraEngine::Audio
{
	AudioAuthoringSourceRecord CaptureAudioSourceAuthoring(const AudioSourceComponent& component,
		AudioSourceIdentity identity, std::uint64_t clipGeneration, std::uint64_t clipFrames, AudioVector3 position)
	{
		auto lock = component.acquireReadLock();
		AudioAuthoringSourceRecord record;
		record.identity = identity;
		record.clipAssetId = component.clipAssetId;
		record.clipGeneration = clipGeneration;
		record.clipFrames = clipFrames;
		record.position = position;
		record.volume = component.volume;
		record.pitch = component.pitch;
		record.minimumDistance = component.minDistance;
		record.maximumDistance = component.maxDistance;
		record.spatialBlend = component.spatialBlend;
		record.priority = component.priority;
		record.enabled = component.active;
		record.loop = component.loop;
		record.spatial = component.spatialBlend > 0.0F;
		return record;
	}

	AudioAuthoringListenerRecord CaptureAudioListenerAuthoring(const AudioListenerComponent& component,
		TerraEngine::UUID objectId, std::uint64_t componentGeneration, AudioVector3 position, AudioVector3 forward)
	{
		auto lock = component.acquireReadLock();
		AudioAuthoringListenerRecord record;
		record.objectId = objectId;
		record.componentGeneration = componentGeneration;
		record.position = position;
		record.forward = forward;
		record.enabled = component.active;
		record.eligible = component.eligible;
		return record;
	}
}
