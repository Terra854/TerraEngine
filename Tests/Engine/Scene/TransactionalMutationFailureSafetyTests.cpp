#include "TestHarness.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/Serializer.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/SceneMutationQueue.h"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace TerraEngine::Tests {
	namespace {
		// A supported custom authored setter can fail while preparing its detached
		// candidate. It owns only its scalar state and declares no external effects.
		struct ThrowingAuthoredComponent : Component<ThrowingAuthoredComponent> {
			static constexpr std::string_view kComponentName = "ThrowingAuthoredComponent";
			static constexpr bool kTransactionalAuthoringSafe = true;
			int value = 0;
			int GetValue() const noexcept { return value; }
			void SetValue(int next) {
				if (next == 13) throw std::runtime_error("Controlled detached setter failure.");
				value = next;
			}
			static void reflect(entt::meta_factory<ThrowingAuthoredComponent>& factory) {
				TerraEngine::Reflection::RegisterAccessorProperty<&ThrowingAuthoredComponent::SetValue, &ThrowingAuthoredComponent::GetValue>(factory, "value");
			}
		};
		// Capture authored values and every public publication revision before
		// entering the production commit path. Entity count also catches a leaked
		// entity whose malformed component data removed its default Transform.
		struct ObservableScene {
			nlohmann::json authored;
			std::size_t entityCount;
			std::uint64_t spatial;
			std::uint64_t hierarchy;
			std::uint64_t membership;
			std::uint64_t components;
			std::uint64_t scene;
			std::uint64_t publication;
		};

		ObservableScene Capture(ObjectManager& objects) {
			nlohmann::json authored = nlohmann::json::object();
			for (const Object& root : objects.getRootObjects()) {
				authored[TerraEngine::ToHexString(objects.getUUID(root))] = Serializer::SerializeObject(root.EntityId());
			}
			return {std::move(authored), objects.getRegistry().storage<entt::entity>().free_list(),
				objects.GetSpatialRevision(), objects.GetHierarchyRevision(),
				objects.GetRuntimeActiveMembershipRevision(), objects.GetComponentMutationRevision(),
				objects.GetSceneGeneration(), objects.LoadPublishedSceneChangePacket().PublicationRevision()};
		}

		void ExpectUnchanged(TestContext& context, ObjectManager& objects, const ObservableScene& before, bool drainEmpty = true) {
			const ObservableScene after = Capture(objects);
			context.Expect(before.authored == after.authored,
				"Rejected restoration must retain exact authored metadata, components, identities and hierarchy.");
			context.ExpectEqual(before.entityCount, after.entityCount, "Rejected restoration must not leak entities.");
			context.ExpectEqual(before.spatial, after.spatial, "Rejected restoration must not advance spatial visibility.");
			context.ExpectEqual(before.hierarchy, after.hierarchy, "Rejected restoration must not advance hierarchy visibility.");
			context.ExpectEqual(before.membership, after.membership, "Rejected restoration must not advance runtime membership visibility.");
			context.ExpectEqual(before.components, after.components, "Rejected restoration must not advance component visibility.");
			context.ExpectEqual(before.scene, after.scene, "Rejected restoration must retain the scene lifetime.");
			context.ExpectEqual(before.publication, after.publication, "Commit must not publish an aborted scene packet.");
			// Drain only after checking the previous packet. A new protected packet
			// may advance its sequence, but must contain no aborted change entries.
			context.Expect(!drainEmpty || objects.PublishSceneChangePacket().Empty(),
				"The next protected publication must not expose any aborted mutation.");
		}

		// A valid snapshot comes from the serializer rather than a test-only
		// restore implementation. The live baseline also contains a child, so
		// rollback must preserve existing hierarchy, not just one root's name.
		void RejectMalformedRestore(TestContext& context,
			const std::function<void(nlohmann::json&)>& corrupt) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			Object existingChild = fixture.objects.createObject("Existing Child");
			fixture.objects.setParent(existingChild, fixture.root);
			Object source = fixture.objects.createObject("Restored Root");
			Object sourceChild = fixture.objects.createObject("Restored Child");
			fixture.objects.setParent(sourceChild, source);
			const TerraEngine::UUID restoredId = fixture.objects.getUUID(source);
			nlohmann::json snapshot = Serializer::SerializeObject(source.EntityId());
			fixture.objects.removeObject(source);
			corrupt(snapshot);
			(void)fixture.objects.PublishSceneChangePacket();
			const ObservableScene before = Capture(fixture.objects);

			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder(
				"restore-failure", TerraEngine::Scene::TransactionOrigin::Test, "failure-safety")
				.ObserveScene(fixture.objects).ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "Earlier Rename Must Be Reverted").Build();
			TerraEngine::Scene::TransactionWriteOperation restore;
			restore.kind = TerraEngine::Scene::TransactionWriteKind::RestoreObjectSnapshot;
			restore.target = TerraEngine::Scene::ExistingObject(restoredId);
			restore.serializedObject = std::move(snapshot);
			transaction.writeSet.operations.push_back(std::move(restore));

			std::optional<TerraEngine::Scene::TransactionResult> result;
			std::exception_ptr escaped;
			try {
				result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			} catch (...) {
				// Inspect state even when the old implementation lets decode escape.
				// The expected fixed contract is a truthful rejection, not an exception.
				escaped = std::current_exception();
			}
			ExpectUnchanged(context, fixture.objects, before);
			context.Expect(!escaped, "Malformed restoration must terminate as rejection without escaping commit.");
			context.Expect(result && result->status == TerraEngine::Scene::TransactionStatus::Rejected,
				"Malformed restoration must reject the complete transaction.");
			if (!result) return;
			context.ExpectEqual(transaction.id, result->transactionId, "Rejection must preserve transaction identity.");
			context.ExpectEqual(std::size_t{0}, result->appliedWriteCount, "Rejection must report zero applied writes.");
			context.ExpectEqual(std::size_t{2}, result->rejectedWriteCount, "Rejection must count both rejected operations.");
			context.Expect(result->inverseDeltas.empty() && result->createdObjects.empty()
				&& result->acceptedAnimationPublications.empty(), "Rejection must publish no inverse/history or derived receipts.");
			context.Expect(!result->conflicts.empty(), "Malformed restoration must report its failure reason.");
			if (!result->conflicts.empty()) {
				context.ExpectEqual(std::size_t{1}, result->conflicts.front().operationIndex,
					"Failure diagnostics must identify the restore operation after the earlier rename.");
			}
		}
	}

	void RegisterTransactionalMutationFailureSafetyTests(TestRegistry& registry) {
		registry.Add("Engine.SceneMutationFailureSafety.RestoreNodeBudgetRejectsBeforeAnyWrite", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			const Object source = fixture.objects.createObject("Budget Root");
			const Object child = fixture.objects.createObject("Budget Child");
			fixture.objects.setParent(child, source);
			const auto id = fixture.objects.getUUID(source);
			const auto snapshot = Serializer::SerializeObject(source.EntityId());
			fixture.objects.removeObject(source); (void)fixture.objects.PublishSceneChangePacket();
			const auto before = Capture(fixture.objects);
			TerraEngine::Scene::Transaction transaction;
			transaction.id = "bounded-restore"; transaction.origin = TerraEngine::Scene::TransactionOrigin::Test;
			transaction.budget.maxWriteCount = 1U;
			TerraEngine::Scene::TransactionWriteOperation restore;
			restore.kind = TerraEngine::Scene::TransactionWriteKind::RestoreObjectSnapshot;
			restore.target = TerraEngine::Scene::ExistingObject(id); restore.serializedObject = snapshot;
			transaction.writeSet.operations.push_back(std::move(restore));
			context.Expect(TerraEngine::Scene::CommitTransaction(fixture.objects, transaction).status == TerraEngine::Scene::TransactionStatus::Rejected,
				"A restore graph must respect its node budget before constructing any object.");
			ExpectUnchanged(context, fixture.objects, before);
		});
		registry.Add("Engine.SceneMutationFailureSafety.RestoredObjectsSupportOrderedHierarchyAndSpatialWrites", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			const Object source = fixture.objects.createObject("Ordered Restore");
			const Object child = fixture.objects.createObject("Ordered Child");
			fixture.objects.setParent(child, source);
			const auto id = fixture.objects.getUUID(source); const auto childId = fixture.objects.getUUID(child);
			const auto childPose = *fixture.objects.GetCanonicalWorldPose(child);
			const auto snapshot = Serializer::SerializeObject(source.EntityId()); fixture.objects.removeObject(source);
			auto transaction = TerraEngine::Scene::TransactionalMutationBuilder("ordered-restore", TerraEngine::Scene::TransactionOrigin::Test)
				.CreateObject(1, "Earlier Local Parent").Build();
			TerraEngine::Scene::TransactionWriteOperation restore;
			restore.kind = TerraEngine::Scene::TransactionWriteKind::RestoreObjectSnapshot;
			restore.target = TerraEngine::Scene::ExistingObject(id); restore.parent = TerraEngine::Scene::LocalObject(1);
			restore.serializedObject = snapshot; transaction.writeSet.operations.push_back(restore);
			TerraEngine::Scene::TransactionWriteOperation reparent;
			reparent.kind = TerraEngine::Scene::TransactionWriteKind::ReparentObject;
			reparent.target = TerraEngine::Scene::ExistingObject(fixture.rootId); reparent.parent = TerraEngine::Scene::ExistingObject(childId);
			transaction.writeSet.operations.push_back(reparent);
			TerraEngine::Scene::TransactionWriteOperation spatial;
			spatial.kind = TerraEngine::Scene::TransactionWriteKind::UpdateTransformBatch;
			spatial.transformUpdates.push_back({childId, childPose}); transaction.writeSet.operations.push_back(spatial);
			const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed,
				"Restored stable IDs must be usable by later hierarchy and typed spatial operations.");
			if (result.status != TerraEngine::Scene::TransactionStatus::Committed) return;
			context.Expect(fixture.objects.getUUID(*fixture.objects.getParent(fixture.root)) == childId,
				"Reparent must resolve the earlier restored child.");
			context.Expect(fixture.objects.getUUID(*fixture.objects.getParent(*fixture.objects.getObject(id))) == result.createdObjects.at(1),
				"Restore must resolve the earlier transaction-local parent.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.DeletedDescendantCannotBeUsedLater", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			const Object child = fixture.objects.createObject("Deleted Child"); fixture.objects.setParent(child, fixture.root);
			const auto childId = fixture.objects.getUUID(child); (void)fixture.objects.PublishSceneChangePacket();
			const auto before = Capture(fixture.objects);
			const auto transaction = TerraEngine::Scene::TransactionalMutationBuilder("deleted-descendant", TerraEngine::Scene::TransactionOrigin::Test)
				.DeleteObject(fixture.rootId).SetName(childId, "Must Not Survive").Build();
			context.Expect(TerraEngine::Scene::CommitTransaction(fixture.objects, transaction).status == TerraEngine::Scene::TransactionStatus::Conflicted,
				"Deferred live component storage cannot make a deleted descendant a valid ordered target.");
			ExpectUnchanged(context, fixture.objects, before);
		});
		registry.Add("Engine.SceneMutationFailureSafety.FatalObserverPreservesCommittedHistoryAndCancelsRemainder", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::SceneMutationQueue queue;
			auto first = TerraEngine::Scene::TransactionalMutationBuilder("committed-before-observer", TerraEngine::Scene::TransactionOrigin::Editor)
				.SetName(fixture.rootId, "Committed Name").Build(); first.undoGroupName = "Committed Edit";
			auto later = TerraEngine::Scene::TransactionalMutationBuilder("unstarted-after-observer", TerraEngine::Scene::TransactionOrigin::Editor)
				.SetTag(fixture.rootId, "Must Not Apply").Build(); later.undoGroupName = "Canceled Edit";
			queue.EnqueueTransaction(first); queue.EnqueueTransaction(later);
			struct ResetObserver { ObjectManager& objects; ~ResetObserver() { objects.SetOnModifiedCallback({}); } } reset{fixture.objects};
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Controlled postcommit observer failure."); });
			bool fatal = false;
			try { (void)queue.DrainTransactions(fixture.objects); }
			catch (const TerraEngine::Scene::TransactionPublicationFailure& failure) {
				fatal = failure.committedResult.status == TerraEngine::Scene::TransactionStatus::Committed;
			}
			context.Expect(fatal, "Observer failure must carry truthful committed state as a fatal marker.");
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Committed Name" && fixture.objects.getObjectTag(fixture.root) == "Untagged",
				"Committed values remain visible and unstarted values remain unapplied.");
			const auto completions = queue.DrainCompletedEditorTransactions();
			context.Expect(completions.size() == 2U && completions[0].result.status == TerraEngine::Scene::TransactionStatus::Committed
				&& completions[1].result.status == TerraEngine::Scene::TransactionStatus::Canceled,
				"History receives one committed completion and one canceled completion.");
			context.Expect(queue.Empty() && queue.DrainCompletedEditorTransactions().empty(), "Fatal cohort must be consumed exactly once.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.FatalGroupRetainsEveryTerminalChunkReceipt", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture(); TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::TransactionPublicationGroup group;
			group.groupId = "committed-group";
			group.chunks.push_back(TerraEngine::Scene::TransactionalMutationBuilder("chunk-a", TerraEngine::Scene::TransactionOrigin::Test)
				.SetName(fixture.rootId, "Committed Group Name").Build());
			group.chunks.push_back(TerraEngine::Scene::TransactionalMutationBuilder("chunk-b", TerraEngine::Scene::TransactionOrigin::Test)
				.SetTag(fixture.rootId, "Committed Group Tag").Build());
			TerraEngine::Scene::TransactionPublicationGroup later;
			later.groupId = "canceled-group";
			later.chunks.push_back(TerraEngine::Scene::TransactionalMutationBuilder("chunk-c", TerraEngine::Scene::TransactionOrigin::Test)
				.SetName(fixture.rootId, "Must Not Apply").Build());
			queue.EnqueuePublicationGroup(group); queue.EnqueuePublicationGroup(later);
			struct ResetObserver { ObjectManager& objects; ~ResetObserver() { objects.SetOnModifiedCallback({}); } } reset{fixture.objects};
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Controlled group observer failure."); });
			bool fatal = false; try { (void)queue.DrainPublicationGroups(fixture.objects); }
			catch (const TerraEngine::Scene::TransactionPublicationFailure&) { fatal = true; }
			const auto receipts = queue.TakeCompletedPublicationGroupResults();
			context.Expect(fatal && receipts.size() == 2U && receipts[0].status == TerraEngine::Scene::TransactionStatus::Committed
				&& receipts[1].status == TerraEngine::Scene::TransactionStatus::Canceled,
				"Fatal group publication retains committed and canceled group truth.");
			context.Expect(receipts.size() == 2U && receipts[0].chunkReceipts.size() == 2U
				&& receipts[0].chunkReceipts[0].status == TerraEngine::Scene::TransactionStatus::Committed,
				"Every committed preparation chunk receives a terminal receipt.");
			context.Expect(queue.Empty() && queue.TakeCompletedPublicationGroupResults().empty(), "Group receipts are taken exactly once.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.FatalSpatialRequestCannotReplayCommittedPose", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture(); TerraEngine::Scene::SceneMutationQueue queue;
			auto pose = *fixture.objects.GetCanonicalWorldPose(fixture.root); pose.position.offsetMetres.x = 5.0;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "spatial-fatal-request"; request.operation = TerraEngine::Scene::SceneMutationOperation::SetWorldTransform;
			request.targetObjectId = fixture.rootId;
			request.typedSpatialTransaction = TerraEngine::Scene::TransactionalMutationBuilder("committed-spatial", TerraEngine::Scene::TransactionOrigin::Test)
				.SetWorldTransform(fixture.objects, fixture.rootId, pose).Build();
			queue.Enqueue(request);
			struct ResetObserver { ObjectManager& objects; ~ResetObserver() { objects.SetOnModifiedCallback({}); } } reset{fixture.objects};
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Controlled spatial observer failure."); });
			bool fatal = false; try { (void)queue.Apply(fixture.objects); }
			catch (const TerraEngine::Scene::TransactionPublicationFailure&) { fatal = true; }
			context.Expect(fatal && fixture.objects.GetCanonicalWorldPose(fixture.root)->position.offsetMetres.x == 5.0,
				"Fatal spatial callback retains committed canonical pose.");
			context.Expect(queue.Empty(), "Committed generic spatial request is consumed before the fatal marker escapes.");
			const auto diagnostics = queue.GetTransactionDiagnostics();
			context.Expect(!diagnostics.recentResults.empty() && diagnostics.recentResults.back().status == TerraEngine::Scene::TransactionStatus::Committed,
				"Spatial fatal diagnostics retain committed truth.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.ThrowingDetachedSetterRestoresEarlierOperations", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			fixture.objects.AddComponent<ThrowingAuthoredComponent>(fixture.root);
			(void)fixture.objects.PublishSceneChangePacket();
			const auto before = Capture(fixture.objects);
			const auto transaction = TerraEngine::Scene::TransactionalMutationBuilder("detached-setter-failure", TerraEngine::Scene::TransactionOrigin::Test)
				.SetName(fixture.rootId, "Earlier Setter Rename")
				.UpdateComponentProperty(fixture.rootId, std::string(ThrowingAuthoredComponent::kComponentName), "value", TerraEngine::Reflection::Variant{13}).Build();
			const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected, "Supported throwing setter must reject atomically.");
			ExpectUnchanged(context, fixture.objects, before);
		});
		registry.Add("Engine.SceneMutationFailureSafety.ValidRestoreInverseAndReplayPreserveAuthoredGraph", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			Object source = fixture.objects.createObject("Valid Restore");
			Object child = fixture.objects.createObject("Valid Restore Child");
			fixture.objects.setParent(child, source);
			const auto id = fixture.objects.getUUID(source);
			const auto childId = fixture.objects.getUUID(child);
			const auto snapshot = Serializer::SerializeObject(source.EntityId());
			fixture.objects.removeObject(source);
			TerraEngine::Scene::Transaction transaction;
			transaction.id = "valid-restore"; transaction.origin = TerraEngine::Scene::TransactionOrigin::Test;
			TerraEngine::Scene::TransactionWriteOperation restore;
			restore.kind = TerraEngine::Scene::TransactionWriteKind::RestoreObjectSnapshot;
			restore.target = TerraEngine::Scene::ExistingObject(id); restore.serializedObject = snapshot;
			transaction.writeSet.operations.push_back(restore);
			const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed, "Valid graph must restore successfully.");
			context.Expect(fixture.objects.getObject(id) && fixture.objects.getObject(childId), "Valid restore must preserve every stable identity.");
			context.Expect(Serializer::SerializeObject(fixture.objects.getObject(id)->EntityId()) == snapshot, "Valid restore must preserve authored graph values.");
			context.Expect(TerraEngine::Scene::ApplyTransactionInverseDeltas(fixture.objects, result.inverseDeltas), "Prepared inverse replay must remove the restored graph.");
			context.Expect(!fixture.objects.getObject(id) && !fixture.objects.getObject(childId), "Inverse replay must remove the complete branch.");
			context.Expect(TerraEngine::Scene::CommitTransaction(fixture.objects, transaction).status == TerraEngine::Scene::TransactionStatus::Committed,
				"A fresh replay of the valid restore must still succeed.");
		});
		for (const ObjectCreationStage stage : {ObjectCreationStage::Allocated, ObjectCreationStage::DefaultsConstructed}) {
			registry.Add(stage == ObjectCreationStage::Allocated
				? "Engine.SceneMutationFailureSafety.FactoryAllocationOwnershipSurvivesException"
				: "Engine.SceneMutationFailureSafety.FactoryConstructionOwnershipSurvivesException", [stage](TestContext& context) {
				TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
				(void)fixture.objects.PublishSceneChangePacket();
				const ObservableScene before = Capture(fixture.objects);
				struct ClearHook {
					ObjectManager& objects;
					~ClearHook() { objects.SetCreationFailureHookForTesting({}); }
				} clear{fixture.objects};
				fixture.objects.SetCreationFailureHookForTesting([stage](ObjectCreationStage observed) {
					if (stage == observed) throw std::runtime_error("Controlled object factory failure.");
				});
				const auto transaction = TerraEngine::Scene::TransactionalMutationBuilder(
					"factory-failure", TerraEngine::Scene::TransactionOrigin::Test)
					.SetName(fixture.rootId, "Earlier Factory Rename").CreateObject(1, "Failing Object").Build();
				const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
				context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected,
					"A factory exception after a prior write must become atomic rejection.");
				ExpectUnchanged(context, fixture.objects, before);
				context.Expect(result.inverseDeltas.empty() && result.createdObjects.empty(), "Factory failure must publish no history or created identities.");
			});
		}
		registry.Add("Engine.SceneMutationFailureSafety.AbortPreservesEarlierPendingPublication", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			(void)fixture.objects.PublishSceneChangePacket();
			fixture.objects.setObjectName(fixture.root, "Earlier Pending Name");
			const Object earlierAdded = fixture.objects.createObject("Earlier Pending Object");
			const auto addedId = fixture.objects.getUUID(earlierAdded);
			const ObservableScene before = Capture(fixture.objects);
			auto transaction = TerraEngine::Scene::TransactionalMutationBuilder("later-rejection", TerraEngine::Scene::TransactionOrigin::Test)
				.SetName(fixture.rootId, "Aborted Name").SetActive(fixture.rootId, false).Build();
			transaction.injectedFailureOperationIndex = 1;
			const auto result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);
			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected, "Controlled later failure must reject.");
			ExpectUnchanged(context, fixture.objects, before, false);
			const auto packet = fixture.objects.PublishSceneChangePacket();
			context.Expect(packet.Added().size() == 1 && packet.Added().front().objectId == addedId,
				"Abort must retain an unrelated earlier pending creation.");
			context.Expect(packet.MetadataChanged().size() == 1 && packet.MetadataChanged().front().objectId == fixture.rootId,
				"Abort must retain the earlier pending metadata change without adding aborted entries.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.MalformedRootMetadataRejectsEarlierRename", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) { snapshot["active"] = "invalid boolean"; });
		});
		registry.Add("Engine.SceneMutationFailureSafety.MalformedChildRejectsWholeRestoreGraph", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) { snapshot["children"][0]["tag"] = nlohmann::json::array(); });
		});
		registry.Add("Engine.SceneMutationFailureSafety.MalformedCanonicalComponentRejectsWholeRestoreGraph", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) {
				snapshot["components"]["Transform"]["WorldPose"]["rotation"] = {1.0, 0.0};
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.UnknownComponentRejectsWholeRestoreGraph", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) {
				snapshot["components"]["UnregisteredFailureFixture"] = nlohmann::json::object();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.MalformedReflectedComponentRejectsWholeRestoreGraph", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) {
				snapshot["components"]["AudioSourceComponent"] = {{"loop", "invalid boolean"}};
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.LaterChildDecodeFailureLeavesNoValidPrefix", [](TestContext& context) {
			RejectMalformedRestore(context, [](nlohmann::json& snapshot) {
				// Preserve a complete valid child before a later child's fallible
				// metadata decode, exercising recursive partial graph construction.
				nlohmann::json later = snapshot["children"][0];
				later["uuid"] = TerraEngine::ToHexString(TerraEngine::GenerateUUID());
				later["layer"] = nlohmann::json::object();
				snapshot["children"].push_back(std::move(later));
			});
		});
	}
}
