// SceneMutationQueue.cpp - validates and applies queued scene mutations.

#include "SceneMutationQueue.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <mutex>
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
			case SceneMutationOperation::AddComponent:
			case SceneMutationOperation::RemoveComponent:
			case SceneMutationOperation::UpdateComponent:
			case SceneMutationOperation::ApplyPrefabDefaults:
			case SceneMutationOperation::ResetPrefabDefaults:
				return true;
			default:
				return false;
			}
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

		bool ApplyComponentPropertyPayload(Object& target, const ComponentAccessFunctions& component, const SceneMutationRequest& request)
		{
			if (request.typedComponentPayload.valid)
			{
				return component.setter(target, request.typedComponentPayload.propertyName, request.typedComponentPayload.value);
			}

			const nlohmann::json payload = nlohmann::json::parse(request.payloadJson);
			const std::string propertyName = payload.value("propertyName", std::string{});
			const int valueKind = payload.value("valueKind", -1);
			if (propertyName.empty() || !payload.contains("value"))
			{
				return false;
			}

			TerraEngine::Reflection::Variant value = MakeVariantFromMutationPayload(payload["value"], valueKind);
			return component.setter(target, propertyName, value);
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

	bool SceneMutationQueue::Empty() const
	{
		std::scoped_lock lock(requestsMutex);
		return requests.empty();
	}

	std::size_t SceneMutationQueue::Size() const
	{
		std::scoped_lock lock(requestsMutex);
		return requests.size();
	}

	std::vector<SceneMutationRequest> SceneMutationQueue::PendingRequests() const
	{
		// Return a snapshot instead of exposing the internal vector so scheduler
		// diagnostics and tests can inspect pending requests without mutating the
		// queue that frame systems are still relying on.
		std::scoped_lock lock(requestsMutex);
		std::vector<SceneMutationRequest> snapshot = requests;
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

	void SceneMutationQueue::Clear()
	{
		std::scoped_lock lock(requestsMutex);
		requests.clear();
	}

	SceneMutationBatchResult SceneMutationQueue::Validate(const SceneGraph& graph) const
	{
		SceneMutationBatchResult batch;
		batch.applied = false;
		std::unordered_set<TerraEngine::UUID> deletedTargets;
		const std::size_t rawRequestCount = Size();
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
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

			if (request.operation == SceneMutationOperation::DeleteObject && result.accepted)
			{
				deletedTargets.insert(request.targetObjectId);
			}

			if (!result.accepted)
			{
				batch.diagnostics.insert(batch.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
			}
			batch.results.push_back(std::move(result));
		}

		return batch;
	}

	SceneMutationBatchResult SceneMutationQueue::Apply(ObjectManager& objects)
	{
		const std::size_t rawRequestCount = Size();
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
		SceneGraph graph = BuildSceneGraphFromObjectManager(objects);
		SceneMutationBatchResult batch = Validate(graph);
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
			return batch;
		}

		for (std::size_t index = 0; index < requestSnapshot.size(); ++index)
		{
			const SceneMutationRequest& request = requestSnapshot[index];
			SceneMutationResult& result = batch.results[index];

			switch (request.operation)
			{
			case SceneMutationOperation::CreateObject:
			{
				Object created = objects.createObject(request.displayName.empty() ? "Object" : request.displayName);
				result.createdObjectId = objects.getUUID(created.EntityId());
				if (request.parentObjectId != TerraEngine::EMPTY_UUID)
				{
					if (auto parent = objects.getObject(request.parentObjectId))
					{
						objects.setParent(created, *parent);
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
			case SceneMutationOperation::AddComponent:
				if (auto target = objects.getObject(request.targetObjectId))
				{
					std::lock_guard registryLock(ComponentAccessRegistryMutex());
					auto& registry = ComponentAccessRegistry();
					if (auto component = registry.find(request.componentName); component != registry.end())
					{
						if (!component->second.hasComponent(*target))
						{
							component->second.addComponent(*target);
							objects.notifyObjectStructureChanged(*target);
						}
					}
					else
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component mutation references an unknown component type.", "Register the component in ComponentAccessRegistry before scripts can add it."));
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

						if (!ApplyComponentPropertyPayload(*target, component->second, request))
						{
							result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component update could not write the reflected property.", "Verify the property name and value type match the component reflection metadata."));
						}
						objects.notifyObjectStructureChanged(*target);
					}
					catch (const std::exception& exception)
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Error, request, std::string("Component update payload could not be parsed: ") + exception.what(), "Rebuild the script mutation payload from supported property types."));
					}
				}
				break;
			default:
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Info, request, "Mutation operation recorded but has no direct ObjectManager side effect yet.", "Prefab and component payloads are handled by their owning systems at the approved flush point."));
				break;
			}
		}

		batch.applied = true;
		{
			std::scoped_lock lock(requestsMutex);
			const std::size_t removeCount = std::min(rawRequestCount, requests.size());
			requests.erase(requests.begin(), requests.begin() + static_cast<std::ptrdiff_t>(removeCount));
		}
		return batch;
	}
}
