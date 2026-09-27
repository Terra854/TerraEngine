#include "TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/Serializer.h"
#include "ThreadPool/ThreadPool.h"

#include <string>
#include <utility>

namespace TerraEngine::Tests {
	void RegisterWorldSpatialTransactionTests(TestRegistry& registry)
	{
		registry.Add("Engine.WorldSpatialTransaction.ChildBatchSurvivesWorkerPropagation", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			// Own workers only when the surrounding harness has not started them.
			// RAII also shuts them down if an assertion throws during propagation.
			struct WorkerScope {
				bool owns = ThreadPool::instance().getThreadCount() == 0;
				WorkerScope() { if (owns) ThreadPool::instance().initialize(2); }
				~WorkerScope() { if (owns) ThreadPool::instance().shutdown(); }
			} workers;
			auto fixture = MakeTransactionalMutationFixture();
			std::vector<TransactionTransformUpdate> updates;
			for (int index = 0; index < 512; ++index)
			{
				const Object visual = fixture.objects.createObject("Visual");
				context.Expect(fixture.objects.setParent(visual, fixture.root, ReparentMode::KeepLocal), "Stress visual must attach.");
				WorldPose pose{};
				pose.position.offsetMetres.x = 10 + index * 0.5;
				pose.scale = glm::vec3(0.3f);
				updates.push_back({fixture.objects.getUUID(visual), pose});
			}
			WorldPose parentPose{};
			parentPose.position.offsetMetres.x = 10;
			parentPose.scale = glm::vec3(2);
			updates.push_back({fixture.rootId, parentPose});
			TransactionalMutationBuilder builder("worker-propagation", TransactionOrigin::Test);
			builder.UpdateTransformBatch(fixture.objects, updates);
			context.Expect(CommitTransaction(fixture.objects, builder.Build()).status == TransactionStatus::Committed, "Child-first stress batch must publish at the protected boundary.");
			for (int frame = 0; frame < 4; ++frame)
			{
				auto graph = BuildSceneGraphFromObjectManager(fixture.objects, false);
				const auto propagated = PropagateWorldTransforms(graph, BuildDirtyTransformState(graph));
				context.Expect(propagated.valid && propagated.workerJobCount > 0, "Stress check must actually execute worker hierarchy jobs.");
				for (const auto& update : updates)
				{
					const auto& actual = propagated.worldTransformsByObject.at(update.objectId);
					context.Expect(actual.position == update.pose.position && actual.scale == update.pose.scale, "Workers must retain every accepted world pose across repeated frames.");
				}
			}
		});

