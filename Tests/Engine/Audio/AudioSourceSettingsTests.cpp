// AudioSourceSettingsTests.cpp - live capture regressions for durable controls.

#include "../TestHarness.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/TransactionalMutation.h"

#include <cstdint>
#include <utility>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests {
	namespace {
		// The installed runner passes a fresh destination. Intermediate receipts
		// remain failed until every required independent behavior has succeeded.
		void RecordSettingsEvidence(const char* behavior, nlohmann::json frames = nullptr) {
			const char* destination = std::getenv("TERRAENGINE_AUDIO_SETTINGS_EVIDENCE");
			if (!destination || !*destination) return;
			static std::mutex evidenceMutex;
			static nlohmann::json evidence{{"passed", false}, {"frameCount", 0U}, {"frames", nlohmann::json::array()},
				{"rollbackVerified", false}, {"authoredOverrideVerified", false}, {"retirementVerified", false}, {"independentFieldsVerified", false}};
			std::scoped_lock lock(evidenceMutex);
			if (behavior) evidence[behavior] = true;
			if (frames.is_array()) { evidence["frames"] = std::move(frames); evidence["frameCount"] = evidence["frames"].size(); }
			evidence["passed"] = evidence["frameCount"].get<std::size_t>() >= 120U
				&& evidence["rollbackVerified"].get<bool>() && evidence["authoredOverrideVerified"].get<bool>()
				&& evidence["retirementVerified"].get<bool>() && evidence["independentFieldsVerified"].get<bool>();
			std::ofstream output(destination, std::ios::trunc);
			if (!output || !(output << evidence.dump(2))) throw std::runtime_error("Could not write audio settings evidence.");
		}
		// Own a genuine scene source rather than injecting an authoring snapshot.
		// A missing provider keeps the test deterministic and device independent;
		// pending voices still publish their computed gain and pitch each frame.
		struct LiveSourceSettingsFixture {
			ObjectManager& objects = ObjectManager::instance();
			Audio::AudioSystem& system = Audio::AudioSystem::Instance();
			Audio::AudioMixSnapshot previousMix = system.LatestMixSnapshot();
			Object object;
			AudioSourceComponent* source = nullptr;

			LiveSourceSettingsFixture() {
				ComponentRegistry::RegisterAllComponents();
				system.Shutdown();
				objects.clear();
				system.SetConfiguredProvider("test.missing");
				std::string diagnostic;
				(void)system.ReplaceMixSnapshot({}, diagnostic);
				system.SetRuntimePlaybackEnabled(true);
				object = objects.createObject("Live source setting regression");
				source = objects.AddComponent<AudioSourceComponent>(object);
				(void)objects.AddComponent<Transform>(object);
				source->playOnStart = true;
				source->SetSpatialBlend(0.0F);
				system.Initialize();
				system.Update(1.0F / 60.0F);
			}

			~LiveSourceSettingsFixture() {
				// Restore singleton state even when a red assertion throws.
				objects.SetOnModifiedCallback({});
				system.Shutdown();
				std::string diagnostic;
				(void)system.ReplaceMixSnapshot(std::move(previousMix), diagnostic);
				system.SetConfiguredProvider("terra.miniaudio");
				system.SetRuntimePlaybackEnabled(true);
				objects.clear();
			}

			Audio::AudioControlReceipt Submit(Audio::AudioControlKind kind, float value) {
				Audio::AudioControlCommand command;
				command.kind = kind;
				command.source.objectId = objects.getUUID(object);
				command.source.sceneGeneration = objects.GetSceneGeneration();
				command.source.objectGeneration = static_cast<decltype(command.source.objectGeneration)>(objects.getObjectLifetimeGeneration(object));
				command.source.componentGeneration = static_cast<decltype(command.source.componentGeneration)>(objects.getObjectComponentGeneration(object));
				command.scalarValue = value;
				Audio::AudioControlBatch batch;
				batch.commands.push_back(command);
				return system.SubmitControlBatch(std::move(batch));
			}
			Audio::AudioSourceAttachmentKey Key() const {
				return {objects.getUUID(object), objects.GetSceneGeneration(), objects.getObjectLifetimeGeneration(object), objects.getObjectComponentGeneration(object)};
			}
		};

		void CheckLiveSettingPersistence(TestContext& context, Audio::AudioControlKind kind, float value) {
			LiveSourceSettingsFixture fixture;
			const auto original = fixture.system.LatestPlaybackSnapshot();
			context.Expect(original && original->voices.size() == 1U,
				"The regression requires a real captured autoplay source before submitting its control.");
			const auto queued = fixture.Submit(kind, value);
			context.Expect(queued.status == Audio::AudioControlStatus::Queued,
				"The source setting must enter the normal protected audio command queue.");
			fixture.system.Update(1.0F / 60.0F);
			const auto receipt = fixture.system.ReadControlReceipt(queued.batchSequence);
			context.Expect(receipt && receipt->status == Audio::AudioControlStatus::Applied,
				"The initial source-setting control must apply before checking later captures.");
			const bool volume = kind == Audio::AudioControlKind::SetSourceVolume;
			const auto controlled = fixture.system.LatestPlaybackSnapshot();
			context.Expect(controlled && controlled->voices.size() == 1U,
				"The controlled source must remain present in the published playback snapshot.");
			context.ExpectEqual(value, volume ? controlled->voices.front().gain : controlled->voices.front().pitch,
				"The control boundary must publish the requested source value.");

			// Every Update repeats live ObjectManager capture. On the original code
			// the first later frame reads 1.0 back from the unchanged component.
			for(std::uint32_t frame = 0U; frame < 120U; ++frame) {
				fixture.system.Update(1.0F / 60.0F);
				const auto later = fixture.system.LatestPlaybackSnapshot();
				context.Expect(later && later->voices.size() == 1U,
					"Every later live capture must preserve the controlled logical source.");
				context.ExpectEqual(value, volume ? later->voices.front().gain : later->voices.front().pitch,
					"A later live capture must retain the applied source setting instead of restoring 1.0.");
			}
			context.ExpectEqual(value, volume ? fixture.source->volume : fixture.source->pitch,
				"The existing durable component field must own the accepted source setting.");
			context.ExpectEqual(1.0F, volume ? fixture.source->pitch : fixture.source->volume,
				"Changing one source field must preserve the other authored field.");
		}
	}

	void RegisterAudioSourceSettingsTests(TestRegistry& registry) {
		registry.Add("Audio.SourceSettings.VolumeSurvives120LiveCaptures", [](TestContext& context) {
			CheckLiveSettingPersistence(context, Audio::AudioControlKind::SetSourceVolume, 0.25F);
		});
		registry.Add("Audio.SourceSettings.PitchSurvives120LiveCaptures", [](TestContext& context) {
			CheckLiveSettingPersistence(context, Audio::AudioControlKind::SetSourcePitch, 1.5F);
		});
		registry.Add("Audio.SourceSettings.BothFieldsProduce120ObservedEvidenceFrames", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			(void)fixture.Submit(Audio::AudioControlKind::SetSourcePitch, 1.5F);
			fixture.system.Update(1.0F / 60.0F);
			nlohmann::json rows = nlohmann::json::array();
			for (unsigned frame = 0U; frame < 120U; ++frame) {
				fixture.system.Update(1.0F / 60.0F);
				const auto snapshot = fixture.system.LatestPlaybackSnapshot();
				context.Expect(snapshot && snapshot->voices.size() == 1U, "Each evidence row must come from an actual later live capture.");
				const auto& voice = snapshot->voices.front();
				context.Expect(voice.gain == 0.25F && voice.pitch == 1.5F, "Both controlled fields must persist through every observed frame.");
				rows.push_back({{"frame", snapshot->frame}, {"expectedVolume", 0.25F}, {"actualVolume", voice.gain}, {"expectedPitch", 1.5F}, {"actualPitch", voice.pitch}});
			}
			RecordSettingsEvidence(nullptr, std::move(rows));
		});
		registry.Add("Audio.SourceSettings.OtherFieldsAndSourcesRemainIndependent", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			const Object other = fixture.objects.createObject("Independent source");
			auto* component = fixture.objects.AddComponent<AudioSourceComponent>(other);
			(void)fixture.objects.AddComponent<Transform>(other);
			component->volume = 0.75F; component->pitch = 2.0F;
			fixture.system.Update(0.0F);
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			fixture.system.Update(0.0F);
			context.Expect(fixture.source->volume == 0.25F && fixture.source->pitch == 1.0F
				&& component->volume == 0.75F && component->pitch == 2.0F, "A field-scoped source control cannot overwrite another field or source.");
			RecordSettingsEvidence("independentFieldsVerified");
		});
		registry.Add("Audio.SourceSettings.LaterControlSupersedesEarlierValue", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			fixture.system.Update(0.0F);
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.6F);
			fixture.system.Update(0.0F); fixture.system.Update(0.0F);
			context.ExpectEqual(0.6F, fixture.source->volume, "A later accepted control must become the durable source value.");
		});
		registry.Add("Audio.SourceSettings.AuthoredAndSameValueWritesPreservePlaybackIdentity", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			const auto key = fixture.Key();
			const auto binding = fixture.objects.getObjectBindingGeneration(fixture.object);
			const auto identity = fixture.system.LatestPlaybackSnapshot()->voices.front().identity;
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			fixture.system.Update(0.0F);
			for (unsigned repeat = 0U; repeat < 2U; ++repeat) {
				const auto revision = fixture.objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName);
				const auto result = Scene::CommitTransaction(fixture.objects, Scene::TransactionalMutationBuilder("authored-override", Scene::TransactionOrigin::Editor)
					.ObserveScene(fixture.objects).ObserveObject(fixture.objects, key.objectId)
					.UpdateComponentProperty(key.objectId, "AudioSourceComponent", "volume", Reflection::Variant{1.0F}).Build());
				context.Expect(result.status == Scene::TransactionStatus::Committed
					&& fixture.objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName) > revision,
					"Both an authored override and a subsequent same-value edit must publish value revisions.");
				fixture.system.Update(0.0F);
				context.Expect(fixture.system.LatestPlaybackSnapshot()->voices.front().gain == 1.0F
					&& fixture.system.LatestPlaybackSnapshot()->voices.front().identity == identity
					&& fixture.Key() == key && fixture.objects.getObjectBindingGeneration(fixture.object) == binding,
					"Authored source values supersede controls while preserving playback, component and wrapper identity.");
			}
			RecordSettingsEvidence("authoredOverrideVerified");
		});
		registry.Add("Audio.SourceSettings.MixedInvalidBatchRejectsEverySourceWrite", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			Audio::AudioControlCommand valid;
			valid.kind = Audio::AudioControlKind::SetSourceVolume;
			valid.source = {fixture.Key().objectId, fixture.Key().objectGeneration, fixture.Key().componentGeneration, 0U, fixture.Key().sceneGeneration};
			valid.scalarValue = 0.25F;
			auto invalid = valid; invalid.source.objectId = TerraEngine::UUID{99U};
			Audio::AudioControlBatch batch; batch.commands = {valid, invalid};
			const auto admitted = fixture.system.SubmitControlBatch(std::move(batch));
			fixture.system.Update(0.0F);
			const auto receipt = fixture.system.ReadControlReceipt(admitted.batchSequence);
			context.Expect((admitted.status == Audio::AudioControlStatus::Rejected || (receipt && receipt->status == Audio::AudioControlStatus::Rejected))
				&& fixture.source->volume == 1.0F, "A malformed sibling command must reject the whole batch without an earlier field write.");
		});
		registry.Add("Audio.SourceSettings.InjectedRollbackRestoresRealValuesAndRevisions", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			const auto revision = fixture.objects.GetComponentMutationRevision();
			auto transaction = Scene::TransactionalMutationBuilder("audio-rollback-evidence", Scene::TransactionOrigin::Test)
				.UpdateAudioSourceSettingsBatch({{fixture.Key(), 0.25F, 1.5F}})
				.SetName(fixture.Key().objectId, "must not publish").InjectCommitFailureForTesting(1U).Build();
			const auto result = Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == Scene::TransactionStatus::Rejected && fixture.source->volume == 1.0F
				&& fixture.source->pitch == 1.0F && revision == fixture.objects.GetComponentMutationRevision(), "Injected failure after actual settings writes must restore payload and publication metadata.");
			RecordSettingsEvidence("rollbackVerified");
		});
		registry.Add("Audio.SourceSettings.RemovalRetiresPendingAttachmentWithoutChangingReplacement", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			const auto queued = fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			fixture.objects.RemoveComponent<AudioSourceComponent>(fixture.object);
			auto* replacement = fixture.objects.AddComponent<AudioSourceComponent>(fixture.object);
			replacement->playOnStart = true;
			fixture.source = replacement;
			fixture.system.Update(0.0F);
			const auto result = fixture.system.ReadControlReceipt(queued.batchSequence);
			context.Expect(result && result->status == Audio::AudioControlStatus::Rejected && replacement->volume == 1.0F,
				"An admitted old attachment must retire without altering a replacement source.");
			RecordSettingsEvidence("retirementVerified");
		});
		registry.Add("Audio.SourceSettings.PendingAndRuntimePausedSourcesRetainControls", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			fixture.system.SetRuntimePlaybackPaused(true);
			(void)fixture.Submit(Audio::AudioControlKind::SetSourcePitch, 0.01F);
			fixture.system.Update(1.0F);
			fixture.system.Update(1.0F);
			context.Expect(fixture.source->pitch == 0.01F && fixture.system.LatestPlaybackSnapshot()->voices.front().pitch == 0.01F,
				"Pending assets and game pause cannot erase durable pitch or raise its runtime minimum.");
		});
		registry.Add("Audio.SourceSettings.ManualSnapshotReplacementRemainsValueOwned", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			Audio::AudioAuthoringSnapshotData data;
			Audio::AudioAuthoringSourceRecord source;
			source.identity = fixture.system.LatestPlaybackSnapshot()->voices.front().identity;
			source.attachment = fixture.Key(); source.volume = 0.8F; source.spatial = false;
			data.sources.push_back(source);
			fixture.system.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data));
			(void)fixture.Submit(Audio::AudioControlKind::SetSourceVolume, 0.25F);
			fixture.system.Update(0.0F);
			context.ExpectEqual(1.0F, fixture.source->volume, "Manual snapshot controls must not invent live source mutations.");
			data.sources.front().volume = 0.6F;
			fixture.system.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data));
			fixture.system.Update(0.0F);
			context.ExpectEqual(0.6F, fixture.system.LatestPlaybackSnapshot()->voices.front().gain, "Replacing an owned manual snapshot must supersede its earlier controlled value.");
		});
		registry.Add("Audio.SourceSettings.PostcommitObserverFailureRetainsAcceptedSourceValue", [](TestContext& context) {
			LiveSourceSettingsFixture fixture;
			Audio::AudioControlCommand setting;
			setting.kind = Audio::AudioControlKind::SetSourceVolume;
			const auto key = fixture.Key();
			setting.source = {key.objectId, key.objectGeneration, key.componentGeneration, 0U, key.sceneGeneration};
			setting.scalarValue = 0.25F;
			Audio::AudioControlCommand master;
			master.kind = Audio::AudioControlKind::SetMasterVolume;
			master.scalarValue = 0.5F;
			Audio::AudioControlBatch batch; batch.commands = {setting, master};
			const auto admitted = fixture.system.SubmitControlBatch(std::move(batch));
			const auto playback = fixture.system.LatestPlaybackSnapshot()->voices.front().identity;
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Audio source observer failure after commit"); });
			bool reportedFailure = false;
			try { fixture.system.Update(0.0F); }
			catch (const Scene::TransactionPublicationFailure&) { reportedFailure = true; }
			fixture.objects.SetOnModifiedCallback({});
			const auto receipt = fixture.system.ReadControlReceipt(admitted.batchSequence);
			context.Expect(reportedFailure && fixture.source->volume == 0.25F
				&& fixture.system.LatestMixSnapshot().master.volume == 0.5F
				&& receipt && receipt->status == Audio::AudioControlStatus::Applied,
				"Postcommit observer failure must retain the source write, adopt companion mix and truthfully publish an Applied receipt.");
			fixture.system.Update(0.0F);
			context.ExpectEqual(0.125F, fixture.system.LatestPlaybackSnapshot()->voices.front().gain,
				"After observer recovery the companion state must agree with the committed durable setting.");
			context.Expect(fixture.system.LatestPlaybackSnapshot()->voices.front().identity == playback,
				"Companion adoption must preserve the already accepted playback identity.");
		});
		registry.Add("Audio.SourceSettings.MutedPausedAndVirtualizedManualVoicesRetainPitch", [](TestContext& context) {
			for (unsigned state = 0U; state < 3U; ++state) {
				LiveSourceSettingsFixture fixture;
				Audio::AudioAuthoringSnapshotData data;
				Audio::AudioAuthoringSourceRecord source;
				source.identity = fixture.system.LatestPlaybackSnapshot()->voices.front().identity;
				source.attachment = fixture.Key(); source.spatial = false;
				source.clipAssetId = TerraEngine::UUID{43U}; source.clipGeneration = 1U;
				source.clipFrames = 100000U; source.clipSampleRate = 100U;
				data.sources.push_back(source);
				data.voiceLimit = state == 2U ? 0U : 64U;
				fixture.system.SetAuthoringSnapshot(Audio::AudioAuthoringSnapshot::Create(data));
				if (state == 0U) { std::string diagnostic; (void)fixture.system.SetMasterMix(1.0F, true, diagnostic); }
				if (state == 1U) {
					Audio::AudioControlCommand pause;
					pause.kind = Audio::AudioControlKind::Pause;
					const auto key = fixture.Key();
					pause.source = {key.objectId, key.objectGeneration, key.componentGeneration, 0U, key.sceneGeneration};
					pause.targetMode = Audio::AudioControlTargetMode::BindCurrentAtAdmission;
					Audio::AudioControlBatch batch; batch.commands.push_back(pause);
					(void)fixture.system.SubmitControlBatch(std::move(batch));
				}
				(void)fixture.Submit(Audio::AudioControlKind::SetSourcePitch, 1.5F);
				fixture.system.Update(0.1F); fixture.system.Update(0.1F);
				const auto snapshot = fixture.system.LatestPlaybackSnapshot();
				context.Expect(snapshot && snapshot->voices.size() == 1U && snapshot->voices.front().pitch == 1.5F,
					"Muted, paused and voice-limited logical sources must retain accepted pitch across later captures.");
				const auto& voice = snapshot->voices.front();
				context.Expect(state == 0U ? voice.audibility == Audio::AudioAudibility::MasterMuted
					: state == 1U ? voice.paused : voice.audibility == Audio::AudioAudibility::VoiceLimited,
					"The state-specific source-setting proof must actually reach its muted, paused or virtualized condition.");
			}
		});
	}
}
