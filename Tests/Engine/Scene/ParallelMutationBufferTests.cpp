#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"

namespace TerraEngine::Tests {
	void RegisterParallelMutationBufferTests(TestRegistry& registry)
	{
		registry.Add("Engine.SceneMutationQueue.WorkerBuffersMergeDeterministically", [](TestContext& context) {
			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest late;
			late.requestId = "late";
			late.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			late.displayName = "Late";
			late.sourceFrameOrder = 20;
			TerraEngine::Scene::SceneMutationRequest early = late;
			early.requestId = "early";
			early.displayName = "Early";
			early.sourceFrameOrder = 10;

			queue.EnqueueFromWorker(2, late);
			queue.EnqueueFromWorker(1, early);
			const auto pending = queue.PendingRequests();

			context.ExpectEqual(static_cast<std::size_t>(2), pending.size(), "Both worker requests should be visible after merge.");
			context.ExpectEqual(std::string("early"), pending[0].requestId, "Worker buffers should merge by frame order before request id.");
			context.ExpectEqual(std::string("late"), pending[1].requestId, "Later worker request should remain second.");
		});

		registry.Add("Engine.SceneMutationQueue.WorkerBufferedCreatesFlushAtProtectedCommit", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "worker-create";
			request.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			request.displayName = "Worker Created";
			queue.EnqueueFromWorker(0, request);

			const auto result = queue.Apply(objects, "worker-buffer-flush");

			context.Expect(result.applied, "Worker-buffered create should apply at protected flush.");
			context.Expect(queue.Empty(), "Successful protected flush should clear worker buffers.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getRootObjects().size(), "One object should be created from the worker buffer.");
			objects.clear();
		});

		registry.Add("Engine.SceneMutationQueue.RandomizedWorkerBatchValidationIsDeterministic", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			std::vector<TerraEngine::UUID> existingObjects;
			for (std::uint32_t index = 0; index < 12u; ++index)
			{
				Object object = objects.createObject("Mutation Target " + std::to_string(index));
				existingObjects.push_back(objects.getUUID(object));
			}

			TerraEngine::Scene::SceneMutationQueue queue;
			std::uint32_t seed = 0xC0FFEEu;
			for (std::uint32_t index = 0; index < 64u; ++index)
			{
				seed = (seed * 1664525u) + 1013904223u;
				TerraEngine::Scene::SceneMutationRequest request;
				request.requestId = "randomized-" + std::to_string(index);
				request.sourceFrameOrder = index;
				const bool validTarget = (seed % 5u) != 0u;
				request.targetObjectId = validTarget
					? existingObjects[seed % existingObjects.size()]
					: TerraEngine::GenerateUUID();
				request.operation = (seed % 3u) == 0u
					? TerraEngine::Scene::SceneMutationOperation::DeleteObject
					: TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
				if (request.operation == TerraEngine::Scene::SceneMutationOperation::UpdateComponent)
				{
					request.componentName = (seed % 7u) == 0u ? std::string{} : std::string("Transform");
				}
				queue.EnqueueFromWorker(index % 4u, std::move(request));
			}

			const TerraEngine::Scene::SceneGraph graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(objects);
			const TerraEngine::Scene::SceneMutationBatchResult first = queue.Validate(graph);
			const TerraEngine::Scene::SceneMutationBatchResult second = queue.Validate(graph);

			context.ExpectEqual(first.inputMutationCount, second.inputMutationCount, "Repeated randomized validation should see the same request snapshot.");
			context.ExpectEqual(first.acceptedMutationCount, second.acceptedMutationCount, "Accepted mutation count should be deterministic.");
			context.ExpectEqual(first.rejectedMutationCount, second.rejectedMutationCount, "Rejected mutation count should be deterministic.");
			context.Expect(first.rejectedMutationCount > 0u, "Randomized validation should include intentionally invalid requests.");
			context.Expect(first.acceptedMutationCount > 0u, "Randomized validation should include valid worker requests.");
			objects.clear();
		});
	}
}
