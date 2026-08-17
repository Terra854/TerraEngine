// PhysicsSystem.h - shared editor/export fixed-step physics lifecycle.

#pragma once

#include "Core/SystemBase.h"
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "PhysicsAuthoringSnapshot.h"
#include "PhysicsDiagnostics.h"
#include "PhysicsTransactions.h"
#include "PhysicsWorld.h"
#include "Scene/TransactionalMutation.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Physics {
	// This snapshot is intentionally smaller than provider job diagnostics. It is
	// safe to expose at lifecycle boundaries because it contains only counts and
	// cannot leak Jolt job objects or scheduler-owned task pointers.
	struct PhysicsSystemJobDrainDiagnostics {
		std::uint32_t queued = 0U;
		std::uint32_t activePumps = 0U;
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
		// Returns the current physics-clock generation for copied cross-system
		// requests. Animation must not substitute its independent frame counter:
		// a later physics extraction validates this exact simulation time domain.
		Animation::AnimationFixedTick CurrentFixedTick() const;

		PhysicsCommandReceipt SubmitCommand(PhysicsCommand command);
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
