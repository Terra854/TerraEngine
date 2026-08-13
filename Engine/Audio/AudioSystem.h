// AudioSystem.h - protected audio snapshot/prepare/commit coordinator.

#pragma once

#include "Audio/AudioAuthoringSnapshot.h"
#include "Audio/AudioCommands.h"
#include "Audio/AudioDiagnostics.h"
#include "Audio/AudioMixConfiguration.h"
#include "Audio/AudioPreparationPlan.h"
#include "Audio/Backend/AudioBackendRegistry.h"
#include "Core/SystemBase.h"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace TerraEngine::Audio {
	class TERRAENGINE_API AudioSystem final : public SystemsBase {
	public:
		static AudioSystem& Instance();
		void Initialize() override; void Update(float deltaTime) override; void Shutdown() override; std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;
		AudioControlReceipt SubmitControlBatch(AudioControlBatch batch);
		// Receipts appear only after AudioCommit, matching the same protected
		// visibility boundary as playback and diagnostic snapshots.
		std::optional<AudioControlReceipt> ReadControlReceipt(AudioBatchSequence batchSequence) const;
		std::shared_ptr<const AudioPlaybackSnapshot> LatestPlaybackSnapshot() const;
		AudioMixSnapshot LatestMixSnapshot() const;
		bool SetMasterMix(float volume, bool muted, std::string& diagnostic);
		bool ReplaceMixSnapshot(AudioMixSnapshot snapshot, std::string& diagnostic);
		bool SetMixGroupVolume(const TerraEngine::UUID& id, float volume, std::string& diagnostic);
		bool SetMixGroupMuted(const TerraEngine::UUID& id, bool muted, std::string& diagnostic);
		bool RenameMixGroup(const TerraEngine::UUID& id, std::string name, std::string& diagnostic);
		void SetAuthoringSnapshot(AudioAuthoringSnapshot snapshot);
		void SetConfiguredProvider(std::string providerId);
	private:
		struct ActiveVoiceRecord {
			AudioSourceIdentity identity;
			TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID;
			AudioGeneration clipGeneration = 0U;
		};
		// CapturePhase is the protected bridge from live scene components into the
		// immutable audio authoring snapshot consumed by worker preparation.
		void CapturePhase(float deltaTime);
		void SnapshotPhase(); void PreparePhase(); void CommitPhase(); void PublishFailure(std::string category, std::string message);
		mutable std::mutex mutex;
		AudioControlQueue controls;
		AudioMixConfiguration mix;
		std::unique_ptr<Backend::IAudioBackend> backend;
		std::string providerId = "terra.miniaudio";
		AudioProviderDescriptor descriptor;
		AudioAuthoringSnapshot authoring = AudioAuthoringSnapshot::Create({});
		std::vector<AudioPlaybackSeed> playback;
		AudioPreparedFrame prepared;
		std::shared_ptr<const AudioPlaybackSnapshot> published;
		std::vector<AudioFailureEpisode> failures;
		std::vector<AudioControlReceipt> controlReceipts;
		std::unordered_map<AudioVoiceToken, ActiveVoiceRecord> activeVoices;
		std::unordered_map<std::string, AudioClipRelease> registeredClips;
		AudioFrame frame = 0U;
		AudioPacketSequence packetSequence = 0U;
		bool initialized = false;
		// Device availability is an observed provider fact. Keeping it separately
		// from backend ownership lets a recovered provider retain logical state.
		bool deviceAvailable = false;
		bool manualAuthoringSnapshot = false;
	};
}
