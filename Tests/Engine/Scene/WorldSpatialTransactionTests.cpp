#include "TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include <string>
#include <utility>

namespace TerraEngine::Tests {
	void RegisterWorldSpatialTransactionTests(TestRegistry& registry)
	{
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
