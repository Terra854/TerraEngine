// SceneMutationQueue.cpp - validates and applies queued scene mutations.

#include "SceneMutationQueue.h"

#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"

#include <algorithm>
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
		if (request.requestId.empty())
		{
			request.requestId = "mutation-" + std::to_string(requests.size());
		}
		requests.push_back(std::move(request));
	}

	bool SceneMutationQueue::Empty() const
	{
		return requests.empty();
	}

	std::size_t SceneMutationQueue::Size() const
	{
		return requests.size();
	}

	std::vector<SceneMutationRequest> SceneMutationQueue::PendingRequests() const
	{
		// Return a snapshot instead of exposing the internal vector so scheduler
		// diagnostics and tests can inspect pending requests without mutating the
		// queue that frame systems are still relying on.
		return requests;
	}

	void SceneMutationQueue::Clear()
	{
		requests.clear();
	}

	SceneMutationBatchResult SceneMutationQueue::Validate(const SceneGraph& graph) const
	{
		SceneMutationBatchResult batch;
		batch.applied = false;
		std::unordered_set<TerraEngine::UUID> deletedTargets;

		for (const SceneMutationRequest& request : requests)
		{
			SceneMutationResult result;
			result.requestId = request.requestId;
			result.accepted = true;

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
		SceneGraph graph = BuildSceneGraphFromObjectManager(objects);
		SceneMutationBatchResult batch = Validate(graph);
		const bool hasRejectedRequest = std::any_of(batch.results.begin(), batch.results.end(), [](const SceneMutationResult& result) {
			return !result.accepted;
		});
		if (hasRejectedRequest)
		{
			return batch;
		}

		for (std::size_t index = 0; index < requests.size(); ++index)
		{
			const SceneMutationRequest& request = requests[index];
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
			default:
				result.diagnostics.push_back(MakeMutationDiagnostic(SceneDiagnosticSeverity::Info, request, "Mutation operation recorded but has no direct ObjectManager side effect yet.", "Prefab and component payloads are handled by their owning systems."));
				break;
			}
		}

		batch.applied = true;
		Clear();
		return batch;
	}
}
