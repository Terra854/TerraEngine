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
		// Worker bridges read these immutable captured records; they never inspect
		// ObjectManager or refresh an old attachment into replacement storage.
		std::optional<AudioSourceBinding> ReadSourceBinding(std::uint32_t entity) const;
		std::optional<AudioSourceBinding> ReadSourceBinding(const TerraEngine::UUID& objectId) const;
		std::optional<AudioSourcePlaybackObservation> ReadSourcePlayback(const AudioSourceAttachmentKey& attachment) const;
		// Logical receipts settle at the protected AudioSnapshot boundary. Playback
		// and provider diagnostics publish at AudioCommit; a post-commit observer
		// failure still retains the truthful terminal logical receipt.
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
		// Editor Play Mode owns runtime playback admission. Exported runtimes keep
		// the default enabled state, while the editor disables it until Play starts.
		void SetRuntimePlaybackEnabled(bool enabled);
		void SetRuntimePlaybackPaused(bool paused);
	private:
		struct ActiveVoiceRecord {
			AudioSourceIdentity identity;
			TerraEngine::UUID clipAssetId = TerraEngine::EMPTY_UUID;
			AudioGeneration clipGeneration = 0U;
		};
		// CapturePhase is the protected bridge from live scene components into the
		// immutable canonical-world audio snapshot consumed by worker preparation.
		// Workers localize against their selected listener without reading live poses.
		void CapturePhase(float deltaTime);
		AudioGeneration AllocatePlaybackGeneration() noexcept;
		bool ResolveAdmission(AudioControlBatch& batch, AudioControlReceipt& receipt);
		struct PlayReservation {
			AudioSourceAttachmentKey attachment;
			AudioFrameIndex requestedFrame = 0U;
			AudioBatchSequence sequence = 0U;
			std::uint32_t commandIndex = 0U;
			AudioGeneration playbackGeneration = 0U;
		};
		void SnapshotPhase(); void PreparePhase(); void CommitPhase(); void PublishFailure(std::string category, std::string message);
		mutable std::mutex mutex;
		AudioControlQueue controls;
		AudioMixConfiguration mix;
		std::unique_ptr<Backend::IAudioBackend> backend;
		std::string providerId = "terra.miniaudio";
		AudioProviderDescriptor descriptor;
		AudioAuthoringSnapshot authoring = AudioAuthoringSnapshot::Create({});
		std::vector<AudioPlaybackSeed> playback;
		std::vector<PlayReservation> playReservations;
		// This allocator deliberately survives Shutdown/Initialize and scene swaps.
		AudioGeneration nextPlaybackGeneration = 1U;
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
		bool runtimePlaybackEnabled = true;
		bool runtimePlaybackPaused = false;
	};
}
