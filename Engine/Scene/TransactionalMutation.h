// TransactionalMutation.h - engine-scoped STM for scene-owned mutations.
//
// The transaction layer lets worker systems prepare object, hierarchy,
// metadata, and reflected component writes without touching live scene state.
// Commit still runs on a protected engine path so external side effects and
// EnTT storage ownership remain deterministic.

#pragma once

#include "Core/Export.h"
#include "Core/Object/Reflection.h"
#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {

	enum class TransactionOrigin {
		System,
		Editor,
		ManagedCallback,
		Automation,
		Test
	};

	enum class TransactionMode {
		Commit,
		PreflightOnly
	};

	enum class TransactionStatus {
		Pending,
		Committed,
		Rejected,
		Conflicted,
		Canceled,
		Expired,
		PreflightAccepted,
		PreflightRejected
	};

	enum class TransactionConflictReason {
		None,
		StaleObjectIdentity,
		MissingTarget,
		ComponentVersionMismatch,
		HierarchyConflict,
		MetadataVersionMismatch,
		PolicyRejection,
		InvalidPayload,
		CommitBudgetRejection,
		SceneUnloadedOrSwitched,
		Canceled,
		Expired
	};

	enum class TransactionWriteKind {
		CreateObject,
		DeleteObject,
		ReparentObject,
		SetName,
		SetTag,
		SetLayer,
		SetActive,
		SetStatic,
		AddComponent,
		RemoveComponent,
		UpdateComponentProperty,
		// A physics publication changes both fields together. Keeping the pair
		// typed avoids reflection and per-property allocations at a hot boundary.
		UpdateTransformBatch,
		// Animation owns runtime storage, so this records typed publication intent
		// for the protected boundary instead of writing component state from workers.
		PublishAnimationMutations
	};

	// A transform batch carries local-space values for existing objects. The
	// transaction layer observes every target before it accepts the batch, then
	// applies all entries or rolls the complete batch back at the flush boundary.
	struct TransactionTransformUpdate {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		glm::vec3 position{0.0F, 0.0F, 0.0F};
		glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
	};

	struct AnimationMutationPublication {
		TerraEngine::UUID targetObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t expectedComponentGeneration = 0U;
		std::uint32_t animatorSlot = 0xffffffffU;
		std::uint32_t animatorGeneration = 0U;
		std::int64_t targetTick = 0;
		std::uint64_t producerSequence = 0U;
		std::uint32_t commandCount = 0U;
		std::uint64_t payloadFingerprint = 0U;
	};

	struct TransactionBudget {
		std::size_t maxWriteCount = 4096;
		std::size_t maxConflictCount = 128;
	};

	struct TransactionObjectRef {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::int32_t localObjectId = 0;

		bool UsesLocalObject() const { return localObjectId != 0; }
	};

	struct TransactionObjectObservation {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint64_t lifetimeGeneration = 0;
		std::uint64_t metadataGeneration = 0;
		std::uint64_t componentGeneration = 0;
		std::uint64_t hierarchyRevision = 0;
		std::uint64_t transformRevision = 0;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t parentLifetimeGeneration = 0;
		std::uint64_t parentTransformRevision = 0;
	};

	struct TransactionReadSet {
		// Scene lifetime and hierarchy are separate generations. A clear/swap can
		// reuse hierarchy revision values, so publication packets must observe both.
		std::uint64_t sceneGeneration = 0;
		std::uint64_t sceneRevision = 0;
		std::vector<TransactionObjectObservation> objects;
	};

	struct TransactionWriteOperation {
		TransactionWriteKind kind = TransactionWriteKind::CreateObject;
		TransactionObjectRef target;
		TransactionObjectRef parent;
		std::int32_t createdLocalObjectId = 0;
		std::string displayName;
		std::string stringValue;
		bool boolValue = false;
		std::string componentName;
		std::string propertyName;
		TerraEngine::Reflection::Variant propertyValue;
		std::vector<TransactionTransformUpdate> transformUpdates;
		std::vector<AnimationMutationPublication> animationPublications;
	};

	struct TransactionWriteSet {
		std::vector<TransactionWriteOperation> operations;
	};

	struct TransactionConflictRecord {
		TransactionConflictReason reason = TransactionConflictReason::None;
		TerraEngine::UUID targetObject = TerraEngine::EMPTY_UUID;
		std::string componentType;
		std::string propertyName;
		TransactionWriteKind writeKind = TransactionWriteKind::CreateObject;
		std::uint64_t expectedRevision = 0;
		std::uint64_t actualRevision = 0;
		std::size_t operationIndex = 0;
		std::string humanMessage;
	};

	// A committed write publishes the exact typed operation needed to restore its
	// prior value. Serialized snapshots are used only for object/component removal,
	// where a scalar operation cannot represent the complete authored state.
	struct TransactionInverseDelta {
		std::size_t sourceOperationIndex = 0;
		TransactionWriteOperation operation;
		nlohmann::json serializedObject = nlohmann::json::object();
		nlohmann::json serializedComponent = nlohmann::json::object();
		bool restoresSerializedObject = false;
		bool restoresSerializedComponent = false;
	};

	struct Transaction {
		std::string id;
		TransactionOrigin origin = TransactionOrigin::System;
		TransactionMode mode = TransactionMode::Commit;
		std::string originName;
		std::string undoGroupName;
		TransactionBudget budget;
		TransactionReadSet readSet;
		TransactionWriteSet writeSet;
		bool canceled = false;
	};

	struct TransactionResult {
		std::string transactionId;
		TransactionStatus status = TransactionStatus::Pending;
		std::size_t appliedWriteCount = 0;
		std::size_t rejectedWriteCount = 0;
		std::uint64_t validationCostNs = 0;
		std::uint64_t commitCostNs = 0;
		std::vector<TransactionConflictRecord> conflicts;
		std::vector<TransactionInverseDelta> inverseDeltas;
		std::unordered_map<std::int32_t, TerraEngine::UUID> createdObjects;
		std::string message;
		std::vector<AnimationMutationPublication> acceptedAnimationPublications;

		bool MutatedScene() const { return status == TransactionStatus::Committed && appliedWriteCount > 0; }
	};

	// Preparation chunks are a scheduling detail, not an atomicity boundary.
	// The protected commit flattens one group into a single transaction after
	// validating every chunk, so a stale entry rejects the complete simulation tick.
	struct TransactionPublicationGroup {
		std::string groupId;
		std::vector<Transaction> chunks;
		bool canceled = false;
	};

	struct TransactionPublicationChunkReceipt {
		std::string transactionId;
		std::size_t chunkIndex = 0;
		TransactionStatus status = TransactionStatus::Pending;
		std::size_t writeCount = 0;
	};

	struct TransactionPublicationGroupResult {
		std::string groupId;
		TransactionStatus status = TransactionStatus::Pending;
		bool preflightAccepted = false;
		std::size_t appliedWriteCount = 0;
		std::size_t rejectedWriteCount = 0;
		std::vector<TransactionPublicationChunkReceipt> chunkReceipts;
		TransactionResult aggregateResult;
		std::string message;

		bool Committed() const { return status == TransactionStatus::Committed; }
	};

	struct TransactionDiagnostics {
		std::uint64_t submittedCount = 0;
		std::uint64_t committedCount = 0;
		std::uint64_t rejectedCount = 0;
		std::uint64_t conflictCount = 0;
		std::uint64_t canceledCount = 0;
		std::uint64_t expiredCount = 0;
		std::uint64_t averageValidationCostNs = 0;
		std::uint64_t averageCommitCostNs = 0;
		std::size_t largestSubmittedBatchSize = 0;
		std::size_t largestCommittedBatchSize = 0;
		std::vector<TransactionResult> recentResults;
	};

	class TERRAENGINE_API TransactionalMutationBuilder {
	public:
		TransactionalMutationBuilder(std::string id, TransactionOrigin origin, std::string originName = {});

		TransactionalMutationBuilder& SetMode(TransactionMode mode);
		TransactionalMutationBuilder& SetUndoGroup(std::string undoGroupName);
		TransactionalMutationBuilder& SetBudget(TransactionBudget budget);
		TransactionalMutationBuilder& ObserveObject(ObjectManager& objects, TerraEngine::UUID objectId);
		TransactionalMutationBuilder& ObserveScene(ObjectManager& objects);
		TransactionalMutationBuilder& CreateObject(std::int32_t localObjectId, std::string displayName, TransactionObjectRef parent = {});
		TransactionalMutationBuilder& DeleteObject(TerraEngine::UUID objectId);
		TransactionalMutationBuilder& DeleteObject(TransactionObjectRef object);
		TransactionalMutationBuilder& ReparentObject(TerraEngine::UUID objectId, TransactionObjectRef parent);
		TransactionalMutationBuilder& ReparentObject(TransactionObjectRef object, TransactionObjectRef parent);
		TransactionalMutationBuilder& SetName(TerraEngine::UUID objectId, std::string name);
		TransactionalMutationBuilder& SetName(TransactionObjectRef object, std::string name);
		TransactionalMutationBuilder& SetTag(TerraEngine::UUID objectId, std::string tag);
		TransactionalMutationBuilder& SetTag(TransactionObjectRef object, std::string tag);
		TransactionalMutationBuilder& SetLayer(TerraEngine::UUID objectId, std::string layer);
		TransactionalMutationBuilder& SetLayer(TransactionObjectRef object, std::string layer);
		TransactionalMutationBuilder& SetActive(TerraEngine::UUID objectId, bool active);
		TransactionalMutationBuilder& SetActive(TransactionObjectRef object, bool active);
		TransactionalMutationBuilder& SetStatic(TerraEngine::UUID objectId, bool isStatic);
		TransactionalMutationBuilder& SetStatic(TransactionObjectRef object, bool isStatic);
		TransactionalMutationBuilder& AddComponent(TerraEngine::UUID objectId, std::string componentName);
		TransactionalMutationBuilder& AddComponent(TransactionObjectRef object, std::string componentName);
		TransactionalMutationBuilder& RemoveComponent(TerraEngine::UUID objectId, std::string componentName);
		TransactionalMutationBuilder& RemoveComponent(TransactionObjectRef object, std::string componentName);
		TransactionalMutationBuilder& UpdateComponentProperty(TerraEngine::UUID objectId, std::string componentName, std::string propertyName, TerraEngine::Reflection::Variant value);
		TransactionalMutationBuilder& UpdateComponentProperty(TransactionObjectRef object, std::string componentName, std::string propertyName, TerraEngine::Reflection::Variant value);
		// Snapshot every target while the protected producer owns ObjectManager,
		// so the later protected flush can reject the whole batch if it went stale.
		TransactionalMutationBuilder& UpdateTransformBatch(ObjectManager& objects, std::vector<TransactionTransformUpdate> updates);
		TransactionalMutationBuilder& PublishAnimationMutations(ObjectManager& objects, std::vector<AnimationMutationPublication> publications);
		Transaction Build() &;
		Transaction Build() &&;

	private:
		Transaction transaction;
	};

	TERRAENGINE_API std::string_view ToString(TransactionStatus status);
	TERRAENGINE_API std::string_view ToString(TransactionConflictReason reason);
	TERRAENGINE_API TransactionObjectRef ExistingObject(TerraEngine::UUID objectId);
	TERRAENGINE_API TransactionObjectRef LocalObject(std::int32_t localObjectId);
	TERRAENGINE_API TransactionResult PreflightTransaction(ObjectManager& objects, const Transaction& transaction);
	TERRAENGINE_API TransactionResult CommitTransaction(ObjectManager& objects, const Transaction& transaction);
	TERRAENGINE_API TransactionPublicationGroupResult PreflightTransactionPublicationGroup(ObjectManager& objects, const TransactionPublicationGroup& group);
	TERRAENGINE_API TransactionPublicationGroupResult CommitTransactionPublicationGroup(ObjectManager& objects, const TransactionPublicationGroup& group);
	TERRAENGINE_API bool ApplyTransactionInverseDeltas(ObjectManager& objects, const std::vector<TransactionInverseDelta>& inverseDeltas);
	TERRAENGINE_API void AccumulateTransactionDiagnostics(TransactionDiagnostics& diagnostics, const Transaction& transaction, const TransactionResult& result);
	TERRAENGINE_API TransactionDiagnostics MergeTransactionDiagnostics(const TransactionDiagnostics& left, const TransactionDiagnostics& right);

}
