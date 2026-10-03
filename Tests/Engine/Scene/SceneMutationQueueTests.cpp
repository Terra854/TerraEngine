#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Components/SkyComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"


namespace TerraEngine::Tests {
	void RegisterSceneMutationQueueTests(TestRegistry& registry)
	{
		registry.Add("SceneMutationQueue.AppliesAcceptedRequestsInOrder", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();

			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue({"create-parent", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Parent"});
			auto result = queue.Apply(objects, "After Scene");

			context.Expect(result.applied, "A valid create batch should apply.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getRootObjects().size(), "Create should add one root object.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.acceptedMutationCount, "Protected commit accounting should count accepted mutations.");
			context.Expect(result.formattedCommitSummary.find("After Scene") != std::string::npos, "Protected commit summary should name the barrier.");
			context.Expect(queue.Empty(), "Applied queues should clear their requests.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.RejectsConflictingSelfParent", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object target = objects.createObject("Target");
			const TerraEngine::UUID targetId = objects.getUUID(target);
			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);

			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue({"self-parent", TerraEngine::Scene::SceneMutationOperation::ReparentObject, targetId, targetId});
			const auto result = queue.Validate(graph);

			context.Expect(!result.results.front().accepted, "Self-parent requests should be rejected during validation.");
			context.Expect(!result.diagnostics.empty(), "Rejected requests should return diagnostics.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.RollbackLeavesSceneUnchangedOnRejectedBatch", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object target = objects.createObject("Target");
			const TerraEngine::UUID missingId = TerraEngine::GenerateUUID();
			const std::size_t beforeCount = objects.getRootObjects().size();

			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue({"delete-missing", TerraEngine::Scene::SceneMutationOperation::DeleteObject, missingId});
			queue.Enqueue({"delete-existing", TerraEngine::Scene::SceneMutationOperation::DeleteObject, objects.getUUID(target)});
			const auto result = queue.Apply(objects);

			context.Expect(!result.applied, "Rejected batches should not apply partial changes.");
			context.ExpectEqual(beforeCount, objects.getRootObjects().size(), "The existing object should remain after rollback.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.RejectedBatchDoesNotApplyEarlierCreates", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const std::size_t beforeCount = objects.getRootObjects().size();

			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue({ "create-before-reject", TerraEngine::Scene::SceneMutationOperation::CreateObject, {}, {}, "Should Not Exist" });
			queue.Enqueue({ "delete-missing", TerraEngine::Scene::SceneMutationOperation::DeleteObject, TerraEngine::GenerateUUID() });
			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);

			context.Expect(!result.applied, "A rejected request should block the whole batch.");
			context.ExpectEqual(beforeCount, objects.getRootObjects().size(), "Earlier accepted creates should not become visible after a later rejection.");
			// Apply reports the terminal disposition of the entire atomic cohort;
			// Validate may still count only the operation that caused rejection.
			context.ExpectEqual(static_cast<std::size_t>(2), result.rejectedMutationCount, "Both unapplied requests should receive terminal rejection.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.ComponentMutationsNeedValidPayloadTargets", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object target = objects.createObject("Component Target");
			const TerraEngine::UUID targetId = objects.getUUID(target);
			SkyComponent::registerType();

			TerraEngine::Scene::SceneMutationQueue missingName;
			missingName.Enqueue({ "missing-component", TerraEngine::Scene::SceneMutationOperation::AddComponent, targetId });
			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const auto rejected = missingName.Validate(graph);
			context.Expect(!rejected.results.front().accepted, "Component mutations without component names should be rejected.");
			context.Expect(!rejected.diagnostics.empty(), "Rejected component mutations should include diagnostics.");

			TerraEngine::Scene::SceneMutationQueue valid;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "replace-transform";
			request.operation = TerraEngine::Scene::SceneMutationOperation::AddComponent;
			request.targetObjectId = targetId;
			request.componentName = "SkyComponent";
			request.componentPayloadKind = TerraEngine::Scene::SceneComponentMutationPayloadKind::Add;
			valid.Enqueue(request);
			const auto accepted = valid.Apply(objects);
			context.Expect(accepted.applied, "Valid component payload mutations should flush at the approved boundary.");
			context.Expect(valid.Empty(), "Applied component payload mutations should clear the queue.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.RemoveComponentAppliesThroughRegistry", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			Object target = objects.createObject("Component Removal Target");
			const TerraEngine::UUID targetId = objects.getUUID(target);
			context.Expect(objects.GetComponent<Transform>(target) != nullptr, "Created objects should start with a Transform component.");

			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "remove-transform";
			request.operation = TerraEngine::Scene::SceneMutationOperation::RemoveComponent;
			request.targetObjectId = targetId;
			request.componentName = std::string(Transform::kComponentName);
			request.componentPayloadKind = TerraEngine::Scene::SceneComponentMutationPayloadKind::Remove;
			queue.Enqueue(std::move(request));

			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);

			context.Expect(result.applied, "Valid remove-component requests should apply through the protected flush.");
			context.Expect(objects.GetComponent<Transform>(target) == nullptr, "The reflected component registry should remove the requested component.");
			context.Expect(queue.Empty(), "Applied remove-component requests should clear the queue.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.NavigationSourceIntentPublishesWithCommittedTransaction", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			SceneMutationQueue queue;
			TransactionalMutationBuilder builder("navigation-source-create", TransactionOrigin::System, "NavigationTest");
			builder.CreateObject(1, "Dynamic Support");
			queue.EnqueueTransaction(std::move(builder).Build());

			NavigationSourceMutationIntent intent{};
			intent.intentId = "navigation-intent-create";
			intent.transactionId = "navigation-source-create";
			intent.sourceId = TerraEngine::GenerateUUID();
			intent.operation = NavigationSourceMutationOperation::Create;
			intent.newBounds = {{{0, 0, 0}, {-2.0, 0.0, -2.0}}, {{0, 0, 0}, {2.0, 1.0, 2.0}}, true};
			intent.committedSourceRevision = 1U;
			intent.physicalReadyRevision = 1U;
			intent.newEnabled = true;
			context.Expect(queue.EnqueueNavigationSourceMutationIntent(intent),
				"A valid copied navigation intent should be admitted before the scene transaction flush.");

			const SceneMutationBatchResult result = queue.Apply(objects, "navigation-source-commit");
			context.Expect(result.navigationSourceMutationReceipts.size() == 1U
				&& result.navigationSourceMutationReceipts.front().accepted,
				"The source change must become observable at the same protected boundary as its committed transaction.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.NavigationSourceIntentRejectsAtomicallyWithTransaction", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const TerraEngine::UUID missingObject = TerraEngine::GenerateUUID();
			SceneMutationQueue queue;
			TransactionalMutationBuilder builder("navigation-source-rejected", TransactionOrigin::System, "NavigationTest");
			builder.DeleteObject(missingObject);
			queue.EnqueueTransaction(std::move(builder).Build());

			NavigationSourceMutationIntent intent{};
			intent.intentId = "navigation-intent-delete";
			intent.transactionId = "navigation-source-rejected";
			intent.sourceId = missingObject;
			intent.operation = NavigationSourceMutationOperation::Delete;
			intent.oldBounds = {{{0, 0, 0}, {-2.0, 0.0, -2.0}}, {{0, 0, 0}, {2.0, 1.0, 2.0}}, true};
			intent.expectedSourceRevision = 1U;
			intent.committedSourceRevision = 2U;
			intent.physicalReadyRevision = 2U;
			intent.oldEnabled = true;
			context.Expect(queue.EnqueueNavigationSourceMutationIntent(intent),
				"The copied navigation deletion should wait for the owning transaction decision.");

			const SceneMutationBatchResult result = queue.Apply(objects, "navigation-source-reject");
			context.Expect(result.navigationSourceMutationReceipts.size() == 1U
				&& !result.navigationSourceMutationReceipts.front().accepted,
				"A rejected scene transaction must reject its navigation change without partial visibility.");
			context.Expect(result.navigationSourceMutationReceipts.front().transactionStatus != TransactionStatus::Committed,
				"A rejected source change receipt must retain the exact non-committed transaction decision.");
			objects.clear();
		});
	}
}
