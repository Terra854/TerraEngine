#include "../TestHarness.h"

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
			const TerraEngine::UUID targetId = objects.getUUID(target.EntityId());
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
			queue.Enqueue({"delete-existing", TerraEngine::Scene::SceneMutationOperation::DeleteObject, objects.getUUID(target.EntityId())});
			const auto result = queue.Apply(objects);

			context.Expect(!result.applied, "Rejected batches should not apply partial changes.");
			context.ExpectEqual(beforeCount, objects.getRootObjects().size(), "The existing object should remain after rollback.");
			objects.clear();
		});

		registry.Add("SceneMutationQueue.ComponentMutationsNeedValidPayloadTargets", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object target = objects.createObject("Component Target");
			const TerraEngine::UUID targetId = objects.getUUID(target.EntityId());

			TerraEngine::Scene::SceneMutationQueue missingName;
			missingName.Enqueue({ "missing-component", TerraEngine::Scene::SceneMutationOperation::AddComponent, targetId });
			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const auto rejected = missingName.Validate(graph);
			context.Expect(!rejected.results.front().accepted, "Component mutations without component names should be rejected.");
			context.Expect(!rejected.diagnostics.empty(), "Rejected component mutations should include diagnostics.");

			TerraEngine::Scene::SceneMutationQueue valid;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "replace-transform";
			request.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			request.targetObjectId = targetId;
			request.componentName = "Transform";
			request.componentPayloadKind = TerraEngine::Scene::SceneComponentMutationPayloadKind::Replace;
			valid.Enqueue(request);
			const auto accepted = valid.Apply(objects);
			context.Expect(accepted.applied, "Valid component payload mutations should flush at the approved boundary.");
			context.Expect(valid.Empty(), "Applied component payload mutations should clear the queue.");
			objects.clear();
		});
	}
}
