// SceneMutationQueue.cpp - validates and applies queued scene mutations.

#include "SceneMutationQueue.h"

#include "Animation/Runtime/RootMotion.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"
#include "Components/Transform.h"
#include "Components/ScriptComponent.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		SceneDiagnostic MakeMutationDiagnostic(SceneDiagnosticSeverity severity, const SceneMutationRequest& request, std::string message, std::string action)
		{
			return SceneDiagnostic{
				severity,
				SceneDiagnosticCategory::Mutation,
				request.targetObjectId,
				request.requestId,
				std::move(message),
				std::move(action)
			};
		}

		bool NeedsExistingTarget(SceneMutationOperation operation)
		{
			switch (operation)
			{
			case SceneMutationOperation::DeleteObject:
			case SceneMutationOperation::ReparentObject:
			case SceneMutationOperation::SetObjectName:
			case SceneMutationOperation::SetObjectTag:
			case SceneMutationOperation::SetObjectLayer:
			case SceneMutationOperation::SetObjectActive:
			case SceneMutationOperation::SetObjectStatic:
			case SceneMutationOperation::AddComponent:
			case SceneMutationOperation::RemoveComponent:
			case SceneMutationOperation::UpdateComponent:
			case SceneMutationOperation::AddScriptBinding:
			case SceneMutationOperation::ApplyPrefabDefaults:
			case SceneMutationOperation::ResetPrefabDefaults:
				return true;
			default:
				return false;
			}
		}

		bool IsSpatialMutation(SceneMutationOperation operation)
		{
			return operation == SceneMutationOperation::SetWorldTransform
				|| operation == SceneMutationOperation::TranslateWorld
				|| operation == SceneMutationOperation::ChangeSimulationFrame;
		}

		TerraEngine::Reflection::Variant MakeVariantFromMutationPayload(const nlohmann::json& value, int valueKind)
		{
			const auto readFloat = [&value](std::size_t index, float fallback) {
				return value.is_array() && index < value.size()
					? value.at(index).get<float>()
					: fallback;
			};

			switch (valueKind)
			{
			case 0: return value.get<bool>();
			case 1: return value.get<int>();
			case 2: return value.get<float>();
			case 3: return value.get<std::uint32_t>();
			case 4:
				return glm::vec2(readFloat(0, 0.0f), readFloat(1, 0.0f));
			case 5:
				return glm::vec3(readFloat(0, 0.0f), readFloat(1, 0.0f), readFloat(2, 0.0f));
			case 6:
				return glm::vec4(readFloat(0, 0.0f), readFloat(1, 0.0f), readFloat(2, 0.0f), readFloat(3, 0.0f));
			case 7:
				return glm::quat(readFloat(0, 1.0f), readFloat(1, 0.0f), readFloat(2, 0.0f), readFloat(3, 0.0f));
			default:
				return {};
			}
		}

		bool ApplyComponentPropertyPayload(ObjectManager& objects, entt::entity target, const ComponentAccessFunctions& component, const SceneMutationRequest& request)
		{
			entt::registry& registry = objects.getRegistry();
			if (request.typedComponentPayload.valid)
			{
				return component.entitySetter(registry, target, request.typedComponentPayload.propertyName, request.typedComponentPayload.value);
			}

			const nlohmann::json payload = nlohmann::json::parse(request.payloadJson);
			const std::string propertyName = payload.value("propertyName", std::string{});
			const int valueKind = payload.value("valueKind", -1);
			if (propertyName.empty() || !payload.contains("value"))
			{
				return false;
			}

			TerraEngine::Reflection::Variant value = MakeVariantFromMutationPayload(payload["value"], valueKind);
			return component.entitySetter(registry, target, propertyName, value);
		}

		std::string FormatCommitSummary(const SceneMutationBatchResult& batch)
		{
			std::ostringstream stream;
			stream << "Scene mutation commit '" << batch.commitName << "':"
				<< " applied=" << (batch.applied ? "true" : "false")
				<< " input=" << batch.inputMutationCount
				<< " accepted=" << batch.acceptedMutationCount
				<< " rejected=" << batch.rejectedMutationCount
				<< " typed=" << batch.typedMutationCount
				<< " jsonFallbacks=" << batch.jsonMutationFallbackCount
				<< " coalesced=" << batch.coalescedMutationCount;
			return stream.str();
		}
	}

	SceneMutationQueue& SceneMutationQueue::Instance()
	{
		static SceneMutationQueue queue;
		return queue;
	}

	void SceneMutationQueue::Enqueue(SceneMutationRequest request)
	{
		std::scoped_lock lock(requestsMutex);
		if (request.requestId.empty())
		{
			request.requestId = "mutation-" + std::to_string(requests.size());
		}
		requests.push_back(std::move(request));
	}

	void SceneMutationQueue::EnqueueFromWorker(std::uint32_t workerIndex, SceneMutationRequest request)
	{
		std::scoped_lock lock(requestsMutex);
		if (request.requestId.empty())
		{
			request.requestId = "worker-" + std::to_string(workerIndex) + "-mutation-" + std::to_string(workerBuffers.size());
		}
		auto buffer = std::find_if(workerBuffers.begin(), workerBuffers.end(), [workerIndex](const SceneMutationWorkerBuffer& candidate) {
			return candidate.workerIndex == workerIndex;
		});
		if (buffer == workerBuffers.end())
		{
			SceneMutationWorkerBuffer newBuffer;
			newBuffer.workerIndex = workerIndex;
			newBuffer.requests.push_back(std::move(request));
			workerBuffers.push_back(std::move(newBuffer));
			return;
		}
		buffer->requests.push_back(std::move(request));
	}

	void SceneMutationQueue::EnqueueTransaction(Transaction transaction)
	{
		std::scoped_lock lock(requestsMutex);
		if (transaction.id.empty())
		{
			transaction.id = "transaction-" + std::to_string(transactions.size());
		}
		transactions.push_back(std::move(transaction));
	}

	void SceneMutationQueue::EnqueuePublicationGroup(TransactionPublicationGroup group)
	{
		std::scoped_lock lock(requestsMutex);
		if (group.groupId.empty())
		{
			group.groupId = "publication-group-" + std::to_string(publicationGroups.size());
		}
		publicationGroups.push_back(std::move(group));
	}

	TransactionResult SceneMutationQueue::PreflightTransaction(const Transaction& transaction, ObjectManager& objects) const
	{
		return TerraEngine::Scene::PreflightTransaction(objects, transaction);
	}

	bool SceneMutationQueue::CancelTransaction(const std::string& transactionId)
	{
		std::scoped_lock lock(requestsMutex);
		for (Transaction& transaction : transactions)
		{
			if (transaction.id == transactionId)
			{
				transaction.canceled = true;
				return true;
			}
		}
		return false;
	}

	bool SceneMutationQueue::CancelPublicationGroup(const std::string& groupId)
	{
		std::scoped_lock lock(requestsMutex);
		for (TransactionPublicationGroup& group : publicationGroups)
		{
			if (group.groupId == groupId)
			{
				group.canceled = true;
				return true;
			}
		}
		return false;
	}

	bool SceneMutationQueue::Empty() const
	{
		std::scoped_lock lock(requestsMutex);
		return requests.empty() && transactions.empty() && publicationGroups.empty() && std::all_of(workerBuffers.begin(), workerBuffers.end(), [](const SceneMutationWorkerBuffer& buffer) {
			return buffer.requests.empty();
		});
	}

	std::size_t SceneMutationQueue::Size() const
	{
		std::scoped_lock lock(requestsMutex);
		std::size_t total = requests.size();
		for (const SceneMutationWorkerBuffer& buffer : workerBuffers)
		{
			total += buffer.requests.size();
		}
		total += transactions.size();
		total += publicationGroups.size();
		return total;
	}

	std::vector<SceneMutationRequest> SceneMutationQueue::PendingRequests() const
	{
		// Return a snapshot instead of exposing the internal vector so scheduler
		// diagnostics and tests can inspect pending requests without mutating the
		// queue that frame systems are still relying on.
		std::scoped_lock lock(requestsMutex);
		std::vector<SceneMutationRequest> snapshot = requests;
		std::vector<SceneMutationWorkerBuffer> sortedBuffers = workerBuffers;
		std::stable_sort(sortedBuffers.begin(), sortedBuffers.end(), [](const SceneMutationWorkerBuffer& left, const SceneMutationWorkerBuffer& right) {
			return left.workerIndex < right.workerIndex;
		});
		for (SceneMutationWorkerBuffer& buffer : sortedBuffers)
		{
			std::stable_sort(buffer.requests.begin(), buffer.requests.end(), [](const SceneMutationRequest& left, const SceneMutationRequest& right) {
				if (left.sourceFrameOrder != right.sourceFrameOrder)
				{
					return left.sourceFrameOrder < right.sourceFrameOrder;
				}
				return left.requestId < right.requestId;
			});
			snapshot.insert(snapshot.end(), buffer.requests.begin(), buffer.requests.end());
		}
		std::sort(snapshot.begin(), snapshot.end(), [](const SceneMutationRequest& left, const SceneMutationRequest& right) {
			if (left.sourceFrameOrder != right.sourceFrameOrder)
			{
				return left.sourceFrameOrder < right.sourceFrameOrder;
			}
			return left.requestId < right.requestId;
		});

		std::vector<SceneMutationRequest> coalesced;
		coalesced.reserve(snapshot.size());
		std::unordered_map<std::string, std::size_t> coalescedIndex;
		for (SceneMutationRequest& request : snapshot)
		{
			if (request.coalescingKey.empty())
			{
				coalesced.push_back(std::move(request));
				continue;
			}

			if (auto existing = coalescedIndex.find(request.coalescingKey); existing != coalescedIndex.end())
			{
				coalesced[existing->second] = std::move(request);
				continue;
			}

			coalescedIndex.emplace(request.coalescingKey, coalesced.size());
			coalesced.push_back(std::move(request));
		}
		return coalesced;
	}

	std::vector<Transaction> SceneMutationQueue::PendingTransactions() const
	{
		std::scoped_lock lock(requestsMutex);
		return transactions;
	}

	std::vector<TransactionPublicationGroup> SceneMutationQueue::PendingPublicationGroups() const
	{
		std::scoped_lock lock(requestsMutex);
		return publicationGroups;
	}

	std::vector<TransactionResult> SceneMutationQueue::DrainCompletedEditorTransactionResults()
	{
		std::scoped_lock lock(requestsMutex);
		std::vector<TransactionResult> completed;
		completed.reserve(completedEditorTransactions.size());
		for (EditorTransactionCompletion& completion : completedEditorTransactions)
		{
			completed.push_back(std::move(completion.result));
		}
		completedEditorTransactions.clear();
		return completed;
	}

	std::vector<EditorTransactionCompletion> SceneMutationQueue::DrainCompletedEditorTransactions()
	{
		std::scoped_lock lock(requestsMutex);
		std::vector<EditorTransactionCompletion> completed;
		completed.swap(completedEditorTransactions);
		return completed;
	}

	TransactionDiagnostics SceneMutationQueue::GetTransactionDiagnostics() const
	{
		std::scoped_lock lock(requestsMutex);
		return transactionDiagnostics;
	}

	void SceneMutationQueue::Clear()
	{
		std::scoped_lock lock(requestsMutex);
		requests.clear();
		workerBuffers.clear();
		transactions.clear();
		publicationGroups.clear();
		completedEditorTransactions.clear();
	}

	SceneMutationBatchResult SceneMutationQueue::Validate(const SceneGraph& graph) const
	{
		SceneMutationBatchResult batch;
		batch.applied = false;
		std::unordered_set<TerraEngine::UUID> deletedTargets;
		const std::size_t rawRequestCount = Size();
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
		batch.inputMutationCount = requestSnapshot.size();
		batch.coalescedMutationCount = rawRequestCount > requestSnapshot.size() ? rawRequestCount - requestSnapshot.size() : 0;

		for (const SceneMutationRequest& request : requestSnapshot)
		{
			SceneMutationResult result;
			result.requestId = request.requestId;
			result.accepted = true;
			result.sourceObjectId = request.sourceObjectId;
			result.sourceObjectUuid = request.sourceObjectUuid;
			result.sourceBindingId = request.sourceBindingId;
			result.sourceFrameOrder = request.sourceFrameOrder;

			if (request.expectedGraphRevision != 0 && request.expectedGraphRevision != graph.revision)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation was captured against a stale scene graph revision.", "Refresh the request from the latest graph before applying it."));
			}

			if (NeedsExistingTarget(request.operation) && graph.objects.find(request.targetObjectId) == graph.objects.end())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation target no longer exists.", "Drop or rebuild the request from current selection state."));
			}

			// Deferred managed creation reserves its stable UUID before publication.
			// Reject collisions during preflight so the entire request batch remains
			// atomic and managed receipts never resolve to an unrelated live object.
			if (request.operation == SceneMutationOperation::CreateObject
				&& request.targetObjectId != TerraEngine::EMPTY_UUID
				&& graph.objects.find(request.targetObjectId) != graph.objects.end())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Create mutation reserved an existing object UUID.", "Generate a fresh stable UUID before retrying the request."));
			}

			if (deletedTargets.contains(request.targetObjectId))
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Mutation targets an object already deleted earlier in the batch.", "Split the batch or remove the later request."));
			}

			if (request.operation == SceneMutationOperation::ReparentObject && request.parentObjectId == request.targetObjectId)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Object cannot be parented to itself.", "Choose a different parent or detach to root."));
			}

			if ((request.operation == SceneMutationOperation::AddComponent
				|| request.operation == SceneMutationOperation::RemoveComponent
				|| request.operation == SceneMutationOperation::UpdateComponent)
				&& request.componentName.empty())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Component mutation is missing a component name.", "Set componentName so the owning system can apply the payload deterministically."));
			}

			if (IsSpatialMutation(request.operation) && !request.typedSpatialTransaction)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request,
					"Spatial mutation ingress must be lowered to a typed transaction before it reaches the queue.",
					"Build SetWorldTransform, TranslateWorld, ReparentObject, or ChangeSimulationFrame through TransactionalMutationBuilder."));
			}

			if (request.operation == SceneMutationOperation::UpdateComponent
				&& request.componentName == Transform::kComponentName)
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request,
					"Generic reflected Transform mutations are not a spatial authority.",
					"Lower canonical movement to a typed world-spatial transaction instead of supplying a Transform property payload."));
			}

			if (request.operation == SceneMutationOperation::AddScriptBinding && request.metadataValue.empty())
			{
				result.accepted = false;
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Script binding mutation is missing a managed class name.", "Provide the fully qualified managed script type name."));
			}

			if (request.operation == SceneMutationOperation::DeleteObject && result.accepted)
			{
				deletedTargets.insert(request.targetObjectId);
			}

			if (!result.accepted)
			{
				++batch.rejectedMutationCount;
				batch.diagnostics.insert(batch.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
			}
			else
			{
				++batch.acceptedMutationCount;
			}
			batch.results.push_back(std::move(result));
		}

		batch.formattedCommitSummary = FormatCommitSummary(batch);
		return batch;
	}

	SceneMutationBatchResult SceneMutationQueue::Apply(ObjectManager& objects)
	{
		return Apply(objects, "SceneMutationQueue.Apply");
	}

	SceneMutationBatchResult SceneMutationQueue::Apply(ObjectManager& objects, std::string commitName)
	{
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
		SceneMutationBatchResult batch;
		if (!requestSnapshot.empty())
		{
			// Request mutations need a graph-wide preflight. Typed transactions carry
			// their own read sets, so avoiding this traversal keeps a physics-only
			// After Physics flush proportional to the bodies that actually moved.
			SceneGraph graph = BuildSceneGraphFromObjectManager(objects);
			batch = Validate(graph);
		}
		batch.commitName = commitName.empty() ? "SceneMutationQueue.Apply" : std::move(commitName);
		for (const SceneMutationRequest& request : requestSnapshot)
		{
			if (request.operation == SceneMutationOperation::UpdateComponent)
			{
				if (request.typedComponentPayload.valid)
				{
					++batch.typedMutationCount;
				}
				else if (!request.payloadJson.empty() || request.usedJsonFallback)
				{
					++batch.jsonMutationFallbackCount;
				}
			}
		}
		const bool hasRejectedRequest = std::any_of(batch.results.begin(), batch.results.end(), [](const SceneMutationResult& result) {
			return !result.accepted;
		});
		if (hasRejectedRequest)
		{
			batch.formattedCommitSummary = FormatCommitSummary(batch);
			return batch;
		}

		for (std::size_t index = 0; index < requestSnapshot.size(); ++index)
		{
			const SceneMutationRequest& request = requestSnapshot[index];
			SceneMutationResult& result = batch.results[index];
			if (IsSpatialMutation(request.operation))
			{
				// Generic ingress has completed validation. The only publication route
				// from here is the typed STM, which owns observations and rollback.
				const TransactionResult typedResult = CommitTransaction(objects, *request.typedSpatialTransaction);
				{
					std::scoped_lock lock(requestsMutex);
					AccumulateTransactionDiagnostics(transactionDiagnostics, *request.typedSpatialTransaction, typedResult);
				}
				batch.transactionResults.push_back(typedResult);
				if (typedResult.status == TransactionStatus::Committed)
				{
					batch.acceptedMutationCount += typedResult.appliedWriteCount;
					batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(),
						typedResult.acceptedAnimationPublications.begin(), typedResult.acceptedAnimationPublications.end());
				}
				else
				{
					batch.rejectedMutationCount += typedResult.rejectedWriteCount;
				}
				continue;
			}

			switch (request.operation)
			{
			case SceneMutationOperation::CreateObject:
			{
				Object created = objects.createObject(request.displayName.empty() ? "Object" : request.displayName);
				if (request.targetObjectId != TerraEngine::EMPTY_UUID)
				{
					objects.UpdateObjectUUID(created, request.targetObjectId);
				}
				result.createdObjectId = objects.getUUID(created);
				if (request.parentObjectId != TerraEngine::EMPTY_UUID)
				{
					if (auto parent = objects.getObject(request.parentObjectId))
					{
						objects.setParent(created, parent);
					}
				}
				break;
			}
			case SceneMutationOperation::DeleteObject:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.removeObject(*target);
				}
				break;
			case SceneMutationOperation::ReparentObject:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					std::optional<Object> parent;
					if (request.parentObjectId != TerraEngine::EMPTY_UUID)
					{
						parent = objects.getObject(request.parentObjectId);
					}
					objects.setParent(*target, parent);
				}
				break;
			case SceneMutationOperation::SetObjectName:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.setObjectName(*target, request.metadataValue);
				}
				break;
			case SceneMutationOperation::SetObjectTag:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.setObjectTag(*target, request.metadataValue);
				}
				break;
			case SceneMutationOperation::SetObjectLayer:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.setObjectLayer(*target, request.metadataValue);
				}
				break;
			case SceneMutationOperation::SetObjectActive:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.setObjectActive(*target, request.metadataBooleanValue);
				}
				break;
			case SceneMutationOperation::SetObjectStatic:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					objects.setObjectStatic(*target, request.metadataBooleanValue);
				}
				break;
			case SceneMutationOperation::AddComponent:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto& registry = ComponentAccessRegistry();
					if (auto component = registry.find(request.componentName); component != registry.end())
					{
						entt::registry& enttRegistry = objects.getRegistry();
						const entt::entity targetEntity = target->EntityId();
						if (!component->second.entityHasComponent(enttRegistry, targetEntity))
						{
							component->second.entityAddComponent(enttRegistry, targetEntity);
							objects.notifyObjectStructureChanged(*target);
						}
					}
					else
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component mutation references an unknown component type.", "Register the component in ComponentAccessRegistry before scripts can add it."));
					}
				}
				break;
			case SceneMutationOperation::RemoveComponent:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto& registry = ComponentAccessRegistry();
					if (auto component = registry.find(request.componentName); component != registry.end())
					{
						entt::registry& enttRegistry = objects.getRegistry();
						const entt::entity targetEntity = target->EntityId();
						if (component->second.entityHasComponent(enttRegistry, targetEntity))
						{
							component->second.entityRemoveComponent(enttRegistry, targetEntity);
							objects.notifyObjectStructureChanged(*target);
						}
					}
					else
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component removal references an unknown component type.", "Register the component in ComponentAccessRegistry before scripts can remove it."));
					}
				}
				break;
			case SceneMutationOperation::UpdateComponent:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto& registry = ComponentAccessRegistry();
					auto component = registry.find(request.componentName);
					if (component == registry.end())
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component update references an unknown component type.", "Register the component in ComponentAccessRegistry before scripts can update it."));
						break;
					}

					try
					{
						const bool hasPayload = request.typedComponentPayload.valid || !request.payloadJson.empty();
						if (!hasPayload)
						{
							result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Component update payload is missing property data.", "Include propertyName, valueKind, and value in the deferred payload."));
							break;
						}

						if (!ApplyComponentPropertyPayload(objects, target->EntityId(), component->second, request))
						{
							result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component update could not write the reflected property.", "Verify the property name and value type match the component reflection metadata."));
						}
						if (request.componentName != Transform::kComponentName)
						{
							// Reflected Transform setters publish their spatial
							// revision centrally; other component edits still
							// invalidate the object's structural snapshot here.
							objects.notifyObjectStructureChanged(*target);
						}
					}
					catch (const std::exception& exception)
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, std::string("Component update payload could not be parsed: ") + exception.what(), "Rebuild the script mutation payload from supported property types."));
					}
				}
				break;
			case SceneMutationOperation::AddScriptBinding:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					ScriptComponent* scripts = objects.GetComponent<ScriptComponent>(*target);
					if (scripts == nullptr)
					{
						scripts = objects.AddComponent<ScriptComponent>(*target);
					}
					if (scripts == nullptr)
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, "Script component could not be created.", "Retry after refreshing the target object."));
						break;
					}
					auto guard = scripts->acquireWriteLock();
					if (!scripts->HasBinding(request.metadataValue))
					{
						scripts->AddBinding(TerraEngine::EMPTY_UUID, request.metadataValue);
						objects.notifyObjectStructureChanged(*target);
					}
				}
				break;
			default:
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Info, request, "Mutation operation recorded but has no direct ObjectManager side effect yet.", "Prefab and component payloads are handled by their owning systems at the approved flush point."));
				break;
			}
		}

		// A protected boundary with no accepted request/transaction/group is a
		// no-op, not a successful publication. Later receipts may set this true.
		batch.applied = !requestSnapshot.empty();
		const std::vector<TransactionResult> transactionResults = DrainTransactions(objects);
		batch.transactionResults.insert(batch.transactionResults.end(), transactionResults.begin(), transactionResults.end());
		for (const TransactionResult& transactionResult : transactionResults)
		{
			// Unknown transaction IDs are ignored by the animation publisher. Known
			// root-motion IDs become observable only at this protected scene flush.
			TerraEngine::Animation::AnimationRootMotionResultPublisher::Instance()
				.ObserveSceneTransactionResult(transactionResult);
			if (transactionResult.status == TransactionStatus::Committed)
			{
				batch.applied = true;
				batch.acceptedMutationCount += transactionResult.appliedWriteCount;
				batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(), transactionResult.acceptedAnimationPublications.begin(), transactionResult.acceptedAnimationPublications.end());
			}
			else
			{
				batch.rejectedMutationCount += transactionResult.rejectedWriteCount;
			}
		}
		const std::vector<TransactionPublicationGroupResult> publicationGroupResults = DrainPublicationGroups(objects);
		batch.publicationGroupResults = publicationGroupResults;
		for (const TransactionPublicationGroupResult& groupResult : publicationGroupResults)
		{
			if (groupResult.status == TransactionStatus::Committed)
			{
				batch.applied = true;
				batch.acceptedMutationCount += groupResult.appliedWriteCount;
				batch.acceptedAnimationPublications.insert(batch.acceptedAnimationPublications.end(), groupResult.aggregateResult.acceptedAnimationPublications.begin(), groupResult.aggregateResult.acceptedAnimationPublications.end());
			}
			else
			{
				batch.rejectedMutationCount += groupResult.rejectedWriteCount;
			}
		}
		{
			std::scoped_lock lock(requestsMutex);
			requests.clear();
			workerBuffers.clear();
		}
		batch.formattedCommitSummary = FormatCommitSummary(batch);
		return batch;
	}

	std::vector<TransactionResult> SceneMutationQueue::DrainTransactions(ObjectManager& objects)
	{
		std::vector<Transaction> pending;
		{
			std::scoped_lock lock(requestsMutex);
			pending.swap(transactions);
		}
		std::vector<TransactionResult> results;
		results.reserve(pending.size());
		for (const Transaction& transaction : pending)
		{
			TransactionResult result = CommitTransaction(objects, transaction);
			{
				std::scoped_lock lock(requestsMutex);
				AccumulateTransactionDiagnostics(transactionDiagnostics, transaction, result);
				if (transaction.origin == TransactionOrigin::Editor
					&& (!transaction.undoGroupName.empty() || transaction.originName == "UndoRedoReplay"))
				{
					// Keep one complete receipt until MainEditorSubsystem records a new
					// command or reconciles a deferred undo/redo replay. The bounded
					// diagnostics copy cannot carry the inverse payload needed by history.
					completedEditorTransactions.push_back({transaction, result});
				}
			}
			results.push_back(std::move(result));
		}
		return results;
	}

	std::vector<TransactionPublicationGroupResult> SceneMutationQueue::DrainPublicationGroups(ObjectManager& objects)
	{
		std::vector<TransactionPublicationGroup> pending;
		{
			std::scoped_lock lock(requestsMutex);
			pending.swap(publicationGroups);
		}
		std::vector<TransactionPublicationGroupResult> results;
		results.reserve(pending.size());
		for (const TransactionPublicationGroup& group : pending)
		{
			results.push_back(CommitTransactionPublicationGroup(objects, group));
		}
		return results;
	}
}
