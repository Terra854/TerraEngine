// Native source-setting transactions exercise the actual protected journal.
#include "../TestHarness.h"
#include "Components/AudioSourceComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionCommitOutcome.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace TerraEngine::Tests {
	namespace {
		struct SourceTransactionFixture {
			ObjectManager& objects = ObjectManager::instance();
			Object first{}, second{};
			SourceTransactionFixture() {
				ComponentRegistry::RegisterAllComponents();
				objects.clear();
				first = objects.createObject("First transactional source");
				second = objects.createObject("Second transactional source");
				objects.AddComponent<AudioSourceComponent>(first)->volume = 0.75F;
				objects.AddComponent<AudioSourceComponent>(second)->volume = 0.5F;
				(void)objects.PublishSceneChangePacket();
			}
			~SourceTransactionFixture() {
				objects.SetOnModifiedCallback({});
				Scene::SceneMutationQueue::Instance().Clear();
				objects.clear();
			}
			Audio::AudioSourceAttachmentKey Key(Object object) {
				return {objects.getUUID(object), objects.GetSceneGeneration(), objects.getObjectLifetimeGeneration(object), objects.getObjectComponentGeneration(object)};
			}
			Scene::Transaction Make(std::vector<Scene::AudioSourceSettingsUpdate> updates) {
				return Scene::TransactionalMutationBuilder("typed-source-settings", Scene::TransactionOrigin::Test)
					.UpdateAudioSourceSettingsBatch(std::move(updates)).Build();
			}
		};
	}

	void RegisterAudioSourceSettingsTransactionTests(TestRegistry& registry) {
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsCallerOutcomeSurvivesBadAllocObserver", [](TestContext& context) {
			SourceTransactionFixture fixture;
			Scene::TransactionCommitOutcome outcome;
			fixture.objects.SetOnModifiedCallback([] { throw std::bad_alloc(); });
			bool escaped = false;
			bool adoptedCompanion = false;
			try { (void)Scene::CommitTransaction(fixture.objects, fixture.Make({{fixture.Key(fixture.first), 0.25F, {}}}), outcome); }
			catch (...) {
				escaped = true;
				// The real caller can adopt preallocated companion state even when
				// the diagnostic exception carrier cannot be relied upon.
				if (outcome.committed) adoptedCompanion = true;
			}
			context.Expect(escaped && outcome.committed && adoptedCompanion
				&& fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume == 0.25F,
				"A bad_alloc thrown by a postcommit observer must preserve a no-allocation committed witness for its caller.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsBudgetCountsBothFields", [](TestContext& context) {
			SourceTransactionFixture fixture;
			auto transaction = fixture.Make({{fixture.Key(fixture.first), 0.25F, 1.5F}});
			transaction.budget.maxWriteCount = 1U;
			const auto result = Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == Scene::TransactionStatus::Rejected && result.rejectedWriteCount == 2U,
				"One typed entry containing two requested fields must consume two writes and reject atomically.");
			context.ExpectEqual(0.75F, fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume, "Budget rejection cannot publish source values.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsValidateEveryAttachmentVersion", [](TestContext& context) {
			SourceTransactionFixture fixture;
			for (unsigned version = 0U; version < 3U; ++version) {
				auto key = fixture.Key(fixture.first);
				if (version == 0U) ++key.sceneGeneration;
				if (version == 1U) ++key.objectGeneration;
				if (version == 2U) ++key.componentGeneration;
				const auto result = Scene::CommitTransaction(fixture.objects, fixture.Make({{key, 0.25F, {}}}));
				context.Expect(result.status == Scene::TransactionStatus::Conflicted,
					"Scene, object lifetime and component attachment versions must each reject stale packets.");
			}
			context.ExpectEqual(0.75F, fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume, "Stale attachments cannot alter a current source.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsRejectEmptyNonfiniteAndDuplicateEntries", [](TestContext& context) {
			SourceTransactionFixture fixture;
			const auto key = fixture.Key(fixture.first);
			const std::vector<std::vector<Scene::AudioSourceSettingsUpdate>> invalid{
				{}, {{key, {}, {}}}, {{key, std::numeric_limits<float>::quiet_NaN(), {}}},
				{{key, {}, std::numeric_limits<float>::infinity()}}, {{key, 0.25F, {}}, {key, {}, 2.0F}}};
			for (const auto& entries : invalid)
				context.Expect(Scene::CommitTransaction(fixture.objects, fixture.Make(entries)).status == Scene::TransactionStatus::Rejected,
					"Malformed typed source packets must reject before any field write.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsRollbackRetainsValuesAndAllRevisions", [](TestContext& context) {
			SourceTransactionFixture fixture;
			const auto components = fixture.objects.GetComponentMutationRevision();
			const auto values = fixture.objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName);
			const auto hierarchy = fixture.objects.GetHierarchyRevision();
			const auto spatial = fixture.objects.GetSpatialRevision();
			const auto packet = fixture.objects.LoadPublishedSceneChangePacket().PublicationRevision();
			auto transaction = fixture.Make({{fixture.Key(fixture.first), 0.25F, 2.0F}});
			auto second = fixture.Make({{fixture.Key(fixture.second), 0.1F, {}}});
			transaction.writeSet.operations.push_back(std::move(second.writeSet.operations.front()));
			transaction.injectedFailureOperationIndex = 1U;
			const auto result = Scene::CommitTransaction(fixture.objects, transaction);
			const auto* source = fixture.objects.GetComponent<AudioSourceComponent>(fixture.first);
			context.Expect(result.status == Scene::TransactionStatus::Rejected && result.inverseDeltas.empty()
				&& source->volume == 0.75F && source->pitch == 1.0F, "A fault after the first changed source must restore its actual old component payload.");
			context.Expect(components == fixture.objects.GetComponentMutationRevision()
				&& values == fixture.objects.GetComponentValueMutationRevision(AudioSourceComponent::kComponentName)
				&& hierarchy == fixture.objects.GetHierarchyRevision() && spatial == fixture.objects.GetSpatialRevision()
				&& packet == fixture.objects.LoadPublishedSceneChangePacket().PublicationRevision(), "Aborted audio publication must restore every observed revision.");
			context.Expect(fixture.objects.PublishSceneChangePacket().Empty(), "The next packet cannot expose rolled-back audio changes.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsExactInverseRestoresAuthoredRange", [](TestContext& context) {
			SourceTransactionFixture fixture;
			auto* source = fixture.objects.GetComponent<AudioSourceComponent>(fixture.first);
			source->volume = 3.5F;
			source->pitch = 0.5F;
			const auto result = Scene::CommitTransaction(fixture.objects, fixture.Make({{fixture.Key(fixture.first), 7.0F, 0.001F}}));
			context.Expect((result.status == Scene::TransactionStatus::Committed) && source->volume == 1.0F && source->pitch == 0.01F,
				"Forward publication retains established runtime unit volume and .01 pitch normalization.");
			context.Expect(Scene::ApplyTransactionInverseDeltas(fixture.objects, result.inverseDeltas)
				&& source->volume == 3.5F && source->pitch == 0.5F, "Typed native inverse must restore exact authored values above runtime gain range.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsBecomeVisibleOnlyAtProtectedFlush", [](TestContext& context) {
			SourceTransactionFixture fixture;
			auto& queue = Scene::SceneMutationQueue::Instance();
			queue.Clear();
			queue.EnqueueTransaction(fixture.Make({{fixture.Key(fixture.first), 0.25F, {}}}));
			context.ExpectEqual(0.75F, fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume, "Enqueueing typed settings must preserve live values.");
			queue.Apply(fixture.objects, "audio-settings-test-boundary");
			context.ExpectEqual(0.25F, fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume, "The declared protected flush publishes source values.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsValueWritesPreserveAttachmentIdentity", [](TestContext& context) {
			SourceTransactionFixture fixture;
			const auto key = fixture.Key(fixture.first);
			const auto binding = fixture.objects.getObjectBindingGeneration(fixture.first);
			const auto result = Scene::CommitTransaction(fixture.objects, fixture.Make({{key, 0.25F, {}}}));
			context.Expect((result.status == Scene::TransactionStatus::Committed) && key == fixture.Key(fixture.first)
				&& binding == fixture.objects.getObjectBindingGeneration(fixture.first), "Source value writes cannot replace component or managed binding identity.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsObserverFailureReportsCommittedState", [](TestContext& context) {
			SourceTransactionFixture fixture;
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("audio postcommit observer"); });
			bool committedFailure = false;
			try { (void)Scene::CommitTransaction(fixture.objects, fixture.Make({{fixture.Key(fixture.first), 0.25F, {}}})); }
			catch (const Scene::TransactionPublicationFailure& failure) { committedFailure = (failure.committedResult.status == Scene::TransactionStatus::Committed); }
			context.Expect(committedFailure && fixture.objects.GetComponent<AudioSourceComponent>(fixture.first)->volume == 0.25F,
				"A failing postcommit observer cannot turn committed values into a rejected transaction.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.AudioSettingsNativeKindRejectsExternalJson", [](TestContext& context) {
			std::string diagnostic;
			const auto transaction = Scene::LowerSpatialTransaction({{"format", Scene::kTerraSpatialTransactionFormat}, {"id", "external-audio"},
				{"operations", {{{"kind", "updateAudioSourceSettingsBatch"}, {"target", TerraEngine::ToHexString(TerraEngine::UUID{1U})}}}}}, &diagnostic);
			context.Expect(!transaction && diagnostic.find("native-only") != std::string::npos,
				"External JSON must not manufacture a native audio hot-path packet or its exact inverse flag.");
		});
	}
}
