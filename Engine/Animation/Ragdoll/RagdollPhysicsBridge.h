// RagdollPhysicsBridge.h - Typed deferred request queue for the Physics boundary.
#pragma once
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
namespace TerraEngine::Animation {
	struct RagdollProfileAsset;

	enum class RagdollPhysicsRequestKind : std::uint8_t {
		Create,
		Rebuild,
		Activate,
		Deactivate,
		Teardown,
		UpdateMotors
	};
	struct RagdollMotorDrive {
		std::uint64_t constraintId = 0U;
		// The animation blend produces one residual drive contribution for the
		// authored constraint. Physics applies it to every enabled authored axis;
		// the provider-specific target remains owned by the immutable profile.
		float strengthScale = 0.0F;
		auto operator<=>(const RagdollMotorDrive&) const = default;
	};
	struct RagdollPhysicsRequest {
		RagdollPhysicsRequestKind kind = RagdollPhysicsRequestKind::Create;
		std::uint64_t ragdollId = 0U;
		// The target generation names the complete state that becomes visible when
		// this command succeeds. Rebuild and lifecycle commands may additionally
		// name the exact currently visible generation they expect to replace.
		RagdollGenerationStamp generation{};
		RagdollGenerationStamp expectedCurrentGeneration{};
		// The profile is immutable for the lifetime of the queued request. Create
		// requires it; rebuild may replace it; activation can reuse the live copy.
		// A shared immutable value avoids reparsing assets at the protected boundary
		// and never exposes a mutable provider object to animation workers.
		std::shared_ptr<const RagdollProfileAsset> profile;
		PhysicsPoseSnapshot activationPose{};
		std::vector<RagdollMotorDrive> motorDrives;
		std::uint64_t correlationId = 0U;
		std::uint64_t producerOrder = 0U;
		auto operator<=>(const RagdollPhysicsRequest&) const = default;
	};

	// A result is copied at the PhysicsCommandCommit boundary. Consumers can
	// correlate rejection without observing a mutable provider or lifecycle map.
	struct RagdollPhysicsResult {
		std::uint64_t ragdollId = 0U;
		RagdollPhysicsRequestKind kind = RagdollPhysicsRequestKind::Create;
		RagdollGenerationStamp generation{};
		std::uint64_t correlationId = 0U;
		AnimationStatus status = AnimationStatus::NotReady;
		AnimationFixedTick committedStep = 0;
		std::string diagnostic;
	};
	struct RagdollPhysicsResultSnapshot {
		AnimationFixedTick committedStep = 0;
		std::shared_ptr<const std::vector<RagdollPhysicsResult>> results;
	};
	// The bridge has no Jolt dependency. PhysicsSystem must drain this queue once
	// at its protected boundary and atomically publish acknowledgements/snapshots.
	class TERRAENGINE_API RagdollPhysicsBridge final {
	public:
		AnimationStatus Enqueue(RagdollPhysicsRequest request) noexcept;
		// Multi-step lifecycle changes such as first-use create+activate must enter
		// the protected physics boundary as one complete batch. Validation or
		// allocation failure leaves the existing pending queue untouched.
		AnimationStatus EnqueueBatch(
			std::span<const RagdollPhysicsRequest> requests) noexcept;
		AnimationStatus Drain(std::vector<RagdollPhysicsRequest>& output) noexcept;
		// Restores a drained batch when result-allocation fails before a protected
		// boundary can acknowledge it. Moving the vector back is allocation-free.
		void RestoreUndeliveredDrain(std::vector<RagdollPhysicsRequest>&& requests) noexcept;
		// PhysicsSystem calls this only at PhysicsCommandCommit when no complete
		// profile-to-provider binding is available. Every request in the drained
		// batch receives the same rejection, so no lifecycle transition is partial.
		AnimationStatus RejectBatchAtProtectedBoundary(
			std::span<const RagdollPhysicsRequest> requests,
			AnimationFixedTick committedStep,
			std::string diagnostic) noexcept;
		// Successful provider work prepares its immutable acknowledgement before
		// committing staging. Publishing that already-built snapshot cannot allocate,
		// so an out-of-memory error can never leave a committed batch unacknowledged.
		AnimationStatus PrepareResultSnapshot(
			std::span<const RagdollPhysicsRequest> requests,
			AnimationFixedTick committedStep,
			AnimationStatus status,
			std::string diagnostic,
			std::shared_ptr<const RagdollPhysicsResultSnapshot>& output) const noexcept;
		void PublishPreparedResultsAtProtectedBoundary(
			std::shared_ptr<const RagdollPhysicsResultSnapshot> results,
			std::span<const std::uint64_t> retiredPoseIds = {}) noexcept;
		// PhysicsSystem calls this only after one successful provider extraction.
		// The shared immutable allocation is swapped only after all generation
		// checks pass, preserving the previous complete snapshot on rejection.
		AnimationStatus PublishCurrentStepPose(
			std::uint64_t ragdollId,
			PhysicsPoseSnapshot snapshot,
			const RagdollGenerationStamp& expected,
			std::span<const PhysicsPoseExpectedBodyGeneration> expectedBodies) noexcept;
		std::shared_ptr<const PhysicsPoseSnapshot> AcquireCurrentStepPose(
			std::uint64_t ragdollId,
			const RagdollGenerationStamp& expected) const noexcept;
		// Animation may have observed an earlier fixed step while Physics has already
		// published a newer immutable step. Stable ownership generations must match;
		// the returned snapshot carries the exact newer tick for worker validation.
		std::shared_ptr<const PhysicsPoseSnapshot> AcquireLatestCurrentStepPose(
			std::uint64_t ragdollId,
			const RagdollGenerationStamp& expectedIdentity) const noexcept;
		std::shared_ptr<const RagdollPhysicsResultSnapshot> LatestResults() const noexcept;
		std::size_t PendingCount() const noexcept;
		void Clear() noexcept;
	private:
		mutable std::mutex mutex_;
		std::vector<RagdollPhysicsRequest> pending_;
		std::vector<RagdollPhysicsRequest> undelivered_;
		std::unordered_map<std::uint64_t, std::shared_ptr<const PhysicsPoseSnapshot>> currentStepPoses_;
		std::shared_ptr<const RagdollPhysicsResultSnapshot> latestResults_;
	};
}