		registry.Add("Engine.WorldSpatialTransaction.ChildScaleVisibleOnlyAfterFlush", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			const Object visual = fixture.objects.createObject("Visual");
			context.Expect(fixture.objects.setParent(visual, fixture.root, ReparentMode::KeepLocal), "Visual fixture must attach.");
			const auto visualId = fixture.objects.getUUID(visual);
			auto pose = *fixture.objects.GetCanonicalWorldPose(visual);
			pose.scale = glm::vec3(0.321496f);
			TerraEngine::Scene::TransactionalMutationBuilder builder("scale-at-boundary", TerraEngine::Scene::TransactionOrigin::Test);
			builder.SetWorldTransform(fixture.objects, visualId, pose);
			TerraEngine::Scene::TransactionPublicationGroup group;
			group.groupId = "visual-scale";
			group.chunks.push_back(builder.Build());
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.EnqueuePublicationGroup(std::move(group));
			const auto before = TerraEngine::Scene::BuildSceneGraphFromObjectManager(fixture.objects);
			context.Expect(before.objects.at(visualId).worldPose.scale == glm::vec3(1), "Submission must not expose either part of the pose early.");
			context.Expect(queue.Apply(fixture.objects, "Frame End").applied, "Existing protected boundary must publish the scale.");
			const auto after = TerraEngine::Scene::BuildSceneGraphFromObjectManager(fixture.objects);
			context.Expect(after.objects.at(visualId).worldPose.scale == pose.scale, "Rendered size must match the accepted world-space edit.");
			const auto saved = Serializer::SerializeObject(fixture.root);
			fixture.objects.clear();
			(void)Serializer::DeserializeObject(saved);
			const auto reloaded = TerraEngine::Scene::BuildSceneGraphFromObjectManager(fixture.objects);
			context.Expect(reloaded.objects.contains(visualId) && reloaded.objects.at(visualId).worldPose.scale == pose.scale, "Save/load must retain the child scale consumed by rendering.");
		});

		registry.Add("Engine.WorldSpatialTransaction.AncestorEditConflictsBeforeChildPublication", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture();
			const Object middle = fixture.objects.createObject("Middle");
			const Object child = fixture.objects.createObject("Child");
			context.Expect(fixture.objects.setParent(middle, fixture.root, ReparentMode::KeepLocal) && fixture.objects.setParent(child, middle, ReparentMode::KeepLocal), "Nested fixture must attach.");
			const auto before = *fixture.objects.GetComponent<Transform>(child);
			auto desired = before.worldPose;
			desired.scale = glm::vec3(0.3f);
			TransactionalMutationBuilder builder("stale-ancestor", TransactionOrigin::Test);
			builder.SetWorldTransform(fixture.objects, fixture.objects.getUUID(child), desired);
			auto moved = *fixture.objects.GetCanonicalWorldPose(fixture.root);
			moved.scale = glm::vec3(2);
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.root, moved) == WorldCoordinateError::None, "Change a non-direct ancestor after preparation.");
			context.Expect(CommitTransaction(fixture.objects, builder.Build()).status == TransactionStatus::Conflicted, "A changed ancestor invalidates world-to-local preparation.");
			const auto* actual = fixture.objects.GetComponent<Transform>(child);
			context.Expect(actual->worldPose.scale == before.worldPose.scale && actual->parentRelativePose.scale == before.parentRelativePose.scale, "Conflicted preparation must not change the child.");
		});

		registry.Add("Engine.WorldSpatialTransaction.ChildFirstBatchAndHistoryPreserveBothPoses", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture();
			const Object visual = fixture.objects.createObject("Visual");
			context.Expect(fixture.objects.setParent(visual, fixture.root, ReparentMode::KeepLocal), "Visual fixture must attach.");
			const auto visualId = fixture.objects.getUUID(visual);
			// Deliberately leave the child's stored world cache behind its moved
			// parent. History must preserve this exact pair, not derive a new local.
			WorldPose parentPose{};
			parentPose.position.offsetMetres.x = 10;
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.root, parentPose) == WorldCoordinateError::None, "Move the fixture ancestor.");
			const Transform before = *fixture.objects.GetComponent<Transform>(visual);
			parentPose.position.offsetMetres.x = 20;
			parentPose.scale = glm::vec3(2);
			WorldPose visualPose = parentPose;
			visualPose.position.offsetMetres.x = 22;
			visualPose.scale = glm::vec3(0.3f);
			TransactionalMutationBuilder builder("child-first-batch", TransactionOrigin::Test);
			builder.UpdateTransformBatch(fixture.objects, {{visualId, visualPose}, {fixture.rootId, parentPose}});
			const auto result = CommitTransaction(fixture.objects, builder.Build());
			context.Expect(result.status == TransactionStatus::Committed, "A child-first worker packet must commit parent-first internally.");
			const auto graph = BuildSceneGraphFromObjectManager(fixture.objects);
			const auto& actual = graph.objects.at(visualId).worldPose;
			context.Expect(actual.position == visualPose.position && actual.scale == visualPose.scale, "Batch targets must reach their requested world poses regardless of packet order.");
			// Editor history copies inverse operations and submits a new transaction.
			Transaction undo;
			undo.id = "undo-child-first-batch";
			undo.origin = TransactionOrigin::Editor;
			for (auto inverse = result.inverseDeltas.rbegin(); inverse != result.inverseDeltas.rend(); ++inverse) undo.writeSet.operations.push_back(inverse->operation);
			const auto undone = CommitTransaction(fixture.objects, undo);
			context.Expect(undone.status == TransactionStatus::Committed, "The same typed inverse data must support committed history replay.");
			const auto* restored = fixture.objects.GetComponent<Transform>(visual);
			context.Expect(restored->worldPose.position == before.worldPose.position && restored->parentRelativePose.translationMetres == before.parentRelativePose.translationMetres && restored->parentRelativePose.scale == before.parentRelativePose.scale, "Undo must restore exact world-cache and local values, even when the cache was stale.");
			context.Expect(ApplyTransactionInverseDeltas(fixture.objects, undone.inverseDeltas), "Inverse of undo should restore the edited state without drift.");
			context.Expect(BuildSceneGraphFromObjectManager(fixture.objects).objects.at(visualId).worldPose.scale == visualPose.scale, "Redo must restore rendered scale.");
		});

		registry.Add("Engine.WorldSpatialTransaction.SingularBatchRollbackRestoresExactChildPose", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture();
			const Object visual = fixture.objects.createObject("Visual");
			context.Expect(fixture.objects.setParent(visual, fixture.root, ReparentMode::KeepLocal), "Visual fixture must attach.");
			const auto visualId = fixture.objects.getUUID(visual);
			WorldPose moved{};
			moved.position.offsetMetres.x = 10;
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.root, moved) == WorldCoordinateError::None, "Move ancestor without propagating the live child cache.");
			const Transform before = *fixture.objects.GetComponent<Transform>(visual);
			WorldPose singular = moved;
			singular.scale.x = 0;
			WorldPose desired = moved;
			desired.scale = glm::vec3(0.3f);
			TransactionalMutationBuilder builder("reject-singular-batch", TransactionOrigin::Test);
			builder.UpdateTransformBatch(fixture.objects, {{visualId, desired}, {fixture.rootId, singular}});
			const auto result = CommitTransaction(fixture.objects, builder.Build());
			context.Expect(result.status == TransactionStatus::Rejected, "Parent inversion failure must reject the entire publication.");
			const auto* restored = fixture.objects.GetComponent<Transform>(visual);
			context.Expect(restored->worldPose.position == before.worldPose.position && restored->parentRelativePose.translationMetres == before.parentRelativePose.translationMetres && restored->parentRelativePose.scale == before.parentRelativePose.scale, "Rollback must restore both exact representations even while the parent is temporarily singular.");
			context.Expect(fixture.objects.GetCanonicalWorldPose(fixture.root)->scale == moved.scale, "Rejected batch must restore the parent's authored scale too.");
		});

		registry.Add("Engine.WorldSpatialTransaction.TranslateChildUsesResolvedPoseAndRollsBack", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture();
			const Object child = fixture.objects.createObject("Child");
			context.Expect(fixture.objects.setParent(child, fixture.root, ReparentMode::KeepLocal), "Fixture must attach.");
			WorldPose moved{};
			moved.position.offsetMetres.x = 10;
			context.Expect(fixture.objects.SetCanonicalWorldPose(fixture.root, moved) == WorldCoordinateError::None, "Move ancestor.");
			const auto childId = fixture.objects.getUUID(child);
			const Transform before = *fixture.objects.GetComponent<Transform>(child);
			TransactionalMutationBuilder builder("translate-child", TransactionOrigin::Test);
			builder.TranslateWorld(fixture.objects, childId, {{}, {2, 0, 0}});
			const auto result = CommitTransaction(fixture.objects, builder.Build());
			context.Expect(result.status == TransactionStatus::Committed, "Resolved child translation must commit.");
			context.Expect(BuildSceneGraphFromObjectManager(fixture.objects).objects.at(childId).worldPose.position.offsetMetres.x == 12, "Translation must begin at the inherited world position, not the stale cache.");
			context.Expect(ApplyTransactionInverseDeltas(fixture.objects, result.inverseDeltas), "Exact inverse must restore the translation.");
			const auto* restored = fixture.objects.GetComponent<Transform>(child);
			context.Expect(restored->worldPose.position == before.worldPose.position && restored->parentRelativePose.translationMetres == before.parentRelativePose.translationMetres, "Translation inverse must restore both original poses.");
			TransactionalMutationBuilder failed("translate-child-rollback", TransactionOrigin::Test);
			failed.TranslateWorld(fixture.objects, childId, {{}, {2, 0, 0}}).SetName(fixture.rootId, "Must roll back").InjectCommitFailureForTesting(1);
			context.Expect(CommitTransaction(fixture.objects, failed.Build()).status == TransactionStatus::Rejected, "Injected later failure must reject the translated transaction.");
			context.Expect(restored->worldPose.position == before.worldPose.position && restored->parentRelativePose.translationMetres == before.parentRelativePose.translationMetres, "Injected failure must not leave a partial local edit.");
		});

		registry.Add("Engine.WorldSpatialTransaction.TypedWorldPoseCommitsWithoutFloatAuthority", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::WorldPose pose;
			pose.position.cell = { 15, -12, 3 };
			pose.position.offsetMetres = { 12.125, -0.5, 31'999.875 };
			TerraEngine::Scene::TransactionalMutationBuilder builder("typed-world-pose", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.SetWorldTransform(fixture.objects, fixture.rootId, pose);

			const TerraEngine::Scene::TransactionResult result =
				TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());
			const auto committedPose = fixture.objects.GetCanonicalWorldPose(fixture.root);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Committed,
				"A normalized typed WorldPose should commit through the protected transaction path.");
			context.Expect(committedPose.has_value(), "A transformed object should retain an exact canonical pose.");
			if (committedPose)
			{
				context.ExpectEqual(static_cast<std::int64_t>(15), committedPose->position.cell.x,
					"Typed world transforms must preserve exact signed cell coordinates.");
				context.ExpectEqual(12.125, committedPose->position.offsetMetres.x,
					"Typed world transforms must preserve local double-precision offsets.");
			}
		});

		registry.Add("Engine.WorldSpatialTransaction.PublicationGroupsUseDeterministicChunkOrder", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder later("zeta", TerraEngine::Scene::TransactionOrigin::Test, "test");
			later.ObserveObject(fixture.objects, fixture.rootId).SetName(fixture.rootId, "Zeta");
			TerraEngine::Scene::TransactionalMutationBuilder earlier("alpha", TerraEngine::Scene::TransactionOrigin::Test, "test");
			earlier.ObserveObject(fixture.objects, fixture.rootId).SetTag(fixture.rootId, "Alpha");

			TerraEngine::Scene::TransactionPublicationGroup group;
			group.groupId = "deterministic-group";
			group.chunks = { later.Build(), earlier.Build() };
			const TerraEngine::Scene::TransactionPublicationGroupResult result =
				TerraEngine::Scene::CommitTransactionPublicationGroup(fixture.objects, group);

			context.Expect(result.Committed(), "A valid spatial publication group should commit.");
			context.ExpectEqual(std::string("alpha"), result.chunkReceipts.front().transactionId,
				"Publication receipts should use deterministic transaction-id order.");
			context.ExpectEqual(std::string("Zeta"), fixture.objects.getObjectName(fixture.root),
				"Deterministic ordering must retain every independent typed write.");
			context.ExpectEqual(std::string("Alpha"), fixture.objects.getObjectTag(fixture.root),
				"Deterministic ordering must retain every independent typed write.");
		});

		registry.Add("Engine.WorldSpatialTransaction.RejectsStaleObservedRevisionWithoutPublication", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("stale-observation", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveObject(fixture.objects, fixture.rootId).SetName(fixture.rootId, "Must Not Publish");
			const TerraEngine::Scene::Transaction transaction = builder.Build();
			fixture.objects.setObjectName(fixture.root, "Concurrent Edit");

			const TerraEngine::Scene::TransactionResult result =
				TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Conflicted,
				"A spatial transaction prepared from an old metadata revision must conflict.");
			context.ExpectEqual(std::string("Concurrent Edit"), fixture.objects.getObjectName(fixture.root),
				"A stale transaction must not overwrite the newer live value.");
		});

		registry.Add("Engine.WorldSpatialTransaction.RollbackRestoresAllWritesAfterInjectedFailure", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::TransactionalMutationBuilder builder("rollback", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "Transient Name")
				.SetTag(fixture.rootId, "Transient Tag")
				.InjectCommitFailureForTesting(1U);

			const TerraEngine::Scene::TransactionResult result =
				TerraEngine::Scene::CommitTransaction(fixture.objects, builder.Build());

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Rejected,
				"An injected protected-boundary failure must reject the complete transaction.");
			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root),
				"Rollback must restore writes completed before the injected failure.");
			context.ExpectEqual(std::string("Untagged"), fixture.objects.getObjectTag(fixture.root),
				"Rollback must leave writes after the injected failure invisible too.");
			context.Expect(result.inverseDeltas.empty(),
				"Rejected publications must not expose inverse deltas from a rolled-back attempt.");
		});

		registry.Add("Engine.WorldSpatialTransaction.QueueDefersTypedGroupsUntilProtectedApply", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::TransactionalMutationBuilder builder("frame-end", TerraEngine::Scene::TransactionOrigin::Test, "test");
			builder.ObserveObject(fixture.objects, fixture.rootId).SetName(fixture.rootId, "Visible At Frame End");
			TerraEngine::Scene::TransactionPublicationGroup group;
			group.groupId = "frame-end-group";
			group.chunks.push_back(builder.Build());
			queue.EnqueuePublicationGroup(std::move(group));

			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root),
				"Worker-safe submission must not mutate ObjectManager before its protected boundary.");
			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(fixture.objects, "Frame End");
			context.Expect(result.applied, "The protected Frame End boundary should publish the accepted typed group.");
			context.ExpectEqual(std::string("Visible At Frame End"), fixture.objects.getObjectName(fixture.root),
				"The accepted group becomes visible only after the protected boundary.");
		});
	}
}
