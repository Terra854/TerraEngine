// SceneMutationQueue.h - transaction-style structural scene mutation requests.
//
// Callers enqueue changes while systems are updating; the queue validates the
// whole batch before applying anything so unrelated objects stay unchanged when
// one request is stale or unsafe.

#pragma once

#include "Core/Export.h"
#include "SceneGraphRuntime.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {

	enum class SceneMutationOperation {
		CreateObject,
		DeleteObject,
		ReparentObject,
		AddComponent,
		RemoveComponent,
		UpdateComponent,
		CreatePrefab,
		DeletePrefab,
		ApplyPrefabDefaults,
		ResetPrefabDefaults
	};

	enum class SceneComponentMutationPayloadKind {
		None,
		Add,
		Remove,
		Replace
	};

	struct SceneMutationRequest {
		std::string requestId;
		SceneMutationOperation operation = SceneMutationOperation::CreateObject;
		TerraEngine::UUID targetObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::string displayName;
		std::string componentName;
		std::string payloadJson;
		SceneComponentMutationPayloadKind componentPayloadKind = SceneComponentMutationPayloadKind::None;
		std::uint64_t expectedGraphRevision = 0;
	};

	struct SceneMutationResult {
		std::string requestId;
		bool accepted = false;
		TerraEngine::UUID createdObjectId = TerraEngine::EMPTY_UUID;
		std::vector<SceneDiagnostic> diagnostics;
	};

	struct SceneMutationBatchResult {
		bool applied = false;
		std::vector<SceneMutationResult> results;
		std::vector<SceneDiagnostic> diagnostics;
	};

	class TERRAENGINE_API SceneMutationQueue {
	public:
		static SceneMutationQueue& Instance();

		void Enqueue(SceneMutationRequest request);
		bool Empty() const;
		std::size_t Size() const;
		std::vector<SceneMutationRequest> PendingRequests() const;
		void Clear();
		SceneMutationBatchResult Validate(const SceneGraph& graph) const;
		SceneMutationBatchResult Apply(ObjectManager& objects);

	private:
		std::vector<SceneMutationRequest> requests;
	};

}
