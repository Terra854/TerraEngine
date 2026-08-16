#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Components/CameraComponent.h"
#include "Components/ScriptComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptGlue.h"
#include "Scripting/ScriptingSystem.h"

#include <glm/vec3.hpp>

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

		registry.Add("Scripting.Mutation.MainThreadManagedCallbacksRemainInvisibleUntilFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			Object target = objects.createObject("Before");

			ScriptingSystem::ScriptDispatchContext dispatchContext;
			dispatchContext.workerDispatch = false;
			dispatchContext.sourceObjectId = static_cast<std::uint32_t>(target.EntityId());
			dispatchContext.sourceObjectUuid = objects.getUUID(target);
			dispatchContext.sourceBindingId = "main-thread-managed";
			dispatchContext.frameOrder = 77U;
			ScriptingSystem::SetCurrentScriptDispatchContext(dispatchContext);
			Native_Object_SetName(static_cast<std::uint32_t>(target.EntityId()), "After");
			Native_Object_SetTag(static_cast<std::uint32_t>(target.EntityId()), "QueuedTag");
			const bool cameraAccepted = Native_AddComponent(static_cast<std::uint32_t>(target.EntityId()), "CameraComponent");
			const bool scriptAccepted = Native_ScriptComponent_AddBinding(static_cast<std::uint32_t>(target.EntityId()), "Gameplay.QueuedScript");
			ScriptingSystem::ClearCurrentScriptDispatchContext();

			context.Expect(cameraAccepted && scriptAccepted, "Main-thread managed component writes should be accepted into the mutation queue.");
			context.ExpectEqual(std::string("Before"), objects.getObjectName(target), "Queued metadata must not publish during the managed callback.");
			context.ExpectEqual(std::string("Untagged"), objects.getObjectTag(target), "Queued tags must retain the old value until the protected flush.");
			context.Expect(objects.GetComponent<CameraComponent>(target) == nullptr, "Queued component membership must remain invisible before the protected flush.");
			context.Expect(objects.GetComponent<ScriptComponent>(target) == nullptr, "Queued script bindings must not create live component storage during callback dispatch.");

			const TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "main-thread-managed-hardening-test");
			const ScriptComponent* scripts = objects.GetComponent<ScriptComponent>(target);
			context.Expect(result.applied, "The protected boundary should publish the complete managed mutation batch.");
			context.ExpectEqual(std::string("After"), objects.getObjectName(target), "The flush should publish queued object metadata.");
			context.ExpectEqual(std::string("QueuedTag"), objects.getObjectTag(target), "The flush should publish the queued tag.");
			context.Expect(objects.GetComponent<CameraComponent>(target) != nullptr, "The flush should publish queued component membership.");
			context.Expect(scripts != nullptr && scripts->HasBinding("Gameplay.QueuedScript"), "The flush should publish queued managed script bindings.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.ManagedTransactionJsonQueuesAndCommits", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();

			const char* response = Native_Scene_SubmitTransactionJson(R"({"id":"managed-create","originName":"test-managed-callback","writes":[{"op":"create","localObjectId":1,"name":"Managed Transaction Spawn"}]})");
			const std::string responseText = response != nullptr ? response : "";
			const TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "managed-transaction-test");

			context.Expect(responseText.find(R"("ok":true)") != std::string::npos, "Managed transaction bridge should accept a valid JSON payload.");
			context.Expect(result.applied, "Managed transaction payloads should commit through the protected mutation flush.");
			context.ExpectEqual(static_cast<std::size_t>(1), objects.getRootObjects().size(), "Managed transaction create should become a scene object.");
			context.Expect(TerraEngine::Scene::SceneMutationQueue::Instance().Empty(), "Managed transaction flush should clear pending queue state.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.PendingRequestsAreSortedByFrameOrder", [](TestContext& context) {
			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest later;
			later.requestId = "later";
			later.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			later.sourceFrameOrder = 20;
			TerraEngine::Scene::SceneMutationRequest earlier;
			earlier.requestId = "earlier";
			earlier.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			earlier.sourceFrameOrder = 10;
			queue.Enqueue(later);
			queue.Enqueue(earlier);

			const std::vector<TerraEngine::Scene::SceneMutationRequest> pending = queue.PendingRequests();

			context.ExpectEqual(std::string("earlier"), pending.front().requestId, "Pending script mutations should sort by source frame order before flushing.");
			context.ExpectEqual(std::string("later"), pending.back().requestId, "Later script mutations should flush after earlier script callbacks.");
		});

		registry.Add("Scripting.Mutation.UpdateComponentPayloadWritesReflectedTransform", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			Object target = objects.createObject("Mover");
			const TerraEngine::UUID targetId = objects.getUUID(target);

			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "move";
			request.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			request.targetObjectId = targetId;
			request.componentName = "Transform";
			request.payloadJson = R"({"propertyName":"Position","valueKind":5,"value":[1.0,2.0,3.0]})";
			queue.Enqueue(request);

			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);
			const Transform* transform = objects.GetComponent<Transform>(target);

			context.Expect(!result.applied, "Legacy JSON component envelopes must be rejected by the strict typed mutation boundary.");
			context.Expect(transform != nullptr, "The test object should still own its transform.");
			context.ExpectEqual(0.0, transform->worldPose.position.offsetMetres.x, "Rejected legacy payloads must not mutate canonical world values.");
			context.Expect(!result.diagnostics.empty(), "Rejected legacy payloads should explain the strict envelope requirement.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.TypedPayloadWritesReflectedTransform", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			Object target = objects.createObject("Typed Mover");
			const TerraEngine::UUID targetId = objects.getUUID(target);

			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "typed-move";
			request.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			request.targetObjectId = targetId;
			request.componentName = "Transform";
			request.typedComponentPayload.propertyName = "Position";
			request.typedComponentPayload.valueKind = 5;
			request.typedComponentPayload.value = glm::vec3(4.0f, 5.0f, 6.0f);
			request.typedComponentPayload.valid = true;
			queue.Enqueue(request);

			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);
			const Transform* transform = objects.GetComponent<Transform>(target);

			context.Expect(!result.applied, "Generic Transform component payloads must be rejected; canonical movement uses a spatial transaction.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.typedMutationCount, "Typed component updates should be counted separately from JSON fallbacks.");
			context.ExpectEqual(static_cast<std::size_t>(0), result.jsonMutationFallbackCount, "Typed component updates should avoid the JSON fallback counter.");
			context.ExpectEqual(0.0, transform->worldPose.position.offsetMetres.x, "Rejected Transform payloads must leave canonical world values unchanged.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.JsonFallbackIsCountedForLegacyPayloads", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			Object target = objects.createObject("Json Mover");
			const TerraEngine::UUID targetId = objects.getUUID(target);

			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "json-move";
			request.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			request.targetObjectId = targetId;
			request.componentName = "Transform";
			request.payloadJson = R"({"propertyName":"Position","valueKind":5,"value":[7.0,8.0,9.0]})";
			request.usedJsonFallback = true;
			queue.Enqueue(request);

			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);
			const Transform* transform = objects.GetComponent<Transform>(target);

			context.Expect(!result.applied, "Legacy JSON component payloads must be rejected without compatibility aliases.");
			context.ExpectEqual(static_cast<std::size_t>(0), result.typedMutationCount, "JSON component updates should not increment typed counters.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.jsonMutationFallbackCount, "JSON component updates should increment fallback counters.");
			context.ExpectEqual(0.0, transform->worldPose.position.offsetMetres.x, "Rejected JSON fallback payloads must not mutate canonical world values.");
			context.Expect(!result.diagnostics.empty(), "Rejected JSON fallback payloads should expose a diagnostic.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.CoalescesSamePropertyWritesByDeterministicKey", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			ComponentRegistry::RegisterAllComponents();
			Object target = objects.createObject("Coalesced Mover");
			const TerraEngine::UUID targetId = objects.getUUID(target);

			TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest first;
			first.requestId = "first";
			first.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			first.targetObjectId = targetId;
			first.componentName = "Transform";
			first.sourceFrameOrder = 10;
			first.coalescingKey = "10|target|Transform|Position";
			first.typedComponentPayload.propertyName = "Position";
			first.typedComponentPayload.value = glm::vec3(1.0f, 1.0f, 1.0f);
			first.typedComponentPayload.valid = true;
			TerraEngine::Scene::SceneMutationRequest second = first;
			second.requestId = "second";
			second.typedComponentPayload.value = glm::vec3(2.0f, 3.0f, 4.0f);
			queue.Enqueue(first);
			queue.Enqueue(second);

			const std::vector<TerraEngine::Scene::SceneMutationRequest> pending = queue.PendingRequests();
			const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(objects);
			const Transform* transform = objects.GetComponent<Transform>(target);

			context.ExpectEqual(static_cast<std::size_t>(1), pending.size(), "Same-key writes should coalesce before validation.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.coalescedMutationCount, "Coalesced write count should report removed duplicate payloads before strict validation.");
			context.Expect(!result.applied, "Coalesced generic Transform writes must be rejected by the spatial authority boundary.");
			context.ExpectEqual(0.0, transform->worldPose.position.offsetMetres.x, "Rejected coalesced Transform writes must leave world values unchanged.");
			objects.clear();
		});

		registry.Add("Scripting.Mutation.WorkerDeferredMutationStressFlushesDeterministically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();

			for (std::uint32_t index = 0; index < 96u; ++index)
			{
				ScriptingSystem::ScriptDispatchContext dispatchContext;
				dispatchContext.workerDispatch = true;
				dispatchContext.sourceObjectId = index + 1u;
				dispatchContext.sourceBindingId = "stress-binding-" + std::to_string(index % 8u);
				dispatchContext.frameOrder = 1000u + index;
				ScriptingSystem::Instance().RecordScriptMutationRequest(ScriptingSystem::Instance().BuildScriptMutationRequest(
					TerraEngine::Scene::SceneMutationOperation::CreateObject,
					dispatchContext,
					TerraEngine::EMPTY_UUID,
					"Script Spawn " + std::to_string(index)));
			}

			const std::vector<TerraEngine::Scene::SceneMutationRequest> pending = TerraEngine::Scene::SceneMutationQueue::Instance().PendingRequests();
			const TerraEngine::Scene::SceneMutationBatchResult result = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "script-stress-flush");

			context.ExpectEqual(static_cast<std::size_t>(96), pending.size(), "Script-originated worker stress should enqueue every deferred mutation.");
			context.Expect(result.applied, "Script-originated worker mutations should apply at the protected flush point.");
			context.ExpectEqual(static_cast<std::size_t>(96), result.acceptedMutationCount, "Every stress create should be accepted.");
			context.ExpectEqual(static_cast<std::size_t>(96), objects.getRootObjects().size(), "Every accepted script create should become a root object.");
			context.Expect(TerraEngine::Scene::SceneMutationQueue::Instance().Empty(), "Successful script stress flush should clear the global mutation queue.");
			objects.clear();
		});
	}
}

