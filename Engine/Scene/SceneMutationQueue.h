// SceneMutationQueue.h - transaction-style structural scene mutation requests.
//
// Callers enqueue changes while systems are updating; the queue validates the
// whole batch before applying anything so unrelated objects stay unchanged when
// one request is stale or unsafe.

#pragma once

#include "Core/Export.h"
#include "Core/Object/Reflection.h"
#include "SceneGraphRuntime.h"
#include "TransactionalMutation.h"
#include "Utilities/UUID.h"

#include <cstdint>
#include <mutex>
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

	struct SceneComponentPropertyMutation {
		std::string propertyName;
		int valueKind = -1;
		TerraEngine::Reflection::Variant value;
		bool valid = false;
	};

	struct SceneMutationRequest {
		std::string requestId;
		SceneMutationOperation operation = SceneMutationOperation::CreateObject;
		TerraEngine::UUID targetObjectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::string displayName;
		std::string componentName;
		std::string payloadJson;
		SceneComponentPropertyMutation typedComponentPayload;
		std::string coalescingKey;
		bool usedJsonFallback = false;
		SceneComponentMutationPayloadKind componentPayloadKind = SceneComponentMutationPayloadKind::None;
		std::uint64_t expectedGraphRevision = 0;
		std::uint32_t sourceObjectId = 0;
		TerraEngine::UUID sourceObjectUuid = TerraEngine::EMPTY_UUID;
		std::string sourceBindingId;
		std::uint64_t sourceFrameOrder = 0;
	};

	struct SceneMutationResult {
		std::string requestId;
		bool accepted = false;
		TerraEngine::UUID createdObjectId = TerraEngine::EMPTY_UUID;
		std::uint32_t sourceObjectId = 0;
		TerraEngine::UUID sourceObjectUuid = TerraEngine::EMPTY_UUID;
		std::string sourceBindingId;
		std::uint64_t sourceFrameOrder = 0;
		std::vector<SceneDiagnostic> diagnostics;
	};

	struct SceneMutationBatchResult {
		bool applied = false;
		std::string commitName;
		std::size_t inputMutationCount = 0;
		std::size_t acceptedMutationCount = 0;
		std::size_t rejectedMutationCount = 0;
		std::size_t typedMutationCount = 0;
		std::size_t jsonMutationFallbackCount = 0;
		std::size_t coalescedMutationCount = 0;
		std::vector<SceneMutationResult> results;
		// Transaction receipts let the protected producer acknowledge publication
		// only after this flush has actually accepted or rejected its full batch.
		std::vector<TransactionResult> transactionResults;
		std::vector<SceneDiagnostic> diagnostics;
		std::string formattedCommitSummary;
	};

	struct SceneMutationWorkerBuffer {
		std::uint32_t workerIndex = 0;
		std::vector<SceneMutationRequest> requests;
	};

	class TERRAENGINE_API SceneMutationQueue {
	public:
		static SceneMutationQueue& Instance();

		void Enqueue(SceneMutationRequest request);
		void EnqueueFromWorker(std::uint32_t workerIndex, SceneMutationRequest request);
		void EnqueueTransaction(Transaction transaction);
		TransactionResult PreflightTransaction(const Transaction& transaction, ObjectManager& objects) const;
		bool CancelTransaction(const std::string& transactionId);
		bool Empty() const;
		std::size_t Size() const;
		std::vector<SceneMutationRequest> PendingRequests() const;
		std::vector<Transaction> PendingTransactions() const;
		TransactionDiagnostics GetTransactionDiagnostics() const;
		void Clear();
		SceneMutationBatchResult Validate(const SceneGraph& graph) const;
		SceneMutationBatchResult Apply(ObjectManager& objects);
		SceneMutationBatchResult Apply(ObjectManager& objects, std::string commitName);
		std::vector<TransactionResult> DrainTransactions(ObjectManager& objects);

	private:
		std::vector<SceneMutationRequest> requests;
		std::vector<SceneMutationWorkerBuffer> workerBuffers;
		std::vector<Transaction> transactions;
		TransactionDiagnostics transactionDiagnostics;
		mutable std::mutex requestsMutex;
	};

}
