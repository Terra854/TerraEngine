// AudioComponentCapture.h - conversion from locked authored components to values.

#pragma once

#include "Audio/AudioAuthoringSnapshot.h"
#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"

namespace TerraEngine::Audio
{
	// These helpers intentionally copy values only. Audio workers receive the
	// resulting records, never a component pointer, ECS registry, or lock.
	[[nodiscard]] AudioAuthoringSourceRecord CaptureAudioSourceAuthoring(
		const AudioSourceComponent& component, AudioSourceIdentity identity,
		std::uint64_t clipGeneration, std::uint64_t clipFrames, AudioVector3 position);
	[[nodiscard]] AudioAuthoringListenerRecord CaptureAudioListenerAuthoring(
		const AudioListenerComponent& component, TerraEngine::UUID objectId,
		std::uint64_t componentGeneration, AudioVector3 position, AudioVector3 forward);
}
