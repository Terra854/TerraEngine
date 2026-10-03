#include "TestHarness.h"
#include "Scene/TransactionalMutationTestFixtures.h"

#include "Components/CameraComponent.h"
#include "Components/ScriptComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace TerraEngine::Tests {
	namespace {
		// This component deliberately lacks safe transactional authoring opt-in.
		// A reflected setter alone cannot promise that rollback is supported.
		struct UnpreparedGenericComponent : Component<UnpreparedGenericComponent> {
			static constexpr std::string_view kComponentName = "UnpreparedGenericComponent";
			int value = 3;
			static void reflect(entt::meta_factory<UnpreparedGenericComponent>& factory) {
				TerraEngine::Reflection::RegisterDataProperty<&UnpreparedGenericComponent::value>(factory, "value");
			}
		};

		// The setter owns only scalar state. A controlled exception verifies the
		// production queue prepares it away from earlier live metadata writes.
		struct ThrowingGenericComponent : Component<ThrowingGenericComponent> {
			static constexpr std::string_view kComponentName = "ThrowingGenericComponent";
			static constexpr bool kTransactionalAuthoringSafe = true;
			int value = 4;
			int GetValue() const noexcept { return value; }
			void SetValue(int next) {
				if (next == 17) throw std::runtime_error("Controlled generic authored setter failure.");
				value = next;
			}
			static void reflect(entt::meta_factory<ThrowingGenericComponent>& factory) {
				TerraEngine::Reflection::RegisterAccessorProperty<&ThrowingGenericComponent::SetValue,
					&ThrowingGenericComponent::GetValue>(factory, "value");
			}
		};

		struct GenericSceneBefore {
			nlohmann::json authored;
			std::size_t entities;
			std::uint64_t spatial, hierarchy, membership, components, scene, publication;
		};

		GenericSceneBefore CaptureGenericScene(ObjectManager& objects) {
			nlohmann::json authored = nlohmann::json::object();
			for (const Object& root : objects.getRootObjects()) {
				authored[TerraEngine::ToHexString(objects.getUUID(root))] = Serializer::SerializeObject(root.EntityId());
			}
			return {std::move(authored), objects.getRegistry().storage<entt::entity>().free_list(),
				objects.GetSpatialRevision(), objects.GetHierarchyRevision(), objects.GetRuntimeActiveMembershipRevision(),
				objects.GetComponentMutationRevision(), objects.GetSceneGeneration(),
				objects.LoadPublishedSceneChangePacket().PublicationRevision()};
		}

		void ExpectGenericSceneUnchanged(TestContext& context, ObjectManager& objects, const GenericSceneBefore& before) {
			const auto after = CaptureGenericScene(objects);
			context.Expect(before.authored == after.authored,
				"Rejected generic group must retain every authored value, identity and hierarchy edge.");
			context.ExpectEqual(before.entities, after.entities, "Rejected generic group must not leak or remove entities.");
			context.ExpectEqual(before.spatial, after.spatial, "Rejected generic group must not change spatial visibility.");
			context.ExpectEqual(before.hierarchy, after.hierarchy, "Rejected generic group must not change hierarchy visibility.");
			context.ExpectEqual(before.membership, after.membership, "Rejected generic group must not change active membership.");
			context.ExpectEqual(before.components, after.components, "Rejected generic group must not change component visibility.");
			context.ExpectEqual(before.scene, after.scene, "Rejected generic group must preserve scene lifetime.");
			context.ExpectEqual(before.publication, after.publication, "Rejected generic group must not publish a new scene packet.");
			context.Expect(objects.PublishSceneChangePacket().Empty(), "Rejected generic group must leave no pending scene changes.");
		}

		TerraEngine::Scene::SceneMutationRequest RenameRequest(TerraEngine::UUID id) {
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "earlier-generic-rename";
			request.sourceFrameOrder = 1;
			request.operation = TerraEngine::Scene::SceneMutationOperation::SetObjectName;
			request.targetObjectId = id;
			request.metadataValue = "Earlier Rename Must Not Survive";
			return request;
		}

		TerraEngine::Scene::SceneMutationRequest CameraPropertyRequest(TerraEngine::UUID id) {
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "later-generic-property";
			request.sourceFrameOrder = 2;
			request.operation = TerraEngine::Scene::SceneMutationOperation::UpdateComponent;
			request.targetObjectId = id;
			request.componentName = std::string(CameraComponent::kComponentName);
			request.payloadJson = nlohmann::json{{"propertyName", "nearPlane"}, {"valueKind", 2}, {"value", 0.25F}}.dump();
			request.usedJsonFallback = true;
			return request;
		}

		// Use only existing generic ingress and Apply. The initial rename makes
		// partial publication observable even if a later request merely reports a
		// warning, swallows a parse exception, or returns a false setter result.
		void RejectGenericGroup(TestContext& context,
			const std::function<void(TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest&)>& configure) {
			auto fixture = MakeTransactionalMutationFixture();
			fixture.objects.AddComponent<CameraComponent>(fixture.root);
			const Object child = fixture.objects.createObject("Generic Baseline Child");
			fixture.objects.setParent(child, fixture.root);
			auto failing = CameraPropertyRequest(fixture.rootId);
			configure(fixture, failing);
			(void)fixture.objects.PublishSceneChangePacket();
			const auto before = CaptureGenericScene(fixture.objects);
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue(RenameRequest(fixture.rootId));
			queue.Enqueue(failing);
			std::optional<TerraEngine::Scene::SceneMutationBatchResult> result;
			std::exception_ptr escaped;
			try { result = queue.Apply(fixture.objects, "Generic failure safety"); }
			catch (...) { escaped = std::current_exception(); }
			ExpectGenericSceneUnchanged(context, fixture.objects, before);
			context.Expect(!escaped && result.has_value(), "Invalid generic authoring must return a terminal rejection rather than escape.");
			context.Expect(queue.Empty(), "Rejected generic cohort must be consumed exactly once.");
			context.Expect(queue.DrainCompletedEditorTransactions().empty(), "Rejected generic authoring must publish no editor history.");
			if (!result) return;
			context.Expect(!result->applied, "Rejected generic group must report no applied publication.");
			context.ExpectEqual(std::size_t{2}, result->inputMutationCount, "Result must account for both submitted requests.");
			context.ExpectEqual(std::size_t{0}, result->acceptedMutationCount, "Rejected group must report zero accepted/applied requests.");
			context.ExpectEqual(std::size_t{2}, result->rejectedMutationCount, "Atomic rejection must count the earlier rename too.");
			context.ExpectEqual(std::size_t{2}, result->results.size(), "Every submitted request needs a truthful terminal receipt.");
			if (result->results.size() == 2U) {
				context.ExpectEqual(std::string("earlier-generic-rename"), result->results[0].requestId, "Receipt ordering must retain the earlier request identity.");
				context.ExpectEqual(failing.requestId, result->results[1].requestId, "Failure mapping must retain the original request identity.");
				context.Expect(!result->results[0].accepted && !result->results[1].accepted,
					"Neither request in a rejected atomic group may claim success.");
				context.Expect(!result->results[1].diagnostics.empty(), "Failing request must retain a diagnostic explaining rejection.");
			}
			for (const auto& transaction : result->transactionResults) {
				context.Expect(transaction.status != TerraEngine::Scene::TransactionStatus::Committed
					&& transaction.appliedWriteCount == 0U && transaction.inverseDeltas.empty(),
					"Native lowering receipts must expose no committed writes or inverse history for rejected authoring.");
			}
		}
	}

	void RegisterGenericMutationFailureSafetyTests(TestRegistry& registry) {
		registry.Add("Engine.SceneMutationFailureSafety.GenericMalformedNumericRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson = nlohmann::json{{"propertyName", "nearPlane"}, {"valueKind", 2}, {"value", "not a number"}}.dump();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericMissingValueRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson = nlohmann::json{{"propertyName", "nearPlane"}, {"valueKind", 2}}.dump();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericMissingPropertyRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson = nlohmann::json{{"valueKind", 2}, {"value", 0.25F}}.dump();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericUnknownValueKindRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson = nlohmann::json{{"propertyName", "nearPlane"}, {"valueKind", 999}, {"value", 0.25F}}.dump();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericUnknownPropertyRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson = nlohmann::json{{"propertyName", "unknownAuthoredProperty"}, {"valueKind", 2}, {"value", 0.25F}}.dump();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericUnknownComponentRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.componentName = "UnregisteredGenericComponent";
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericMissingComponentRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture& fixture, TerraEngine::Scene::SceneMutationRequest& request) {
				// A child has only its default Transform; its Camera property target
				// is valid as an object but absent as a component.
				request.targetObjectId = fixture.objects.getUUID(fixture.objects.getChildren(fixture.root).front());
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericUnsupportedAuthoringRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture& fixture, TerraEngine::Scene::SceneMutationRequest& request) {
				fixture.objects.AddComponent<UnpreparedGenericComponent>(fixture.root);
				request.componentName = std::string(UnpreparedGenericComponent::kComponentName);
				request.payloadJson.clear(); request.usedJsonFallback = false;
				request.typedComponentPayload = {"value", 1, TerraEngine::Reflection::Variant{9}, true};
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericThrowingSetterRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture& fixture, TerraEngine::Scene::SceneMutationRequest& request) {
				fixture.objects.AddComponent<ThrowingGenericComponent>(fixture.root);
				request.componentName = std::string(ThrowingGenericComponent::kComponentName);
				request.payloadJson.clear(); request.usedJsonFallback = false;
				request.typedComponentPayload = {"value", 1, TerraEngine::Reflection::Variant{17}, true};
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericInvalidScriptBindingRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.operation = TerraEngine::Scene::SceneMutationOperation::AddScriptBinding;
				request.componentName.clear(); request.payloadJson.clear(); request.usedJsonFallback = false;
				request.metadataValue.clear();
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericTypedKindMismatchRejectsEarlierRename", [](TestContext& context) {
			RejectGenericGroup(context, [](TransactionalMutationFixture&, TerraEngine::Scene::SceneMutationRequest& request) {
				request.payloadJson.clear(); request.usedJsonFallback = false;
				request.typedComponentPayload = {"nearPlane", 0, TerraEngine::Reflection::Variant{0.25F}, true};
			});
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericOrderedComponentEditsPreserveEarlierCandidates", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture(); fixture.objects.AddComponent<CameraComponent>(fixture.root);
			TerraEngine::Scene::SceneMutationQueue queue;
			auto first = CameraPropertyRequest(fixture.rootId);
			first.requestId = "first-camera-candidate"; first.sourceFrameOrder = 1;
			queue.Enqueue(first);
			auto second = first; second.requestId = "second-camera-candidate"; second.sourceFrameOrder = 2;
			second.payloadJson = nlohmann::json{{"propertyName", "farPlane"}, {"valueKind", 2}, {"value", 250.0F}}.dump();
			queue.Enqueue(second);
			const auto result = queue.Apply(fixture.objects);
			const auto* camera = fixture.objects.GetComponent<CameraComponent>(fixture.root);
			context.Expect(result.applied && result.acceptedMutationCount == 2U && camera->nearPlane == 0.25F && camera->farPlane == 250.0F,
				"The second detached candidate must retain the first property's authored value.");
			context.Expect(result.results.size() == 2U && result.results[0].nativeOperationBegin == 0U
				&& result.results[1].nativeOperationBegin == 1U && result.results[1].appliedWriteCount == 1U,
				"Original request spans and applied counts must survive native lowering.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericReservedCreateSupportsLaterAuthoredWrites", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture(); TerraEngine::Scene::SceneMutationQueue queue;
			const auto reservedId = TerraEngine::GenerateUUID();
			TerraEngine::Scene::SceneMutationRequest create;
			create.requestId = "reserved-create"; create.sourceFrameOrder = 1;
			create.operation = TerraEngine::Scene::SceneMutationOperation::CreateObject;
			create.targetObjectId = reservedId; create.displayName = "Reserved Object";
			queue.Enqueue(create);
			auto rename = RenameRequest(reservedId); rename.sourceFrameOrder = 2; queue.Enqueue(rename);
			const auto result = queue.Apply(fixture.objects);
			const auto object = fixture.objects.getObject(reservedId);
			context.Expect(result.applied && result.acceptedMutationCount == 2U && object
				&& fixture.objects.getObjectName(*object) == rename.metadataValue,
				"Later generic writes must resolve the earlier reserved creation identity.");
			context.Expect(result.results.size() == 2U && result.results[0].createdObjectId == reservedId,
				"Creation receipt must return the reserved stable UUID.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericUnsupportedInfoCannotClaimAnAppliedWrite", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture(); TerraEngine::Scene::SceneMutationQueue queue;
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "informational-prefab"; request.operation = TerraEngine::Scene::SceneMutationOperation::CreatePrefab;
			queue.Enqueue(request);
			const auto result = queue.Apply(fixture.objects);
			context.Expect(!result.applied && result.acceptedMutationCount == 0U && result.results.size() == 1U
				&& result.results[0].accepted && result.results[0].nativeOperationCount == 0U && result.results[0].appliedWriteCount == 0U,
				"Existing informational prefab outcome must not imply a newly authored operation committed.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericValidMixedGroupPublishesEveryRequest", [](TestContext& context) {
			auto fixture = MakeTransactionalMutationFixture();
			fixture.objects.AddComponent<CameraComponent>(fixture.root);
			(void)fixture.objects.PublishSceneChangePacket();
			TerraEngine::Scene::SceneMutationQueue queue;
			queue.Enqueue(RenameRequest(fixture.rootId));
			auto property = CameraPropertyRequest(fixture.rootId);
			property.payloadJson.clear(); property.usedJsonFallback = false;
			property.typedComponentPayload = {"nearPlane", 2, TerraEngine::Reflection::Variant{0.25F}, true};
			queue.Enqueue(std::move(property));
			TerraEngine::Scene::SceneMutationRequest script;
			script.requestId = "valid-generic-script-binding"; script.sourceFrameOrder = 3;
			script.operation = TerraEngine::Scene::SceneMutationOperation::AddScriptBinding;
			script.targetObjectId = fixture.rootId; script.metadataValue = "Fixture.ManagedBehavior";
			queue.Enqueue(script);
			const auto result = queue.Apply(fixture.objects, "Valid mixed generic group");
			context.Expect(result.applied && result.acceptedMutationCount == 3U && result.rejectedMutationCount == 0U,
				"A valid metadata/component/script group must publish all three requests truthfully.");
			context.Expect(result.results.size() == 3U, "Every valid request needs a receipt.");
			for (const auto& receipt : result.results) context.Expect(receipt.accepted, "Every committed mixed-group receipt must report acceptance.");
			context.Expect(fixture.objects.getObjectName(fixture.root) == "Earlier Rename Must Not Survive"
				&& fixture.objects.GetComponent<CameraComponent>(fixture.root)->nearPlane == 0.25F,
				"Valid mixed group must publish both metadata and component authoring.");
			const auto* scripts = fixture.objects.GetComponent<ScriptComponent>(fixture.root);
			context.Expect(scripts && scripts->HasBinding("Fixture.ManagedBehavior"), "Valid generic script binding must use the same committed group.");
			context.Expect(queue.Empty(), "Committed mixed generic cohort must not replay at a later boundary.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericSpatialReceiptsOwnOnlyTheirSpan", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture(); SceneMutationQueue queue;
			const Object other = fixture.objects.createObject("Other Spatial Target"); const auto otherId = fixture.objects.getUUID(other);
			queue.Enqueue(RenameRequest(fixture.rootId));
			for (const auto& id : {fixture.rootId, otherId}) {
				auto object = *fixture.objects.getObject(id); auto pose = *fixture.objects.GetCanonicalWorldPose(object);
				pose.position.offsetMetres.x = 7.0;
				SceneMutationRequest request; request.requestId = TerraEngine::ToHexString(id);
				request.operation = SceneMutationOperation::SetWorldTransform; request.targetObjectId = id;
				request.typedSpatialTransaction = TransactionalMutationBuilder(request.requestId, TransactionOrigin::Test)
					.SetWorldTransform(fixture.objects, id, pose).Build(); queue.Enqueue(request);
			}
			const auto result = queue.Apply(fixture.objects);
			context.Expect(result.acceptedMutationCount == 3U && result.results.size() == 3U, "Mixed cohort accounting counts its three original writes once.");
			for (const auto& id : {fixture.rootId, otherId}) {
				const auto receipt = std::find_if(result.transactionResults.begin(), result.transactionResults.end(), [&](const auto& value) { return value.transactionId == TerraEngine::ToHexString(id); });
				context.Expect(receipt != result.transactionResults.end() && receipt->status == TransactionStatus::Committed
					&& receipt->appliedWriteCount == 1U && receipt->rejectedWriteCount == 0U && receipt->inverseDeltas.size() == 1U,
					"Each spatial producer receives its one logical write and one inverse.");
				context.Expect(receipt->inverseDeltas.front().sourceOperationIndex == 0U
					&& receipt->inverseDeltas.front().operation.target.objectId == id && receipt->createdObjects.empty(),
					"Projected replay indices are local and cannot own metadata or another spatial producer.");
			}
		});
		registry.Add("Engine.SceneMutationFailureSafety.GenericFatalSpatialCountsRemainPerProducer", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture(); SceneMutationQueue queue;
			const Object other = fixture.objects.createObject("Other Fatal Target"); const auto otherId = fixture.objects.getUUID(other);
			queue.Enqueue(RenameRequest(fixture.rootId));
			for (const auto& id : {fixture.rootId, otherId}) {
				auto pose = *fixture.objects.GetCanonicalWorldPose(*fixture.objects.getObject(id)); pose.position.offsetMetres.x = 9.0;
				SceneMutationRequest request; request.requestId = TerraEngine::ToHexString(id);
				request.operation = SceneMutationOperation::SetWorldTransform; request.targetObjectId = id;
				request.typedSpatialTransaction = TransactionalMutationBuilder(request.requestId, TransactionOrigin::Test)
					.SetWorldTransform(fixture.objects, id, pose).Build(); queue.Enqueue(request);
			}
			struct ResetObserver { ObjectManager& objects; ~ResetObserver() { objects.SetOnModifiedCallback({}); } } reset{fixture.objects};
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Controlled projected receipt failure."); });
			bool fatal = false; try { (void)queue.Apply(fixture.objects); } catch (const TransactionPublicationFailure&) { fatal = true; }
			const auto diagnostics = queue.GetTransactionDiagnostics();
			for (const auto& id : {fixture.rootId, otherId}) {
				const auto receipt = std::find_if(diagnostics.recentResults.begin(), diagnostics.recentResults.end(), [&](const auto& value) { return value.transactionId == TerraEngine::ToHexString(id); });
				context.Expect(fatal && receipt != diagnostics.recentResults.end() && receipt->status == TransactionStatus::Committed
					&& receipt->appliedWriteCount == 1U, "Postcommit failure preserves the same per-producer count as normal publication.");
			}
			context.Expect(queue.Empty(), "Fatal committed envelopes cannot replay.");
		});
		registry.Add("Engine.SceneMutationFailureSafety.CanceledTypedPacketsCountEveryLogicalWrite", [](TestContext& context) {
			using namespace TerraEngine::Scene;
			auto fixture = MakeTransactionalMutationFixture(); SceneMutationQueue queue;
			const Object other = fixture.objects.createObject("Other Packet Target");
			const std::vector<TransactionTransformUpdate> updates{{fixture.rootId, *fixture.objects.GetCanonicalWorldPose(fixture.root)},
				{fixture.objects.getUUID(other), *fixture.objects.GetCanonicalWorldPose(other)}};
			TransactionPublicationGroup group; group.groupId = "canceled-packet-group"; group.canceled = true;
			group.chunks.push_back(TransactionalMutationBuilder("canceled-transform-packet", TransactionOrigin::Test).UpdateTransformBatch(fixture.objects, updates).Build());
			queue.EnqueuePublicationGroup(group);
			const auto batch = queue.Apply(fixture.objects);
			context.ExpectEqual(std::size_t{1U}, batch.publicationGroupResults.size(), "One canceled group must receive a terminal receipt.");
			const auto& canceledGroup = batch.publicationGroupResults.front();
			context.Expect(canceledGroup.status == TransactionStatus::Canceled, "Explicit group cancellation retains canceled status.");
			context.ExpectEqual(std::size_t{2U}, canceledGroup.rejectedWriteCount, "Canceled group counts every logical transform target.");
			context.ExpectEqual(std::size_t{2U}, canceledGroup.aggregateResult.rejectedWriteCount, "Canceled aggregate count matches the group.");
			context.ExpectEqual(std::size_t{1U}, canceledGroup.chunkReceipts.size(), "Canceled group settles every admitted chunk.");
			context.ExpectEqual(std::size_t{2U}, canceledGroup.chunkReceipts.front().writeCount, "Canceled chunk counts every logical transform target.");

			queue.EnqueueTransaction(TransactionalMutationBuilder("first-fatal-native", TransactionOrigin::Test).SetName(fixture.rootId, "Committed").Build());
			queue.EnqueueTransaction(TransactionalMutationBuilder("later-canceled-native", TransactionOrigin::Editor, "UndoRedoReplay").UpdateTransformBatch(fixture.objects, updates).Build());
			struct ResetObserver { ObjectManager& objects; ~ResetObserver() { objects.SetOnModifiedCallback({}); } } reset{fixture.objects};
			fixture.objects.SetOnModifiedCallback([] { throw std::runtime_error("Controlled native packet cancellation."); });
			try { (void)queue.Apply(fixture.objects); } catch (const TransactionPublicationFailure&) {}
			const auto canceled = queue.DrainCompletedEditorTransactions();
			context.Expect(canceled.size() == 1U && canceled.front().result.status == TransactionStatus::Canceled
				&& canceled.front().result.rejectedWriteCount == 2U, "Fatal remainder cancellation uses the same logical packet count.");
		});

	}
}