// AudioPlaybackTests.cpp - logical playback and spatial invariance checks.

#include "../TestHarness.h"
#include "Audio/AudioPreparationPlan.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"

namespace TerraEngine::Tests {
	void RegisterAudioPlaybackTests(TestRegistry& registry) {
		registry.Add("Audio.Playback.OneShotLoopAndPauseUseLogicalTimeline", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data; data.frame = 1U; data.deltaSeconds = 1.0F / 60.0F;
			Audio::AudioAuthoringSourceRecord source; source.identity = {TerraEngine::GenerateUUID(), 1U, 1U};
			source.clipAssetId = TerraEngine::GenerateUUID(); source.clipGeneration = 1U; source.clipFrames = 1000U;
			source.clipSampleRate = 8000U;
			source.spatial = false; source.spatialBlend = 0.0F; data.sources.push_back(source);
			std::vector<Audio::AudioPlaybackSeed> playback{{source.identity, Audio::AudioPlaybackState::Playing, 0U}};
			const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(prepared.voices.size() == 1U && prepared.voices[0].logicalFrame == 133U,
				"A playing source should advance in clip-rate frames without consulting device time.");
			context.Expect(prepared.voices[0].spatial.pan == 0.0F,
				"Non-spatial playback must be invariant to listener availability.");

			data.deltaSeconds = 1.0F;
			data.sources[0].clipSampleRate = 100U;
			data.sources[0].clipFrames = 100U;
			playback[0].logicalFrame = 0U;
			(void)Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(playback[0].state == Audio::AudioPlaybackState::Completed,
				"A non-looping one-shot must complete when its logical cursor reaches the final frame.");

			data.sources[0].loop = true;
			playback[0].state = Audio::AudioPlaybackState::Playing;
			playback[0].logicalFrame = 75U;
			(void)Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(playback[0].state == Audio::AudioPlaybackState::Playing && playback[0].logicalFrame == 75U,
				"Looping playback must wrap with integer frame arithmetic and remain playing.");

			playback[0].state = Audio::AudioPlaybackState::Paused;
			const Audio::AudioFrame pausedFrame = playback[0].logicalFrame;
			(void)Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), playback);
			context.Expect(playback[0].logicalFrame == pausedFrame,
				"A paused source must retain its logical cursor until an explicit resume.");
		});

		registry.Add("Audio.Playback.ControlBatchesRejectAtomicallyAtSnapshot", [](TestContext& context) {
			auto& system = Audio::AudioSystem::Instance();
			system.SetConfiguredProvider("test.missing");
			system.Initialize();
			Audio::AudioAuthoringSnapshotData data;
			Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::UUID{42U}, 1U, 1U};
			source.clipAssetId = TerraEngine::UUID{43U};
			source.clipGeneration = 1U;
			source.clipFrames = 48000U;
			source.spatial = false;
			data.sources.push_back(source);
			system.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data));

			Audio::AudioControlBatch rejected;
			rejected.requestedFrame = 1U;
			rejected.correlationId = "atomic-reject";
			rejected.commands.push_back({Audio::AudioControlKind::Play, {source.identity.objectId, 1U, 1U, 1U}});
			Audio::AudioControlCommand invalidGroup;
			invalidGroup.kind = Audio::AudioControlKind::SetGroupVolume;
			invalidGroup.mixGroupId = TerraEngine::UUID{99U};
			invalidGroup.scalarValue = 0.5F;
			rejected.commands.push_back(invalidGroup);
			const auto rejectedReceipt = system.SubmitControlBatch(std::move(rejected));
			system.Update(1.0F / 60.0F);
			const auto rejectedDiagnostics = Audio::AcquireAudioDiagnosticsSnapshot();
			const auto publishedRejectedReceipt = system.ReadControlReceipt(rejectedReceipt.batchSequence);
			context.Expect(system.LatestPlaybackSnapshot()->voices.empty()
				&& rejectedDiagnostics && !rejectedDiagnostics->controlReceipts.empty()
				&& rejectedDiagnostics->controlReceipts.back().status == Audio::AudioControlStatus::Rejected
				&& publishedRejectedReceipt && publishedRejectedReceipt->status == Audio::AudioControlStatus::Rejected,
				"One invalid command must reject the complete batch without starting its valid play command.");

			Audio::AudioControlBatch accepted;
			accepted.requestedFrame = 2U;
			accepted.correlationId = "atomic-accept";
			accepted.commands.push_back({Audio::AudioControlKind::Play, {source.identity.objectId, 1U, 1U, 1U}});
			(void)system.SubmitControlBatch(std::move(accepted));
			system.Update(1.0F / 60.0F);
			context.Expect(system.LatestPlaybackSnapshot()->voices.size() == 1U,
				"A complete valid batch must become visible after the following AudioCommit boundary.");
			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
		});

		registry.Add("Audio.Playback.PendingAssetsAndMutePreserveLogicalSemantics", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData pendingData; pendingData.deltaSeconds = 1.0F;
			Audio::AudioAuthoringSourceRecord pending; pending.identity = {TerraEngine::UUID{77U}, 1U, 1U}; pending.spatial = false;
			pendingData.sources.push_back(pending);
			std::vector<Audio::AudioPlaybackSeed> pendingPlayback{{pending.identity, Audio::AudioPlaybackState::Playing, 91U}};
			const auto pendingPrepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(pendingData), pendingPlayback);
			context.Expect(pendingPrepared.voices[0].audibility == Audio::AudioAudibility::PendingAsset
				&& pendingPrepared.voices[0].logicalFrame == 91U && pendingPlayback[0].logicalFrame == 91U,
				"A missing clip generation must retain its pending logical instance without advancing device-independent time.");

			Audio::AudioAuthoringSnapshotData mutedData; mutedData.deltaSeconds = 1.0F;
			Audio::AudioAuthoringSourceRecord muted; muted.identity = {TerraEngine::UUID{78U}, 1U, 1U}; muted.clipAssetId = TerraEngine::UUID{79U}; muted.clipGeneration = 1U; muted.clipFrames = 96000U; muted.spatial = false; muted.mixMuted = true;
			mutedData.sources.push_back(muted);
			std::vector<Audio::AudioPlaybackSeed> mutedPlayback{{muted.identity, Audio::AudioPlaybackState::Playing, 0U}};
			const auto mutedPrepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(mutedData), mutedPlayback);
			context.Expect(mutedPrepared.voices[0].audibility == Audio::AudioAudibility::GroupMuted && mutedPrepared.voices[0].gain == 0.0F
				&& mutedPlayback[0].logicalFrame > 0U,
				"Mix mute must silence output without changing the engine-owned logical timeline.");
			mutedData.gamePaused = true; const Audio::AudioFrame cursor = mutedPlayback[0].logicalFrame;
			(void)Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(mutedData)), mutedPlayback);
			context.Expect(mutedPlayback[0].logicalFrame == cursor,
				"Game pause must freeze logical time independently from source or mix mute.");
		});

		registry.Add("Audio.Playback.ClipGenerationChangeStopsExistingInstance", [](TestContext& context) {
			Audio::AudioAuthoringSnapshotData data;
			Audio::AudioAuthoringSourceRecord source; source.identity = {TerraEngine::UUID{81U}, 1U, 1U}; source.clipAssetId = TerraEngine::UUID{82U}; source.clipGeneration = 3U; source.clipFrames = 48000U; source.spatial = false;
			data.sources.push_back(source);
			std::vector<Audio::AudioPlaybackSeed> playback{{source.identity, Audio::AudioPlaybackState::Playing, 1234U, TerraEngine::UUID{83U}, 2U}};
			const auto prepared = Audio::PrepareAudioFrame(Audio::AudioAuthoringSnapshot::Create(std::move(data)), playback);
			context.Expect(playback[0].state == Audio::AudioPlaybackState::Stopped && playback[0].logicalFrame == 0U
				&& prepared.voices[0].audibility == Audio::AudioAudibility::PendingAsset,
				"Changing a source clip generation must stop the old logical instance until an explicit new Play.");
		});

		registry.Add("Audio.Playback.PlayOnStartMissingClipAndSceneRemovalStaySafe", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& system = Audio::AudioSystem::Instance();
			objects.clear();
			system.Shutdown();
			// An unavailable provider keeps this lifecycle test headless while the
			// protected AudioCommit path still publishes deterministic snapshots.
			system.SetConfiguredProvider("test.missing");

			Object sourceObject = objects.createObject("Play On Start Source");
			auto* source = objects.AddComponent<AudioSourceComponent>(sourceObject);
			(void)objects.AddComponent<Transform>(sourceObject);
			source->playOnStart = true;
			source->SetSpatialBlend(0.0F);
			source->clipAssetId = TerraEngine::UUID{101U};

			system.Initialize();
			system.Update(1.0F / 60.0F);
			const auto firstPublication = system.LatestPlaybackSnapshot();
			context.Expect(firstPublication && firstPublication->voices.size() == 1U
				&& firstPublication->voices.front().audibility == Audio::AudioAudibility::PendingAsset,
				"Play On Start must create one safe pending instance when its authored clip is not published yet.");

			// Clearing the object simulates scene removal. The following protected
			// snapshot must prune the orphan instead of retaining a stale voice.
			objects.clear();
			system.Update(1.0F / 60.0F);
			const auto removedPublication = system.LatestPlaybackSnapshot();
			context.Expect(removedPublication && removedPublication->voices.empty(),
				"Removing an audio-source object must remove its logical instance at the next AudioCommit boundary.");

			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
	});
	}
}
