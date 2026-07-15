#include "TestHarness.h"
#include "Scene/TransactionalMutation.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Core/Object/ObjectManager.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	void RegisterTransactionalMutationConflictTests(TestRegistry& registry)
	{
		registry.Add("Engine.TransactionalMutation.RejectsStaleObservedObjectWithoutPartialWrites", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-stale", TerraEngine::Scene::TransactionOrigin::Test, "test")
				.ObserveObject(fixture.objects, fixture.rootId)
				.SetName(fixture.rootId, "ShouldNotApply")
				.Build();
			fixture.objects.setObjectName(fixture.root, "ChangedOutsideTransaction");

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Conflicted, "Stale metadata observation should conflict.");
			context.ExpectEqual(std::string("ChangedOutsideTransaction"), fixture.objects.getObjectName(fixture.root), "Rejected transaction must not apply partial writes.");
		});

		registry.Add("Engine.TransactionalMutation.RejectsMissingTarget", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			TerraEngine::Scene::Transaction transaction = TerraEngine::Scene::TransactionalMutationBuilder("tx-missing", TerraEngine::Scene::TransactionOrigin::Test, "test")
				.SetName(fixture.rootId, "DeletedTarget")
				.Build();
			fixture.objects.removeObject(fixture.root);

			const TerraEngine::Scene::TransactionResult result = TerraEngine::Scene::CommitTransaction(fixture.objects, transaction);

			context.Expect(result.status == TerraEngine::Scene::TransactionStatus::Conflicted, "Missing target should conflict.");
			context.Expect(!result.conflicts.empty(), "Missing target should be reported.");
		});

		registry.Add("Engine.TransactionalMutation.PublicationGroupsRejectEveryChunkSizeWithoutPartialWrites", [](TestContext& context) {
			// A preparation chunk is only a scheduling unit. This matrix proves a
			// missing object in a later chunk rejects the complete simulated tick.
			for(const std::size_t chunkSize : std::array<std::size_t, 3U>{1U, 2U, 3U}) {
				TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
				const Object second = fixture.objects.createObject("Second");
				const Object third = fixture.objects.createObject("Third");
				const std::array<TerraEngine::UUID, 3U> objectIds{
					fixture.rootId, fixture.objects.getUUID(second), fixture.objects.getUUID(third)};

				TerraEngine::Scene::TransactionPublicationGroup group;
				group.groupId = "chunk-size-" + std::to_string(chunkSize);
				for(std::size_t first = 0U, chunk = 0U; first < objectIds.size(); first += chunkSize, ++chunk) {
					TerraEngine::Scene::TransactionalMutationBuilder builder(
						group.groupId + "-" + std::to_string(chunk), TerraEngine::Scene::TransactionOrigin::Test, "tick-wide-rollback");
					builder.ObserveScene(fixture.objects);
					for(std::size_t index = first; index < std::min(first + chunkSize, objectIds.size()); ++index) {
						builder.ObserveObject(fixture.objects, objectIds[index]).SetName(objectIds[index], "Must Not Publish");
					}
					group.chunks.push_back(std::move(builder).Build());
				}

				fixture.objects.removeObject(third);
				const TerraEngine::Scene::TransactionPublicationGroupResult result =
					TerraEngine::Scene::CommitTransactionPublicationGroup(fixture.objects, group);
				context.Expect(!result.Committed(), "A missing target must reject every tick-wide chunk configuration.");
				context.Expect(result.appliedWriteCount == 0U,
					"Rejected tick-wide publications must report zero visible writes regardless of chunk size.");
				context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root),
					"The first chunk must not publish before a later chunk's missing target is found.");
				context.ExpectEqual(std::string("Second"), fixture.objects.getObjectName(second),
					"A middle chunk must remain invisible when the complete group is rejected.");
				fixture.objects.clear();
			}
		});

		registry.Add("Engine.TransactionalMutation.PublicationGroupsRollBackReparentConflictsAsOneTick", [](TestContext& context) {
			TransactionalMutationFixture fixture = MakeTransactionalMutationFixture();
			const Object originalParent = fixture.root;
			const Object requestedParent = fixture.objects.createObject("Requested Parent");
			const Object conflictingParent = fixture.objects.createObject("Conflicting Parent");
			const Object child = fixture.objects.createObject("Child");
			context.Expect(fixture.objects.setParent(child, originalParent), "The rollback fixture needs an authored parent before preparation.");
			const TerraEngine::UUID childId = fixture.objects.getUUID(child);
			const TerraEngine::UUID requestedParentId = fixture.objects.getUUID(requestedParent);

			TerraEngine::Scene::TransactionPublicationGroup group;
			group.groupId = "reparent-conflict";
			TerraEngine::Scene::TransactionalMutationBuilder names("reparent-conflict-names", TerraEngine::Scene::TransactionOrigin::Test, "tick-wide-rollback");
			names.ObserveScene(fixture.objects).ObserveObject(fixture.objects, fixture.rootId).SetName(fixture.rootId, "Must Not Publish");
			group.chunks.push_back(std::move(names).Build());
			TerraEngine::Scene::TransactionalMutationBuilder hierarchy("reparent-conflict-hierarchy", TerraEngine::Scene::TransactionOrigin::Test, "tick-wide-rollback");
			hierarchy.ObserveScene(fixture.objects).ObserveObject(fixture.objects, childId)
				.ObserveObject(fixture.objects, requestedParentId)
				.ReparentObject(childId, TerraEngine::Scene::ExistingObject(requestedParentId));
			group.chunks.push_back(std::move(hierarchy).Build());

			// A later hierarchy change invalidates the captured read set. The target
			// reparent is deliberately different, so a partial publish is obvious.
			context.Expect(fixture.objects.setParent(child, conflictingParent), "The conflict fixture must advance hierarchy state outside the group.");
			const TerraEngine::Scene::TransactionPublicationGroupResult result =
				TerraEngine::Scene::CommitTransactionPublicationGroup(fixture.objects, group);
			context.Expect(!result.Committed() && result.appliedWriteCount == 0U,
				"A reparent conflict must reject names and hierarchy writes together.");
			context.ExpectEqual(std::string("TransactionRoot"), fixture.objects.getObjectName(fixture.root),
				"A sibling metadata chunk must not leak when hierarchy preflight conflicts.");
			context.Expect(fixture.objects.getParent(child) == conflictingParent,
				"Rejected reparenting must preserve the externally committed hierarchy exactly.");
			fixture.objects.clear();
		});
	}
}
