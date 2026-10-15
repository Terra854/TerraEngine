// AudioAuthoringPersistenceTests.cpp - durable component and queue-boundary coverage.

#include "TestHarness.h"

#include "Components/AudioListenerComponent.h"
#include "Components/AudioSourceComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "Scene/TransactionalMutation.h"
#include "UndoRedoSystem.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class AudioAuthoringToggleCommand final : public ICommand
		{
		public:
			AudioAuthoringToggleCommand(AudioSourceComponent& source, AudioListenerComponent& listener,
				bool sourceLoopBefore, bool sourceLoopAfter, bool listenerEligibleBefore, bool listenerEligibleAfter)
				: m_Source(source), m_Listener(listener), m_SourceLoopBefore(sourceLoopBefore),
				m_SourceLoopAfter(sourceLoopAfter), m_ListenerEligibleBefore(listenerEligibleBefore),
				m_ListenerEligibleAfter(listenerEligibleAfter)
			{
			}

			void Undo() override { m_Source.loop = m_SourceLoopBefore; m_Listener.eligible = m_ListenerEligibleBefore; }
			void Redo() override { m_Source.loop = m_SourceLoopAfter; m_Listener.eligible = m_ListenerEligibleAfter; }
			std::string GetName() const override { return "Audio authoring toggle"; }

		private:
			// The command retains component references only while this focused test
			// owns the scene; production undo uses serialized transaction payloads.
			AudioSourceComponent& m_Source;
			AudioListenerComponent& m_Listener;
			bool m_SourceLoopBefore = false;
			bool m_SourceLoopAfter = false;
			bool m_ListenerEligibleBefore = false;
			bool m_ListenerEligibleAfter = false;
		};
	}

	void RegisterAudioAuthoringPersistenceTests(TestRegistry& registry)
	{
		registry.Add("Editor.AudioAuthoring.SameValueSourceEditUsesValueRevisionOnly", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			auto& objects = ObjectManager::instance();
			objects.clear();
			const auto object = objects.createObject("Stable source value edit");
			auto* source = objects.AddComponent<AudioSourceComponent>(object);
			source->volume = 0.25F;
			const auto component = objects.getObjectComponentGeneration(object);
			const auto binding = objects.getObjectBindingGeneration(object);
			const auto valueRevision = objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName);
			const auto id = objects.getUUID(object);
			const auto result = TerraEngine::Scene::CommitTransaction(objects,
				TerraEngine::Scene::TransactionalMutationBuilder("same-source-value", TerraEngine::Scene::TransactionOrigin::Editor)
					.ObserveScene(objects).ObserveObject(objects, id)
					.UpdateComponentProperty(id, "AudioSourceComponent", "volume", TerraEngine::Reflection::Variant{0.25F}).Build());
			const bool valid = result.status == TerraEngine::Scene::TransactionStatus::Committed
				&& component == objects.getObjectComponentGeneration(object) && binding == objects.getObjectBindingGeneration(object)
				&& objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName) > valueRevision;
			objects.clear();
			context.Expect(valid, "Same-value authored source edits publish a value revision without changing component membership or managed binding identity.");
		});
		registry.Add("Editor.AudioAuthoring.PersistenceSerializationAndTransactionVisibility", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();
			Object sourceObject = objects.createObject("Audio Source");
			auto* source = objects.AddComponent<AudioSourceComponent>(sourceObject);
			auto* listener = objects.AddComponent<AudioListenerComponent>(sourceObject);
			source->clipAssetId = TerraEngine::UUID{17U};
			source->SetVolume(0.4F);
			source->SetPitch(1.5F);
			source->playOnStart = true;
			source->loop = true;
			source->SetSpatialBlend(0.25F);
			source->SetMaxDistance(80.0F);
			source->mixGroupId = TerraEngine::UUID{18U};
			source->SetPriority(200U);
			listener->eligible = false;
			listener->SetOutputModeValue(static_cast<std::uint32_t>(TerraEngine::Audio::AudioOutputMode::Hrtf));
			const nlohmann::json serialized = Serializer::SerializeObject(sourceObject);
			objects.clear();
			Object restored = Serializer::DeserializeObject(serialized);
			auto* restoredSource = objects.GetComponent<AudioSourceComponent>(restored);
			auto* restoredListener = objects.GetComponent<AudioListenerComponent>(restored);
			context.Expect(restoredSource != nullptr && restoredListener != nullptr
				&& restoredSource->clipAssetId == TerraEngine::UUID{17U} && restoredSource->volume == 0.4F
				&& restoredSource->pitch == 1.5F && restoredSource->playOnStart && restoredSource->loop
				&& restoredSource->spatialBlend == 0.25F && restoredSource->maxDistance == 80.0F
				&& restoredSource->mixGroupId == TerraEngine::UUID{18U}
				&& restoredSource->priority == 200U && !restoredListener->eligible
				&& restoredListener->outputMode == TerraEngine::Audio::AudioOutputMode::Hrtf,
				"Source and listener authored values must survive generic reflected scene serialization.");

			const TerraEngine::UUID restoredId = objects.getUUID(restored);
			TerraEngine::Scene::TransactionalMutationBuilder builder("audio-volume", TerraEngine::Scene::TransactionOrigin::Editor, "Audio Inspector");
			builder.ObserveScene(objects).ObserveObject(objects, restoredId)
				.UpdateComponentProperty(restoredId, "AudioSourceComponent", "volume", TerraEngine::Reflection::Variant{0.8F});
			queue.EnqueueTransaction(builder.Build());
			context.Expect(restoredSource->volume == 0.4F,
				"Audio inspector edits must remain invisible until the protected scene queue flush.");
			const auto applied = queue.Apply(objects, "audio-authoring-test");
			context.Expect(applied.applied && restoredSource->volume == 0.8F,
				"A valid queued audio property mutation must publish at the declared boundary.");
			queue.Clear();
			objects.clear();
		});

		registry.Add("Editor.AudioAuthoring.UndoRedoRestoresSourceAndListenerFields", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			UndoRedoSystem& history = UndoRedoSystem::Instance();
			objects.clear();
			history.Clear();
			Object object = objects.createObject("Undoable Audio Authoring");
			auto* source = objects.AddComponent<AudioSourceComponent>(object);
			auto* listener = objects.AddComponent<AudioListenerComponent>(object);
			source->loop = false;
			listener->eligible = true;

			// Simulate an Inspector edit already applied at its protected flush, then
			// prove one undo/redo history entry restores both authored components.
			source->loop = true;
			listener->eligible = false;
			history.PushCommand(std::make_unique<AudioAuthoringToggleCommand>(*source, *listener, false, true, true, false));
			history.Undo();
			context.Expect(!source->loop && listener->eligible && history.CanRedo(),
				"Undo must restore the source loop and listener eligibility values as one authored edit.");
			history.Redo();
			context.Expect(source->loop && !listener->eligible && !history.CanRedo(),
				"Redo must reapply the same source/listener values without creating another history entry.");

			history.Clear();
			objects.clear();
		});

		registry.Add("Editor.AudioAuthoring.ReferenceScenesStayDeterministic", [](TestContext& context) {
			const auto fixtureRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tests/Fixtures/Audio/Scenes";
			std::ifstream nonSpatialInput(fixtureRoot / "non-spatial.tescene");
			std::ifstream spatialInput(fixtureRoot / "spatial-listener-selection.tescene");
			const auto nonSpatial = nlohmann::json::parse(nonSpatialInput);
			const auto spatial = nlohmann::json::parse(spatialInput);
			context.Expect(nonSpatial.at("objects").size() == 1U && spatial.at("objects").size() == 3U
				&& spatial.at("objects")[0].at("uuid") < spatial.at("objects")[1].at("uuid"),
				"Installed spatial reference scenes must retain stable identities and listener tie-break order.");
		});

		registry.Add("Editor.AudioAuthoring.DuplicationAndRejectedBatchPreserveAuthoredState", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			queue.Clear();

			Object original = objects.createObject("Original Audio Source");
			auto* originalSource = objects.AddComponent<AudioSourceComponent>(original);
			originalSource->clipAssetId = TerraEngine::UUID{29U};
			originalSource->SetVolume(0.25F);
			originalSource->SetSpatialBlend(0.75F);
			const nlohmann::json originalJson = Serializer::SerializeObject(original);
			nlohmann::json duplicateJson = originalJson;
			duplicateJson["uuid"] = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
			Object duplicate = Serializer::DeserializeObject(duplicateJson);
			auto* duplicateSource = objects.GetComponent<AudioSourceComponent>(duplicate);
			context.Expect(duplicateSource != nullptr
				&& objects.getUUID(original) != objects.getUUID(duplicate)
				&& duplicateSource->clipAssetId == originalSource->clipAssetId
				&& duplicateSource->volume == originalSource->volume
				&& duplicateSource->spatialBlend == originalSource->spatialBlend,
				"Duplicating an audio-authored object must retain values while assigning a distinct object identity.");

			TerraEngine::Scene::TransactionalMutationBuilder rejectedBuilder(
				"audio-rejected-multi-edit", TerraEngine::Scene::TransactionOrigin::Editor, "Audio Inspector");
			rejectedBuilder.ObserveScene(objects)
				.ObserveObject(objects, objects.getUUID(original))
				.ObserveObject(objects, objects.getUUID(duplicate))
				.UpdateComponentProperty(objects.getUUID(original), "AudioSourceComponent", "volume", TerraEngine::Reflection::Variant{0.5F})
				// The second invalid value proves preflight rejects the whole edit, not
				// just the malformed component property after the first one was applied.
				.UpdateComponentProperty(objects.getUUID(duplicate), "AudioSourceComponent", "volume", TerraEngine::Reflection::Variant{std::string("invalid")});
			const TerraEngine::Scene::TransactionResult rejected =
				TerraEngine::Scene::CommitTransaction(objects, rejectedBuilder.Build());
			context.Expect(rejected.status != TerraEngine::Scene::TransactionStatus::Committed
				&& originalSource->volume == 0.25F && duplicateSource->volume == 0.25F,
				"A rejected audio multi-edit must roll back without partially changing either source.");

			queue.Clear();
			objects.clear();
		});
	}
}
