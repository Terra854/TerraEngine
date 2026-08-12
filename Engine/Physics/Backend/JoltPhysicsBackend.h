// JoltPhysicsBackend.h - provider-neutral public edge of the Jolt 5.6 backend.
//
// Jolt includes, IDs, references, and allocation details live exclusively in
// the .cpp implementation. Engine systems depend only on IPhysicsBackend.

#pragma once

#include "IPhysicsBackend.h"
#include "PhysicsAllocatorAdapter.h"

#include <cstddef>
#include <cstdint>

class ThreadPool;

namespace TerraEngine::Physics::Backend {
	// Shutdown validation needs only the provider adapter's live queue state.
	// Keeping this provider-neutral snapshot here avoids leaking Jolt headers
	// through the public backend header into engine and editor compilation units.
	struct PhysicsBackendJobDrainDiagnostics {
		std::uint32_t queued = 0U;
		std::uint32_t activePumps = 0U;
	};
	struct JoltPhysicsBackendConfiguration {
		std::uint32_t maximumPhysicsWorkers = 0U;
		std::uint32_t maximumBarriers = 8U;
		std::uint32_t maximumJobs = 2048U;
		std::size_t temporaryAllocatorBytes = 32U * 1024U * 1024U;
	};

	class TERRAENGINE_API JoltPhysicsBackend final : public IPhysicsBackend {
	public:
		explicit JoltPhysicsBackend(
			ThreadPool& sharedPool,
			JoltPhysicsBackendConfiguration configuration = {});
		~JoltPhysicsBackend() override;

		JoltPhysicsBackend(const JoltPhysicsBackend&) = delete;
		JoltPhysicsBackend& operator=(const JoltPhysicsBackend&) = delete;
		JoltPhysicsBackend(JoltPhysicsBackend&&) = delete;
		JoltPhysicsBackend& operator=(JoltPhysicsBackend&&) = delete;

		PhysicsBackendResult Initialize() override;
		PhysicsBackendResult Shutdown() override;
		PhysicsBackendResult CreateWorld(const PhysicsBackendWorldConfiguration& configuration) override;
		PhysicsBackendResult DestroyWorld(PhysicsWorldHandle world) override;
		PhysicsBackendResult ReplaceWorld(
			const PhysicsBackendWorldConfiguration& configuration,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult ReserveAdmission(
			const PhysicsBackendAdmissionReservation& reservation,
			PhysicsBackendPreparedAdmission& prepared) override;

		PhysicsCookResult CookShape(const PhysicsCookRequest& request) override;
		PhysicsBackendResult CreateShape(
			PhysicsWorldHandle world,
			PhysicsShapeHandle handle,
			const PhysicsShapeRecipe& recipe,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult DestroyShape(PhysicsShapeHandle handle) override;

		PhysicsBackendResult CreateBody(
			const PhysicsBodyCreateRequest& request,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult CreateBodies(
			std::span<const PhysicsBodyCreateRequest> requests,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult UpdateBody(const PhysicsBodyUpdateRequest& request) override;
		PhysicsBackendResult DestroyBody(PhysicsBodyHandle handle) override;

		PhysicsBackendResult CreateConstraint(
			const PhysicsConstraintCreateRequest& request,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult UpdateConstraint(const PhysicsConstraintCreateRequest& request) override;
		PhysicsBackendResult DestroyConstraint(PhysicsConstraintHandle handle) override;

		PhysicsBackendResult CreateCharacter(
			const PhysicsCharacterCreateRequest& request,
			PhysicsBackendStagingToken& staging) override;
		PhysicsBackendResult UpdateCharacter(const PhysicsCharacterUpdateRequest& request) override;
		PhysicsBackendResult DestroyCharacter(PhysicsCharacterHandle handle) override;

		PhysicsBackendResult ApplyCommand(const PhysicsCommand& command) override;
		PhysicsBackendResult Step(const PhysicsBackendStepRequest& request) override;
		PhysicsBackendQueryResult Query(const PhysicsBackendQueryRequest& request) const override;
		PhysicsBackendResult QueryBatch(
			std::span<const PhysicsBackendQueryRequest> requests,
			std::span<PhysicsBackendQueryResult> results) const override;
		PhysicsBackendResult Extract(PhysicsBackendExtraction& extraction) const override;
		PhysicsBackendResult Extract(
			const PhysicsBackendExtractionRequest& request,
			PhysicsBackendExtraction& extraction) const override;
		PhysicsBackendResult Extract(
			const PhysicsBackendFixedStepWorkProfile& profile,
			PhysicsBackendExtraction& extraction,
			PhysicsBackendExtractionToken& token) const override;
		PhysicsBackendResult AcknowledgeExtraction(PhysicsBackendExtractionToken token) override;
		PhysicsBackendResult AbortExtraction(PhysicsBackendExtractionToken token) override;
		PhysicsBackendExtractionCapabilities ExtractionCapabilities() const noexcept override;
		PhysicsBackendJobTelemetry JobTelemetry() const noexcept override;
		PhysicsBackendSimulationQuality AppliedSimulationQuality() const noexcept override;

		PhysicsBackendResult CommitStaging(PhysicsBackendStagingToken staging) override;
		PhysicsBackendResult RollbackStaging(PhysicsBackendStagingToken staging) override;
		bool IsHealthy() const noexcept override;

		PhysicsAllocationDiagnostics AllocationDiagnostics() const noexcept;
		// This is the narrow lifecycle diagnostic exposed to shutdown validators.
		// It reports adapter-owned queues and pumps, never provider job objects.
		PhysicsBackendJobDrainDiagnostics JobDiagnostics() const noexcept;
		PhysicsProviderError LastProviderError() const;
		// Exercises fatal containment through the same allocator diagnostic observed
		// in production, without depending on an unsafe process-wide OOM condition.
		void ReportFatalProviderFailureForValidation() noexcept;

	private:
		// QueryLocked is shared by single and batched calls. The caller holds the
		// backend read lock so a batch does not reacquire it for every query.
		PhysicsBackendQueryResult QueryLocked(const PhysicsBackendQueryRequest& request) const;
		// A fixed-step profile has already produced a sorted stable selection while
		// holding the membership lock. Reusing it here avoids range encoding, another
		// sort, and a lock hand-off before the retryable lease is retained.
		PhysicsBackendResult ExtractLocked(
			const PhysicsBackendExtractionRequest& request,
			PhysicsBackendExtraction& extraction,
			std::span<const std::uint32_t> preparedStableIndices,
			bool usePreparedSelection,
			bool membershipLockHeld) const;
		struct Impl;
		Impl* impl = nullptr;
	};
}
