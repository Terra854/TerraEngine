// AudioControlIntegrationTests.cpp - provider-neutral recovery packet integration checks.

#include "../TestHarness.h"
#include "Audio/AudioPreparationPlan.h"
#include "Audio/AudioSystem.h"
#include "FakeAudioBackend.h"

#include <cmath>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeRecoveryClipId() {
			TerraEngine::UUID id{};
			id.back() = 73U;
			return id;
		}
	}

	void RegisterAudioControlIntegrationTests(TestRegistry& registry) {
		registry.Add("Audio.ControlIntegration.PreparedMixDiscardsWithoutPartialPublication", [](TestContext& context) {
			TerraEngine::Audio::AudioMixConfiguration mix;
			const auto before = mix.Snapshot();
			std::string diagnostic;
			{
				auto candidate = before;
				candidate.master.volume = 0.25F;
				auto guard = mix.PrepareReplace(std::move(candidate), before.generation, diagnostic);
				context.Expect(guard.has_value(), "A valid copied generation should prepare an exclusive mix publication.");
				// Discard before the scene transaction commits: destruction releases the
				// lock and leaves the entire prior mix generation unchanged.
			}
			const auto after = mix.Snapshot();
			context.Expect(after.generation == before.generation && after.master.volume == before.master.volume,
				"Discarding a prepared companion must preserve the old mix completely and release its lock.");
		});
		registry.Add("Audio.ControlIntegration.PreparedMixNormalizesBeforeNoFailCommit", [](TestContext& context) {
			TerraEngine::Audio::AudioMixConfiguration mix;
			auto candidate = mix.Snapshot();
			const auto generation = candidate.generation;
			candidate.master.volume = 4.0F;
			candidate.groups.front().volume = -2.0F;
			std::string diagnostic;
			{
				auto guard = mix.PrepareReplace(std::move(candidate), generation, diagnostic);
				context.Expect(guard && guard->Candidate().master.volume == 1.0F && guard->Candidate().groups.front().volume == 0.0F,
					"Normalization and all validation must finish before the caller can commit scene settings.");
				guard->Commit();
				guard->Commit();
			}
			const auto after = mix.Snapshot();
			context.Expect(after.generation == generation + 1U && after.master.volume == 1.0F && after.groups.front().volume == 0.0F,
				"No-fail publication must adopt one complete normalized generation exactly once.");
		});
		registry.Add("Audio.ControlIntegration.PreparedMixRejectsStaleAndMalformedCandidates", [](TestContext& context) {
			TerraEngine::Audio::AudioMixConfiguration mix;
			const auto stale = mix.Snapshot();
			std::string diagnostic;
			context.Expect(mix.SetMasterVolume(0.5F, diagnostic), "An intervening authored edit must advance the generation.");
			context.Expect(!mix.PrepareReplace(stale, stale.generation, diagnostic), "A stale prepared generation must reject before scene publication.");
			auto malformed = mix.Snapshot();
			malformed.groups.push_back(malformed.groups.front());
			context.Expect(!mix.PrepareReplace(malformed, malformed.generation, diagnostic), "Duplicate groups must reject without reserving a publication.");
			context.ExpectEqual(0.5F, mix.Snapshot().master.volume, "Neither rejection may partially restore an old master value.");
		});
		registry.Add("Audio.ControlIntegration.PauseResumeStopAndReplayRemainDeterministic", [](TestContext& context) {
			auto& system = TerraEngine::Audio::AudioSystem::Instance();
			system.Shutdown();
			system.SetConfiguredProvider("test.missing");

			TerraEngine::Audio::AudioAuthoringSnapshotData data;
			TerraEngine::Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::UUID{61U}, 1U, 1U};
			source.clipAssetId = MakeRecoveryClipId();
			source.clipGeneration = 1U;
			source.clipFrames = 10000U;
			source.clipSampleRate = 100U;
			source.spatial = false;
			data.sources.push_back(source);
			system.SetAuthoringSnapshot(TerraEngine::Audio::AudioAuthoringSnapshot::Create(data));
			system.Initialize();

			TerraEngine::Audio::AudioGeneration lastPlayGeneration = 0U;
			auto submit = [&](TerraEngine::Audio::AudioControlKind kind, float deltaSeconds) {
				TerraEngine::Audio::AudioControlBatch batch;
				TerraEngine::Audio::AudioControlCommand command;
				command.kind = kind;
				command.source = {source.identity.objectId, 1U, 1U, 0U};
				// Controls bind the engine-allocated predecessor, including autoplay;
				// callers no longer synthesize playback identities in wrapper counters.
				command.targetMode = TerraEngine::Audio::AudioControlTargetMode::BindCurrentAtAdmission;
				batch.commands.push_back(command);
				const auto queued = system.SubmitControlBatch(std::move(batch));
				system.Update(deltaSeconds);
				const auto receipt = system.ReadControlReceipt(queued.batchSequence);
				context.Expect(receipt && receipt->status == TerraEngine::Audio::AudioControlStatus::Applied,
					"Each lifecycle command must become visible only after its protected AudioCommit boundary.");
				if(kind == TerraEngine::Audio::AudioControlKind::Play) lastPlayGeneration = receipt->resolvedPlaybackGeneration;
				return system.LatestPlaybackSnapshot();
			};

			auto snapshot = submit(TerraEngine::Audio::AudioControlKind::Play, 1.0F);
			context.Expect(snapshot && snapshot->voices.size() == 1U && snapshot->voices[0].logicalFrame == 100U,
				"Play should create one logical instance and advance it from frame zero.");
			snapshot = submit(TerraEngine::Audio::AudioControlKind::Pause, 1.0F);
			context.Expect(snapshot->voices[0].paused && snapshot->voices[0].logicalFrame == 100U,
				"Pause must preserve the exact logical cursor.");
			snapshot = submit(TerraEngine::Audio::AudioControlKind::Resume, 1.0F);
			context.Expect(!snapshot->voices[0].paused && snapshot->voices[0].logicalFrame == 200U,
				"Resume must continue from the paused cursor instead of restarting.");
			snapshot = submit(TerraEngine::Audio::AudioControlKind::Stop, 1.0F);
			context.Expect(snapshot->voices[0].logicalFrame == 0U,
				"Stop must reset the logical cursor before a later replay.");

			// Repeated stop/play cycles use fresh playback generations, matching a
			// recorded gameplay command stream while proving the final snapshot is stable.
			for(std::uint32_t replay = 0U; replay < 100U; ++replay) {
				TerraEngine::Audio::AudioControlBatch replayBatch;
				if(replay != 0U) {
					TerraEngine::Audio::AudioControlCommand stop;
					stop.kind = TerraEngine::Audio::AudioControlKind::Stop;
					stop.source = {source.identity.objectId, 1U, 1U, 0U};
					stop.targetMode = TerraEngine::Audio::AudioControlTargetMode::BindCurrentAtAdmission;
					replayBatch.commands.push_back(stop);
				}
				replayBatch.commands.push_back({TerraEngine::Audio::AudioControlKind::Play,
					{source.identity.objectId, 1U, 1U, 0U}});
				const auto admitted = system.SubmitControlBatch(std::move(replayBatch));
				system.Update(1.0F);
				const auto receipt = system.ReadControlReceipt(admitted.batchSequence);
				context.Expect(receipt && receipt->status == TerraEngine::Audio::AudioControlStatus::Applied
					&& receipt->resolvedPlaybackGeneration > lastPlayGeneration,
					"Every replay must apply and publish a fresh monotonic engine-owned playback identity.");
				lastPlayGeneration = receipt->resolvedPlaybackGeneration;
				snapshot = system.LatestPlaybackSnapshot();
				context.Expect(snapshot != nullptr,
					"Every recorded replay must publish an immutable playback snapshot.");
				context.ExpectEqual<std::size_t>(1U, snapshot->voices.size(),
					"Every recorded replay must publish exactly one logical voice.");
				context.ExpectEqual(lastPlayGeneration, snapshot->voices[0].identity.playbackGeneration,
					"Every recorded replay must publish the newly assigned playback generation.");
				context.ExpectEqual<TerraEngine::Audio::AudioFrame>(100U, snapshot->voices[0].logicalFrame,
					"Every recorded replay must publish the same final logical cursor.");
			}
			system.Shutdown();
			system.SetConfiguredProvider("terra.miniaudio");
		});

		registry.Add("Audio.ControlIntegration.MuteMixPitchAndGamePauseStayIndependent", [](TestContext& context) {
			TerraEngine::Audio::AudioAuthoringSnapshotData data;
			data.deltaSeconds = 1.0F;
			data.masterVolume = 0.5F;
			TerraEngine::Audio::AudioAuthoringSourceRecord source;
			source.identity = {TerraEngine::UUID{62U}, 1U, 1U};
			source.clipAssetId = MakeRecoveryClipId();
			source.clipGeneration = 1U;
			source.clipFrames = 10000U;
			source.clipSampleRate = 100U;
			source.volume = 0.5F;
			source.mixVolume = 0.4F;
			source.pitch = 2.0F;
			source.spatial = false;
			data.sources.push_back(source);
			std::vector<TerraEngine::Audio::AudioPlaybackSeed> playback{{source.identity,
				TerraEngine::Audio::AudioPlaybackState::Playing, 0U}};

			auto prepared = TerraEngine::Audio::PrepareAudioFrame(
				TerraEngine::Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(prepared.voices.size() == 1U
				&& std::abs(prepared.voices[0].gain - 0.1F) < 0.0001F
				&& prepared.voices[0].pitch == 2.0F && playback[0].logicalFrame == 200U,
				"Source, mix, and master gains must compose independently while pitch scales logical time.");

			data.sources[0].mixMuted = true;
			prepared = TerraEngine::Audio::PrepareAudioFrame(
				TerraEngine::Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(prepared.voices[0].gain == 0.0F && playback[0].logicalFrame == 400U,
				"Group mute must silence the voice without freezing its timeline.");
			data.gamePaused = true;
			const auto pausedFrame = playback[0].logicalFrame;
			prepared = TerraEngine::Audio::PrepareAudioFrame(
				TerraEngine::Audio::AudioAuthoringSnapshot::Create(data), playback);
			context.Expect(prepared.voices[0].gain == 0.0F && playback[0].logicalFrame == pausedFrame,
				"Game pause must freeze logical time independently from an output mute.");
		});

		registry.Add("Audio.ControlIntegration.RecoveryPacketReconcilesExistingVoiceState", [](TestContext& context) {
			Audio::FakeAudioBackend backend;
			context.Expect(backend.Initialize({}).Succeeded(),
				"The deterministic provider should initialize before recovery packet integration checks.");
			const auto payload = std::make_shared<const std::vector<std::uint8_t>>(64U, 4U);
			const TerraEngine::UUID clipId = MakeRecoveryClipId();

			TerraEngine::Audio::AudioBackendFramePacket initial;
			initial.sequence = 1U;
			initial.registrations.push_back({clipId, 1U, TerraEngine::Audio::AudioClipEncoding::Wav,
				48000U, 2U, 48U, 1U, payload});
			initial.voices.push_back({9U, {TerraEngine::UUID{7U}, 1U, 1U}, clipId, 1U,
				120U, 0.75F, 1.0F, true, false, false, false, {}});
			context.Expect(backend.SubmitFrame(initial).Succeeded(),
				"The initial complete packet should establish the retained clip and voice state.");

			// A recovery packet intentionally repeats the complete currently audible
			// state instead of sending an SDK-specific restart command. This is the
			// provider-neutral contract Miniaudio uses to seek after a device restart.
			TerraEngine::Audio::AudioBackendFramePacket recovery;
			recovery.sequence = 2U;
			recovery.requestRecovery = true;
			recovery.voices.push_back({9U, {TerraEngine::UUID{7U}, 1U, 1U}, clipId, 1U,
				360U, 0.75F, 1.0F, false, false, false, false, {}});
			context.Expect(backend.SubmitFrame(recovery).Succeeded(),
				"A complete recovery packet should reuse retained clips and update the existing voice.");
			context.Expect(backend.LiveVoiceCount() == 1U && backend.RegisteredClipCount() == 1U
				&& backend.Packets().back().requestRecovery,
				"Recovery reconciliation must preserve provider-owned resources while carrying the explicit request flag.");
		});
	}
}
