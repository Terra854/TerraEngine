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
#include "WorldCoordinate.h"
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
		SetObjectName,
		SetObjectTag,
		SetObjectLayer,
		SetObjectActive,
		SetObjectStatic,
		AddComponent,
		RemoveComponent,
		UpdateComponent,
		// Spatial requests are ingress envelopes only. They must contain a lowered
		// typed Transaction and can never carry a float Transform property payload.
		SetWorldTransform,
		TranslateWorld,
		ChangeSimulationFrame,
		AddScriptBinding,
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
		// Metadata mutations carry copied scalar values so worker callbacks never
		// retain pointers into managed strings or touch ObjectManager directly.
		std::string metadataValue;
		bool metadataBooleanValue = false;
		std::string componentName;
		std::string payloadJson;
		SceneComponentPropertyMutation typedComponentPayload;
		std::optional<Transaction> typedSpatialTransaction;
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

	// Navigation observes committed source changes through copied values. Producers
	// never hand workers a live component, transform, or collider pointer that may
	// change while a detached navigation candidate is being prepared.
	enum class NavigationSourceMutationOperation : std::uint8_t {
		Create,
		Enable,
		Disable,
		Move,
		Rotate,
		Scale,
		Delete,
		Replace,
		Reshape
	};

	struct NavigationSourceMutationBounds {
		WorldPosition minimum{};
		WorldPosition maximum{};
		bool valid = false;
	};

	struct NavigationSourceMutationIntent {
		std::string intentId;
		std::string transactionId;
		TerraEngine::UUID sourceId = TerraEngine::EMPTY_UUID;
		NavigationSourceMutationOperation operation = NavigationSourceMutationOperation::Create;
		NavigationSourceMutationBounds oldBounds{};
		NavigationSourceMutationBounds newBounds{};
		std::uint64_t expectedSourceRevision = 0U;
		std::uint64_t committedSourceRevision = 0U;
		// The queue assigns this only after the owning transaction commits. Source
		// content revisions are hashes and must never be used as ordering tokens;
		// this monotonic receipt sequence is the protected ordering evidence for a
		// dynamic navigation publication.
		std::uint64_t committedChangeSequence = 0U;
		std::uint64_t physicalReadyRevision = 0U;
		bool oldEnabled = false;
		bool newEnabled = false;
	};

	struct NavigationSourceMutationReceipt {
		NavigationSourceMutationIntent intent{};
		TransactionStatus transactionStatus = TransactionStatus::Pending;
		bool accepted = false;
		std::string message;
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
		// Group receipts preserve the tick-wide decision while still identifying
		// each preparation chunk for producer-side acknowledgement.
		std::vector<TransactionPublicationGroupResult> publicationGroupResults;
		// Typed animation receipts become visible only because Apply owns a declared
		// protected boundary; systems can consume them without JSON conversion.
		std::vector<AnimationMutationPublication> acceptedAnimationPublications;
		// Dynamic navigation consumes only decisions that share the scene commit's
		// protected observation boundary.
		std::vector<NavigationSourceMutationReceipt> navigationSourceMutationReceipts;
		std::vector<SceneDiagnostic> diagnostics;
		std::string formattedCommitSummary;
	};

	struct SceneMutationWorkerBuffer {
		std::uint32_t workerIndex = 0;
		std::vector<SceneMutationRequest> requests;
	};

	struct EditorTransactionCompletion {
		// Keep the immutable request beside its protected-boundary receipt. Async
		// editor producers may be enqueued and flushed between editor UI passes, so
		// reconstructing this association from PendingTransactions is racy.
		Transaction transaction;
		TransactionResult result;
	};

	class TERRAENGINE_API SceneMutationQueue {
	public:
		static SceneMutationQueue& Instance();

		void Enqueue(SceneMutationRequest request);
		void EnqueueFromWorker(std::uint32_t workerIndex, SceneMutationRequest request);
		void EnqueueTransaction(Transaction transaction);
		void EnqueuePublicationGroup(TransactionPublicationGroup group);
		[[nodiscard]] bool EnqueueNavigationSourceMutationIntent(NavigationSourceMutationIntent intent);
		// Protected scene bridges that derive a source receipt from a committed typed
		// transform use this allocator after the transaction result is known. It is
		// intentionally process-lifetime monotonic, including across Clear(), so a
		// replaced object lifetime cannot reuse an old dynamic-publication order.
		[[nodiscard]] std::uint64_t AllocateCommittedNavigationSourceChangeSequence();
		TransactionResult PreflightTransaction(const Transaction& transaction, ObjectManager& objects) const;
		bool CancelTransaction(const std::string& transactionId);
		bool CancelPublicationGroup(const std::string& groupId);
		bool Empty() const;
		std::size_t Size() const;
		std::vector<SceneMutationRequest> PendingRequests() const;
		std::vector<Transaction> PendingTransactions() const;
		std::vector<TransactionPublicationGroup> PendingPublicationGroups() const;
		// Editor undo needs the complete inverse payload produced at commit time.
		// Diagnostics intentionally discard that payload, so the editor drains this
		// short-lived receipt channel at the beginning of its following frame.
		std::vector<TransactionResult> DrainCompletedEditorTransactionResults();
		std::vector<EditorTransactionCompletion> DrainCompletedEditorTransactions();
		std::vector<NavigationSourceMutationReceipt> DrainCompletedNavigationSourceMutationReceipts();
		TransactionDiagnostics GetTransactionDiagnostics() const;
		void Clear();
		SceneMutationBatchResult Validate(const SceneGraph& graph) const;
		SceneMutationBatchResult Apply(ObjectManager& objects);
		SceneMutationBatchResult Apply(ObjectManager& objects, std::string commitName);
		std::vector<TransactionResult> DrainTransactions(ObjectManager& objects);
		std::vector<TransactionPublicationGroupResult> DrainPublicationGroups(ObjectManager& objects);

	private:
		std::vector<SceneMutationRequest> requests;
		std::vector<SceneMutationWorkerBuffer> workerBuffers;
		std::vector<Transaction> transactions;
		std::vector<TransactionPublicationGroup> publicationGroups;
		std::vector<NavigationSourceMutationIntent> navigationSourceMutationIntents;
		// Keep this process-lifetime counter across Clear(). A scene may replace an
		// object lifetime while navigation still drains old receipts, so resetting
		// it could make an old source update alias a newer committed change.
		std::uint64_t nextNavigationSourceChangeSequence = 1U;
		std::vector<EditorTransactionCompletion> completedEditorTransactions;
		std::vector<NavigationSourceMutationReceipt> completedNavigationSourceMutationReceipts;
		TransactionDiagnostics transactionDiagnostics;
		mutable std::mutex requestsMutex;
	};

}
