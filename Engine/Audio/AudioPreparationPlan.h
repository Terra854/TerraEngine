// AudioPreparationPlan.h - worker-safe logical clocks, spatial values, and voice ranking.

#pragma once

#include "Audio/AudioAuthoringSnapshot.h"

#include <optional>

namespace TerraEngine::Audio {
	struct AudioPreparedVoice { AudioSourceIdentity identity; TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID; AudioGeneration clipGeneration = 0U; AudioFrame logicalFrame = 0U; std::uint32_t priority = 128U; float gain = 0.0F, pitch = 1.0F; AudioAudibility audibility = AudioAudibility::PendingAsset; AudioSpatialParameters spatial; bool loop = false, create = false, destroy = false, paused = false; };
	struct AudioPreparedFrame { AudioFrame frame = 0U; std::optional<AudioAuthoringListenerRecord> listener; std::vector<AudioPreparedVoice> voices; std::uint32_t listenerCandidateCount = 0U; std::uint32_t configuredVoiceLimit = 0U; };
	struct AudioPlaybackSeed {
		AudioSourceIdentity identity;
		AudioPlaybackState state = AudioPlaybackState::Stopped;
		AudioFrame logicalFrame = 0U;
		// The instance retains the publication it started from. A later authored
		// clip change stops this generation instead of silently playing new bytes.
		TerraEngine::UUID startedClipAssetId = TerraEngine::EMPTY_UUID;
		AudioGeneration startedClipGeneration = 0U;
	};
	TERRAENGINE_API AudioPreparedFrame PrepareAudioFrame(const AudioAuthoringSnapshot& snapshot, std::vector<AudioPlaybackSeed>& mutablePlayback);
	TERRAENGINE_API float ComputeAudioAttenuation(float distance, float minimumDistance, float maximumDistance) noexcept;
	TERRAENGINE_API float ComputeAudioPan(const AudioVector3& listenerForward, const AudioVector3& sourceOffset) noexcept;
}
