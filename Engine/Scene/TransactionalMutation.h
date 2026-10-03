// TransactionalMutation.h - engine-scoped STM for scene-owned mutations.
//
// The transaction layer lets worker systems prepare object, hierarchy,
// metadata, and reflected component writes without touching live scene state.
// Commit still runs on a protected engine path so external side effects and
// EnTT storage ownership remain deterministic.

#pragma once

#include "Core/Export.h"
#include "Components/Transform.h"
#include "Core/Object/Reflection.h"
#include "Scene/WorldCoordinate.h"
#include "Utilities/UUID.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {
	inline constexpr std::string_view kTerraSpatialTransactionFormat = "TerraSpatialTransactionV1";

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
		InvalidCoordinate,
		CoordinateOverflow,
		StaleSpatialPublication,
		SimulationFrameConflict,
		InjectedCommitFailure,
		Canceled,
		Expired
	};

	enum class TransactionWriteKind {
		CreateObject,
		// Undo/redo restores an editor-authored object graph from a snapshot at the
		// same protected flush boundary as every other scene mutation.
		RestoreObjectSnapshot,
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
		// World-space writes are deliberately typed. No caller may smuggle an
		// absolute float vector through the generic reflected component path.
		SetWorldTransform,
		TranslateWorld,
		ChangeSimulationFrame,
		// A physics publication changes both fields together. Keeping the pair
		// typed avoids reflection and per-property allocations at a hot boundary.
		UpdateTransformBatch,
		// Animation owns runtime storage, so this records typed publication intent
		// for the protected boundary instead of writing component state from workers.
		PublishAnimationMutations,
		// Append additive kinds to preserve existing native/wire enum values.
		// Script runtime instances are never invoked during authored preparation.
		AddScriptBinding
	};

	// A transform batch carries canonical world poses for existing objects. The
	// transaction layer observes every target before it accepts the batch, then
	// applies all entries or rolls the complete batch back at the flush boundary.
	struct TransactionTransformUpdate {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		// Physics and animation publish an exact canonical pose. A transform batch
		// intentionally has no vec3 absolute-position compatibility field.
		WorldPose pose{};
		// Physics owns position and rotation, not authored mesh size. When requested,
		// retain the target's scale at the protected, revision-validated boundary.
		bool preserveScale = false;
		// Native inverse/history data only: retain the exact local pose as well as
		// the world cache. Normal publications derive local data from the hierarchy.
		std::optional<ParentRelativePose> restoreParentRelativePose;
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
		std::uint64_t coordinateFrameRevision = 0;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t parentLifetimeGeneration = 0;
		std::uint64_t parentTransformRevision = 0;
	};

	struct TransactionReadSet {
		// Scene lifetime and hierarchy are separate generations. A clear/swap can
		// reuse hierarchy revision values, so publication packets must observe both.
		std::uint64_t sceneGeneration = 0;
		std::uint64_t sceneRevision = 0;
		std::uint64_t coordinateFrameRevision = 0;
		std::uint64_t spatialSnapshotGeneration = 0;
		std::uint64_t residencyGeneration = 0;
		std::vector<TransactionObjectObservation> objects;
	};

	struct TransactionWriteOperation {
		TransactionWriteKind kind = TransactionWriteKind::CreateObject;
		TransactionObjectRef target;
		TransactionObjectRef parent;
		std::int32_t createdLocalObjectId = 0;
		// Generic managed creation may reserve an identity before its boundary;
		// native callers leave this empty and retain generated UUID behavior.
		TerraEngine::UUID createdStableObjectId = TerraEngine::EMPTY_UUID;
		std::string displayName;
		std::string stringValue;
		bool boolValue = false;
		std::string componentName;
		std::string propertyName;
		TerraEngine::Reflection::Variant propertyValue;
		// SetWorldTransform replaces the complete canonical pose in one protected
		// write. It cannot be confused with a parent-relative local transform.
		WorldPose worldPose{};
		// Filled by native inverse capture, never by the external JSON decoder.
		std::optional<ParentRelativePose> restoreParentRelativePose;
		// TranslateWorld is intentionally a displacement rather than an absolute
		// location, preventing a caller from treating a delta as a position.
		WorldDelta worldDelta{};
		// Transactions carry the reparent intent explicitly so protected commit
		// never guesses whether canonical or parent-relative state is authoritative.
		bool keepWorldOnReparent = true;
		std::uint64_t sourceSimulationFrameGeneration = 0;
		std::uint64_t destinationSimulationFrameGeneration = 0;
		std::vector<TransactionTransformUpdate> transformUpdates;
		std::vector<AnimationMutationPublication> animationPublications;
		// This payload is populated only by editor history replay. Runtime systems
		// keep using compact typed fields and never pay serialization costs.
		nlohmann::json serializedObject = nlohmann::json::object();
		// Native editor-history removal inverse only. External generic producers
		// still use typed component operations; this is decoded before live writes.
		std::optional<nlohmann::json> serializedComponent;
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
		// Test-only failpoint proves that a protected commit restores every prior
		// write. Production origins reject this field during preflight.
		std::optional<std::size_t> injectedFailureOperationIndex;
		bool canceled = false;
	};

	// Shared accounting keeps budgets, terminal cancellations and projected
	// producer receipts consistent for multi-target typed packets.
	TERRAENGINE_API std::size_t LogicalWriteCount(const TransactionWriteOperation& operation) noexcept;
	TERRAENGINE_API std::size_t LogicalWriteCount(const Transaction& transaction) noexcept;

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
		// The spatial owner assigns this only after an all-or-nothing publication.
		std::uint64_t spatialPublicationGeneration = 0;

		bool MutatedScene() const { return status == TransactionStatus::Committed && appliedWriteCount > 0; }
	};

	// Journal corruption is a required execution failure. Callers must let this
	// exception reach the frame failure boundary rather than report rejection.
	class TransactionRestorationFailure : public std::runtime_error {
	public:
		using std::runtime_error::runtime_error;
	};
	// Observers execute after no-fail commit. Retain the truthful committed
	// result so a queue can publish its receipt/history before stopping the frame.
	class TransactionPublicationFailure : public std::runtime_error {
	public:
		TransactionResult committedResult;
		TransactionPublicationFailure(TransactionResult result, const std::string& reason)
			: std::runtime_error("Committed transaction " + result.transactionId + ": " + reason), committedResult(std::move(result)) {}
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
	// A group has already committed when an observer fails. Retain every chunk
	// receipt so producers can settle once even though the frame cannot return.
	class TransactionGroupPublicationFailure : public TransactionPublicationFailure {
	public:
		TransactionPublicationGroupResult committedGroupResult;
		TransactionGroupPublicationFailure(TransactionPublicationGroupResult result, const std::string& reason)
			: TransactionPublicationFailure(result.aggregateResult, reason), committedGroupResult(std::move(result)) {}
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
		TransactionalMutationBuilder& ReparentObject(TerraEngine::UUID objectId, TransactionObjectRef parent, bool keepWorld = true);
		TransactionalMutationBuilder& ReparentObject(TransactionObjectRef object, TransactionObjectRef parent, bool keepWorld = true);
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
		TransactionalMutationBuilder& SetWorldTransform(ObjectManager& objects, TerraEngine::UUID objectId, WorldPose pose);
		TransactionalMutationBuilder& TranslateWorld(ObjectManager& objects, TerraEngine::UUID objectId, WorldDelta delta);
		TransactionalMutationBuilder& ChangeSimulationFrame(ObjectManager& objects, TerraEngine::UUID objectId, std::uint64_t sourceGeneration, std::uint64_t destinationGeneration);
		TransactionalMutationBuilder& InjectCommitFailureForTesting(std::size_t operationIndex);
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
	// JSON is accepted only at managed/editor boundaries.  This lowerer converts
	// the unbounded wire format into the same typed operations used by native jobs.
	TERRAENGINE_API nlohmann::json SerializeSpatialTransaction(const Transaction& transaction);
	TERRAENGINE_API std::optional<Transaction> LowerSpatialTransaction(const nlohmann::json& document, std::string* outError = nullptr);

}
