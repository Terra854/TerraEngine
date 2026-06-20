// SceneMutationQueue.cpp - validates and applies queued scene mutations.

#include "SceneMutationQueue.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/Component.h"

#include <algorithm>
#include <cstddef>
#include <mutex>
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
		return requests;
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
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();

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
		const std::vector<SceneMutationRequest> requestSnapshot = PendingRequests();
		SceneGraph graph = BuildSceneGraphFromObjectManager(objects);
		SceneMutationBatchResult batch = Validate(graph);
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
						}
					}
					else
					{
						result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Warning, request, "Component mutation references an unknown component type.", "Register the component in ComponentAccessRegistry before scripts can add it."));
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
			const std::size_t removeCount = std::min(requestSnapshot.size(), requests.size());
			requests.erase(requests.begin(), requests.begin() + static_cast<std::ptrdiff_t>(removeCount));
		}
		return batch;
	}
}
