#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptingSystem.h"

namespace TerraEngine::Tests {
	void RegisterScriptingMutationTests(TestRegistry& registry)
	{
		registry.Add("Scripting.Mutation.RequestCarriesSourceMetadata", [](TestContext& context) {
			ScriptingSystem::ScriptDispatchContext dispatchContext;
			dispatchContext.workerDispatch = true;
			dispatchContext.sourceObjectId = 12;
			dispatchContext.sourceBindingId = "script-binding";
			dispatchContext.frameOrder = 99;

			const TerraEngine::Scene::SceneMutationRequest request = ScriptingSystem::Instance().BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::CreateObject,
				dispatchContext,
				TerraEngine::EMPTY_UUID,
				"Spawned");

			context.ExpectEqual(static_cast<std::uint32_t>(12), request.sourceObjectId, "Queued script mutations should keep the source object id.");
			context.ExpectEqual(std::string("script-binding"), request.sourceBindingId, "Queued script mutations should keep the source binding id.");
			context.ExpectEqual(static_cast<std::uint64_t>(99), request.sourceFrameOrder, "Queued script mutations should keep deterministic source order.");
		});

		registry.Add("Scripting.Mutation.FlushCreatesQueuedObject", [](TestContext& context) {
			ObjectManager::instance().clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();

			ScriptingSystem::ScriptDispatchContext dispatchContext;
			dispatchContext.workerDispatch = true;
			dispatchContext.sourceObjectId = 1;
			dispatchContext.sourceBindingId = "creator";
			dispatchContext.frameOrder = 1;
			ScriptingSystem::Instance().RecordScriptMutationRequest(ScriptingSystem::Instance().BuildScriptMutationRequest(
				TerraEngine::Scene::SceneMutationOperation::CreateObject,
				dispatchContext,
				TerraEngine::EMPTY_UUID,
				"Queued"));

			TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(ObjectManager::instance());
			context.Expect(result.applied, "Queued script mutations should apply at the flush point.");
			context.ExpectEqual(static_cast<std::size_t>(1), ObjectManager::instance().getRootObjects().size(), "Flush should create the requested object.");

			ObjectManager::instance().clear();
		});
	}
}
