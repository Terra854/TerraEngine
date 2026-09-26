// PhysicsSystem.h - shared editor/export fixed-step physics lifecycle.

#pragma once

#include "Core/SystemBase.h"
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "PhysicsAuthoringSnapshot.h"
#include "PhysicsDiagnostics.h"
#include "CharacterMotionTrace.h"
#include "PhysicsTransactions.h"
#include "PhysicsWorld.h"
#include "Backend/IPhysicsBackend.h"
#include "Scene/TransactionalMutation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	// The token can only be named by a successful, recovery-tagged ragdoll
	// Deactivate result. Physics retains the immutable pose and old lease; a
	// completion caller cannot replace either value after that result exists.
	struct RagdollExternalRecoveryToken {
		PhysicsWorldHandle world;
		std::uint64_t ragdollId = 0U;
		std::uint64_t correlationId = 0U;
		Animation::RagdollGenerationStamp deactivateGeneration;
		Animation::AnimationFixedTick committedStep = 0;
		bool operator==(const RagdollExternalRecoveryToken&) const = default;
	};

	enum class CharacterExternalRecoveryError : std::uint8_t {
		None, InvalidArgument, StaleWorld, StaleToken, StaleOwner,
		StalePose, EpochExhausted, ProviderRejected, CapacityExceeded
	};

	struct CharacterExternalRecoveryResult {
		bool accepted = false;
		CharacterExternalRecoveryError error = CharacterExternalRecoveryError::InvalidArgument;
		CharacterMotionLease releasedLease;
	};

	// An Activate can fail before a provider generation/result exists. Animation
	// retains this typed terminal proof and may submit it only after a later
	// explicit Deactivate or RequestRecovery, never in the rejection callback.
	struct CharacterFailedRagdollAdmissionProof {
		CharacterMotionLease expectedExternalLease;
		std::uint64_t externalOwnerId = 0U;
		std::uint64_t ragdollId = 0U;
		std::uint64_t failedActivationCorrelationId = 0U;
		Animation::RagdollGenerationStamp failedActivationGeneration;
		std::uint64_t failurePublicationGeneration = 0U;
		Animation::AnimationFixedTick failureFrameTick = 0;
		PhysicsFrameHandle frame;
		std::uint64_t bindingGeneration = 0U;
		std::shared_ptr<const CharacterMotionValidity> bindingGuard;
	};

	// This snapshot is intentionally smaller than provider job diagnostics. It is
	// safe to expose at lifecycle boundaries because it contains only counts and
	// cannot leak Jolt job objects or scheduler-owned task pointers.
	struct PhysicsSystemJobDrainDiagnostics {
		std::uint32_t queued = 0U;
		std::uint32_t activePumps = 0U;
	};

	// Explicit diagnostic reads walk the bounded runtime registry, never provider
	// objects. Byte counts name copied payload storage, not allocator overhead.
	struct CharacterMotionStorageDiagnostics {
		std::size_t characters = 0U;
		std::size_t ownedLeases = 0U;
		std::size_t externalLeases = 0U;
		std::size_t guardedTargets = 0U;
		std::size_t retainedIntervals = 0U;
		std::size_t committedIntervals = 0U;
		std::size_t livePayloadBytes = 0U;
		std::size_t traceReservedBytes = 0U;
	};

	// Copied proof of an accepted terrain provider publication. Scene instances
	// are not collision identities: the terrain asset and authored area are.
	struct TerrainCollisionPublication {
		TerraEngine::UUID terrainAssetId = TerraEngine::EMPTY_UUID;
		std::int64_t areaX = 0;
		std::int64_t areaZ = 0;
		PhysicsWorldHandle world;
		PhysicsBodyHandle body;
		PhysicsShapeHandle shape;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Terrain::Generation sourcePublicationGeneration = 0U;
		std::uint64_t authoredGeneration = 0U;
		std::uint64_t authoredHash = 0U;
		float sampleSpacingMetres = 0.0F;
	};

	class TERRAENGINE_API PhysicsSystem final : public SystemsBase {
	public:
		static PhysicsSystem& Instance();

		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// Editor-hosted engines create a fresh world only for Play Mode. Exported
		// runtimes auto-start one during initialization and use the same pipeline.
		void SetEditorLifecycleControlled(bool controlled);
		bool StartWorld();
		bool StartWorld(const PhysicsSettings& settings);
		bool StopWorld();
		// Project-settings edits enter a typed deferred path. The requested policy
		// is validated immediately and becomes visible only at PhysicsSnapshot's
		// protected boundary (or the next explicit StartWorld call).
		bool QueueSettingsUpdate(PhysicsSettings settings);
		PhysicsSettings ConfiguredSettings() const;
		std::uint64_t SettingsGeneration() const;
		bool SetPaused(bool paused);
		bool RequestExplicitStep();
		bool IsPaused() const;
		float FixedTimeStep() const;
		PhysicsWorldHandle WorldHandle() const;
		// Additional local islands are created only at a protected lifecycle
		// boundary. Each descriptor names the independent backend world that owns
		// its bounded float simulation coordinates.
		std::optional<PhysicsFrameDescriptor> CreateFrame(const PhysicsFrameCreateRequest& request);
		bool DestroyFrame(PhysicsFrameHandle frame);
		std::vector<PhysicsFrameDescriptor> FrameDescriptors() const;
		// Scheduler-owned Before Physics callers submit the complete closure here;
		// worker producers prepare only the typed request and callback packet.
		PhysicsFrameMigrationReceipt CommitFrameMigration(
			const PhysicsFrameMigrationRequest& request,
			const PhysicsFrameMigrationOperations& operations);
		// Called only from the declared Before Physics protected boundary. Terrain
		// workers supply immutable recipes; this method allocates fresh handles and
		// delegates provider staging/rollback without exposing backend pointers.
		TerrainCollisionReplacementReceipt CommitTerrainCollisionReplacement(
			TerrainCollisionReplacementPacket packet);
		[[nodiscard]] std::vector<TerrainCollisionPublication> CopyTerrainCollisionPublications() const;
		// The protected scene-lifecycle boundary calls this after the last component
		// referencing an asset disappears. Every area body is retired atomically from
		// the system-owned lookup before later publications can observe it.
		[[nodiscard]] bool RemoveTerrainCollisions(TerraEngine::UUID terrainAssetId);
		// Returns the current physics-clock generation for copied cross-system
		// requests. Animation must not substitute its independent frame counter:
		// a later physics extraction validates this exact simulation time domain.
		Animation::AnimationFixedTick CurrentFixedTick() const;

		PhysicsCommandReceipt SubmitCommand(PhysicsCommand command);
		// The protected locomotion publisher preflights the full controller set,
		// then publishes it before its own non-fallible authority-root swap.
		CharacterMotionLeaseResult PreflightCharacterMotionLeases(
			std::span<const CharacterMotionLeaseChange> changes) const;
		CharacterMotionLeaseResult PublishCharacterMotionLeases(
			std::span<const CharacterMotionLeaseChange> changes);
		std::optional<CharacterMotionLease> ReadCharacterMotionLease(
			const PhysicsCharacterCommandTarget& controller) const;
		// Animation's protected hot-rebind check asks only whether this numeric
		// ragdoll owner still excludes any live character. No Animation handle,
		// callback, or mutable provider pointer crosses the Physics boundary.
		[[nodiscard]] bool HasExternalCharacterMotionOwner(std::uint64_t externalOwnerId) const;
		[[nodiscard]] std::optional<CharacterMotionLease> ReadExternalCharacterMotionOwner(
			std::uint64_t externalOwnerId) const;
		Backend::PhysicsBackendResult PublishCharacterMotionTarget(const CharacterMotionTarget& target);
		// Reserving counts bounded interval capacity but contributes no movement.
		// The producer reserves its independent application ledger before committing
		// this token at a protected boundary, or cancels it on ledger rejection.
		RootMotionIntervalAdmission ReserveRootMotionInterval(const RootMotionInterval& interval);
		RootMotionIntervalAdmission CommitRootMotionInterval(RootMotionIntervalReservation reservation);
		bool CancelRootMotionIntervalReservation(RootMotionIntervalReservation reservation);
		void CancelRootMotionIntervals(const PhysicsCharacterCommandTarget& controller,
			RootMotionIntervalError reason);
		CharacterExternalRecoveryResult CompleteExternalCharacterRecovery(
			const RagdollExternalRecoveryToken& token);
		CharacterExternalRecoveryError RefreshExternalCharacterRecovery(
			const RagdollExternalRecoveryToken& token,
			std::shared_ptr<const CharacterExternalRecoveryIntent> freshIntent);
		CharacterExternalRecoveryResult AbortFailedExternalRagdollAdmission(
			const CharacterFailedRagdollAdmissionProof& proof);
		[[nodiscard]] std::shared_ptr<const RootMotionResultPacket> ReadRootMotionResults(
			std::uint64_t afterCursor, std::size_t maxRecords = 256U) const;
		bool AcknowledgeRootMotionResults(std::uint64_t throughCursor);
		// Recheck retained producer evidence outside Physics locks immediately
		// before protected command commit and each fixed-step workload.
		void RevalidateCharacterMotionTargets();
		// Copied root source/binding guards run only at the protected command
		// boundary, outside the Physics mutex and before worker-safe fixed stepping.
		void RevalidateRootMotionIntervals();
		// Ragdoll producers may run in animation, scripts, or worker preparation.
		// They only enqueue copied typed requests here; PhysicsCommandCommit owns the
		// later drain and therefore no producer mutates the live provider world.
		Animation::AnimationStatus SubmitRagdollPhysicsRequest(Animation::RagdollPhysicsRequest request);
		Animation::AnimationStatus SubmitRagdollPhysicsRequestBatch(
			std::span<const Animation::RagdollPhysicsRequest> requests);
		std::shared_ptr<const Animation::PhysicsPoseSnapshot> LatestRagdollPhysicsPose(
			std::uint64_t ragdollId,
			const Animation::RagdollGenerationStamp& expectedGeneration) const;
		std::shared_ptr<const Animation::PhysicsPoseSnapshot> LatestRagdollPhysicsPoseAtOrAfter(
			std::uint64_t ragdollId,
			const Animation::RagdollGenerationStamp& expectedIdentity) const;
		std::shared_ptr<const Animation::RagdollPhysicsResultSnapshot> LatestRagdollPhysicsResults() const;
		// Animation root-motion and other deferred producers use the same truthful
		// command-id cancellation path as the world-owned queue.
		bool CancelCommand(PhysicsCommandId commandId);
		PhysicsQueryReceipt SubmitQuery(PhysicsQueryRequest request);
		PhysicsQueryBatchSubmission SubmitQueryBatch(std::span<const PhysicsQueryRequest> requests);
		bool CancelQuery(PhysicsQueryHandle handle);
		PhysicsQueryResult ReadQuery(PhysicsQueryHandle handle) const;
		// Immediate queries execute synchronously under the system's protected
		// boundary. Normal gameplay should prefer scheduled batches.
		PhysicsQueryResult QueryImmediate(PhysicsQueryRequest request);
		PhysicsEventSubscription SubscribeEvents(PhysicsEventSubscriptionPolicy policy);
		bool UnsubscribeEvents(std::uint64_t id, PhysicsGeneration generation);
		std::shared_ptr<const PhysicsBodyStateSnapshot> LatestBodySnapshot() const;
		std::shared_ptr<const IncrementalBodyStateSnapshot> LatestIncrementalBodySnapshot() const;
		std::shared_ptr<const PhysicsCommandResultSnapshot> LatestCommandResults() const;
		std::shared_ptr<const PhysicsEventPacket> LatestEventPacket() const;
		// Returns one immutable retained event packet for the active world. Callers
		// provide a tick rather than receiving the mutable world/history object, so
		// editor automation can inspect bounded history without racing publication.
		std::shared_ptr<const PhysicsEventPacket> ReadRetainedEventPacket(PhysicsTick requestedTick) const;
		std::shared_ptr<const PhysicsCharacterStatePacket> LatestCharacterStatePacket() const;
		std::shared_ptr<const PhysicsDiagnosticsSnapshot> LatestDiagnostics() const;
		PhysicsSystemJobDrainDiagnostics JobDrainDiagnostics() const;
		// Opt-in validation captures accepted ingress only. CopySince reports any
		// overwritten cursor explicitly; missing trace records can never be a pass.
		bool ConfigureCharacterMotionTrace(bool enabled, std::size_t capacity = 16384U);
		CharacterMotionTraceSnapshot CopyCharacterMotionTrace(std::uint64_t afterSequence) const;
		CharacterMotionStorageDiagnostics ReadCharacterMotionStorage() const;
		std::uint64_t ExplicitStepPublicationCount() const;
		// The scheduler calls this only after the protected After Physics scene
		// mutation flush, carrying receipts for generation-validated Transform writes.
		void OnAfterPhysicsFlush(
			const std::vector<TerraEngine::Scene::TransactionResult>& results,
			const std::vector<TerraEngine::Scene::TransactionPublicationGroupResult>& publicationGroups = {});

	private:
		PhysicsSystem();
		~PhysicsSystem() override;

		struct Impl;
		Impl* impl = nullptr;
	};
}
