// PhysicsSystem.h - shared editor/export fixed-step physics lifecycle.

#pragma once

#include "Core/SystemBase.h"
#include "PhysicsAuthoringSnapshot.h"
#include "PhysicsDiagnostics.h"
#include "PhysicsWorld.h"
#include "Scene/TransactionalMutation.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace TerraEngine::Physics {
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

		PhysicsCommandReceipt SubmitCommand(PhysicsCommand command);
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
		std::shared_ptr<const PhysicsCommandResultSnapshot> LatestCommandResults() const;
		std::shared_ptr<const PhysicsEventPacket> LatestEventPacket() const;
		std::shared_ptr<const PhysicsCharacterStatePacket> LatestCharacterStatePacket() const;
		std::shared_ptr<const PhysicsDiagnosticsSnapshot> LatestDiagnostics() const;
		std::uint64_t ExplicitStepPublicationCount() const;
		// The scheduler calls this only after the protected After Physics scene
		// mutation flush, carrying receipts for generation-validated Transform writes.
		void OnAfterPhysicsFlush(const std::vector<TerraEngine::Scene::TransactionResult>& results);

	private:
		PhysicsSystem();
		~PhysicsSystem() override;

		struct Impl;
		Impl* impl = nullptr;
	};
}
