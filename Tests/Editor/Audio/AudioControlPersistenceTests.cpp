// AudioControlPersistenceTests.cpp - runtime controls retain existing saved fields.

#include "TestHarness.h"
#include "Audio/AudioSystem.h"
#include "Components/AudioSourceComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"
#include "Scene/TransactionalMutation.h"

#include <utility>

namespace TerraEngine::EditorTests {
	namespace {
		// A real source ensures the next frame exercises the editor's live capture
		// path. No test-authored AudioAuthoringSnapshot can hide an overwritten field.
		struct EditorLiveControlFixture {
			ObjectManager& objects = ObjectManager::instance();
			Audio::AudioSystem& audio = Audio::AudioSystem::Instance();
			Audio::AudioMixSnapshot previousMix = audio.LatestMixSnapshot();
			Object object;
			AudioSourceComponent* source = nullptr;

			EditorLiveControlFixture() {
				ComponentRegistry::RegisterAllComponents();
				audio.Shutdown();
				objects.clear();
				audio.SetConfiguredProvider("test.missing");
				std::string diagnostic;
				(void)audio.ReplaceMixSnapshot({}, diagnostic);
				audio.SetRuntimePlaybackEnabled(true);
				object = objects.createObject("Editor live audio control");
				source = objects.AddComponent<AudioSourceComponent>(object);
				(void)objects.AddComponent<Transform>(object);
				source->playOnStart = true;
				source->SetSpatialBlend(0.0F);
				audio.Initialize();
				audio.Update(1.0F / 60.0F);
			}

			~EditorLiveControlFixture() {
				// Assertion failures must not leak a live runtime into another test.
				objects.SetOnModifiedCallback({});
				audio.Shutdown();
				std::string diagnostic;
				(void)audio.ReplaceMixSnapshot(std::move(previousMix), diagnostic);
				audio.SetConfiguredProvider("terra.miniaudio");
				audio.SetRuntimePlaybackEnabled(true);
				objects.clear();
			}

			Audio::AudioControlReceipt SubmitVolume(float value, Audio::AudioControlKind kind = Audio::AudioControlKind::SetSourceVolume) {
				Audio::AudioControlCommand command;
				command.kind = kind;
				command.source.objectId = objects.getUUID(object);
				command.source.sceneGeneration = objects.GetSceneGeneration();
				command.source.objectGeneration = static_cast<decltype(command.source.objectGeneration)>(objects.getObjectLifetimeGeneration(object));
				command.source.componentGeneration = static_cast<decltype(command.source.componentGeneration)>(objects.getObjectComponentGeneration(object));
				command.scalarValue = value;
				Audio::AudioControlBatch batch;
				batch.commands.push_back(command);
				return audio.SubmitControlBatch(std::move(batch));
			}
		};
	}

	void RegisterAudioControlPersistenceTests(TestRegistry& registry) {
		registry.Add("Editor.AudioControlPersistence.AcceptedVolumeSurvivesLiveCaptureAndReload", [](TestContext& context) {
			EditorLiveControlFixture fixture;
			const auto queued = fixture.SubmitVolume(0.25F);
			fixture.audio.Update(1.0F / 60.0F);
			const auto receipt = fixture.audio.ReadControlReceipt(queued.batchSequence);
			context.Expect(receipt && receipt->status == Audio::AudioControlStatus::Applied,
				"The editor's source control must apply through the protected audio boundary.");
			fixture.audio.Update(1.0F / 60.0F);
			const auto captured = fixture.audio.LatestPlaybackSnapshot();
			context.Expect(captured && captured->voices.size() == 1U && captured->voices.front().gain == 0.25F,
				"The next editor live capture must retain the accepted volume rather than restoring 1.0.");

			// The existing serializer is the persistence contract: a control must
			// update its ordinary reflected field, with no secondary override format.
			const auto serialized = Serializer::SerializeObject(fixture.object);
			fixture.audio.Shutdown();
			fixture.objects.clear();
			const Object restored = Serializer::DeserializeObject(serialized);
			const auto* restoredSource = fixture.objects.GetComponent<AudioSourceComponent>(restored);
			context.Expect(restoredSource && restoredSource->volume == 0.25F && restoredSource->pitch == 1.0F,
				"Reload must preserve the controlled volume and the unrelated authored pitch.");
		});

		registry.Add("Editor.AudioControlPersistence.PendingControlRemainsInvisibleBeforeAudioBoundary", [](TestContext& context) {
			EditorLiveControlFixture fixture;
			const auto before = fixture.audio.LatestPlaybackSnapshot();
			const auto queued = fixture.SubmitVolume(0.25F);
			context.Expect(queued.status == Audio::AudioControlStatus::Queued
				&& fixture.source->volume == 1.0F && fixture.audio.LatestPlaybackSnapshot() == before,
				"Producer admission must not expose component or snapshot writes before AudioSnapshot.");
		});
		registry.Add("Editor.AudioControlPersistence.PitchUsesExistingSerializedField", [](TestContext& context) {
			EditorLiveControlFixture fixture;
			(void)fixture.SubmitVolume(1.5F, Audio::AudioControlKind::SetSourcePitch);
			fixture.audio.Update(0.0F); fixture.audio.Update(0.0F);
			context.Expect(fixture.source->pitch == 1.5F && fixture.source->volume == 1.0F,
				"Pitch controls must retain their existing durable field while preserving authored volume.");
			const auto serialized = Serializer::SerializeObject(fixture.object);
			fixture.audio.Shutdown(); fixture.objects.clear();
			const auto restored = Serializer::DeserializeObject(serialized);
			context.ExpectEqual(1.5F, fixture.objects.GetComponent<AudioSourceComponent>(restored)->pitch,
				"Existing scene serialization must preserve the accepted pitch without a new format.");
		});
		registry.Add("Editor.AudioControlPersistence.AuthoredOverrideUndoRetainsPlaybackAndSourceIdentity", [](TestContext& context) {
			EditorLiveControlFixture fixture;
			(void)fixture.SubmitVolume(0.25F);
			fixture.audio.Update(0.0F);
			const auto component = fixture.objects.getObjectComponentGeneration(fixture.object);
			const auto binding = fixture.objects.getObjectBindingGeneration(fixture.object);
			const auto playback = fixture.audio.LatestPlaybackSnapshot()->voices.front().identity;
			const auto id = fixture.objects.getUUID(fixture.object);
			const auto result = Scene::CommitTransaction(fixture.objects, Scene::TransactionalMutationBuilder("audio-authored-override", Scene::TransactionOrigin::Editor)
				.ObserveScene(fixture.objects).ObserveObject(fixture.objects, id)
				.UpdateComponentProperty(id, "AudioSourceComponent", "volume", Reflection::Variant{1.0F}).Build());
			fixture.audio.Update(0.0F);
			context.Expect(result.status == Scene::TransactionStatus::Committed && fixture.source->volume == 1.0F,
				"A later authored edit must supersede the runtime control in the same existing source field.");
			context.Expect(Scene::ApplyTransactionInverseDeltas(fixture.objects, result.inverseDeltas), "Editor history must undo the authored edit through the protected transaction path.");
			fixture.audio.Update(0.0F);
			context.Expect(fixture.source->volume == 0.25F && component == fixture.objects.getObjectComponentGeneration(fixture.object)
				&& binding == fixture.objects.getObjectBindingGeneration(fixture.object)
				&& playback == fixture.audio.LatestPlaybackSnapshot()->voices.front().identity,
				"Undo must restore the earlier controlled value without replacing the component, facade or playback.");
		});
	}
}
