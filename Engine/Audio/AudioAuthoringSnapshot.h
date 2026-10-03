// AudioAuthoringSnapshot.h - immutable records captured at the protected boundary.

#pragma once

#include "Audio/AudioTypes.h"
#include "Scene/WorldCoordinate.h"

#include <memory>
#include <optional>
#include <vector>

namespace TerraEngine::Audio {
	// All records share one frame. Production keeps exact cells; existing local
	// callers explicitly describe positions within one common local frame.
	enum class AudioCoordinateMode { SharedLocalFrame, CanonicalWorld };
	struct AudioAuthoringSourceRecord {
		AudioSourceIdentity identity;
		TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID;
		AudioGeneration clipGeneration = 0U;
		AudioFrame clipFrames = 0U;
		std::uint64_t clipChecksum = 0U;
		AudioClipEncoding clipEncoding = AudioClipEncoding::Wav;
		std::uint32_t clipSampleRate = 0U;
		std::uint32_t clipChannels = 0U;
		std::shared_ptr<const std::vector<std::uint8_t>> clipPayload;
		TerraEngine::UUID mixGroupId = TerraEngine::EMPTY_UUID;
		AudioVector3 position;
		float volume = 1.0F, mixVolume = 1.0F, pitch = 1.0F, maximumDistance = 25.0F, spatialBlend = 1.0F;
		std::uint32_t priority = 128U;
		bool enabled = true, loop = false, spatial = true, mixMuted = false;
		std::optional<TerraEngine::Scene::WorldPosition> worldPosition;
	};
	struct AudioAuthoringListenerRecord { TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID; AudioGeneration componentGeneration = 0U; AudioVector3 position; AudioVector3 forward{0.0F, 0.0F, -1.0F}; AudioOutputMode outputMode = AudioOutputMode::Stereo; bool enabled = true, eligible = true; std::optional<TerraEngine::Scene::WorldPosition> worldPosition; };
	struct AudioAuthoringSnapshotData { AudioFrame frame = 0U; float deltaSeconds = 0.0F; std::vector<AudioAuthoringSourceRecord> sources; std::vector<AudioAuthoringListenerRecord> listeners; float masterVolume = 1.0F; bool masterMuted = false, gamePaused = false; std::uint32_t voiceLimit = 64U; AudioCoordinateMode coordinateMode = AudioCoordinateMode::SharedLocalFrame; };
	class TERRAENGINE_API AudioAuthoringSnapshot final { public: static AudioAuthoringSnapshot Create(AudioAuthoringSnapshotData data); const AudioAuthoringSnapshotData& Data() const noexcept { return *data; } private: explicit AudioAuthoringSnapshot(std::shared_ptr<const AudioAuthoringSnapshotData> value) : data(std::move(value)) {} std::shared_ptr<const AudioAuthoringSnapshotData> data; };
}
