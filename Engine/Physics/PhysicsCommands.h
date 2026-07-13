// PhysicsCommands.h - bounded typed commands for protected physics mutation.
//
// Any thread may submit an engine-owned command, but only the declared physics
// commit boundary consumes a canonically sorted batch. Worker arrival timing is
// never used as an observable ordering key.

#pragma once

#include "Core/Export.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace TerraEngine::Physics {
	enum class PhysicsCommandType : std::uint8_t {
		CreateBody,
		RebuildBody,
		EnableBody,
		WakeBody,
		SleepBody,
		ApplyForce,
		ApplyAcceleration,
		ApplyImpulse,
		ApplyVelocityChange,
		SetVelocity,
		Teleport,
		SetKinematicTarget,
		RebuildColliderSet,
		UpdateMaterialFilter,
		CreateConstraint,
		UpdateConstraint,
		ResetConstraint,
		CreateCharacter,
		MoveCharacter,
		JumpCharacter,
		ResizeCharacter,
		UpdateCharacter,
		ScheduleQuery,
		CancelQuery,
		UpdateWorldSettings,
		ReplaceWorld,
		DestroyCharacter,
		DestroyConstraint,
		DestroyBody
	};

	enum class PhysicsCommandRejectionReason : std::uint8_t {
		None,
		QueueStopped,
		InvalidCommand,
		GlobalCapacity,
		ProducerCapacity,
		ProducerSequence,
		IdempotencyCapacity,
		IdempotencyConflict,
		MetadataTooLarge
	};

	// A body target carries the two generations required to reject a command
	// that outlives either the physics world or the authored body component.
	// Keeping this provider-neutral data together makes gameplay call sites less
	// likely to accidentally mix a body from one world with another world.
	struct PhysicsBodyCommandTarget {
		PhysicsWorldHandle world;
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
	};

	// Constraint records live inside an owning component. Keeping every identity
	// in the deferred command makes a stale prefab record or recreated world fail
	// at the protected boundary instead of addressing a newly reused provider slot.
	struct PhysicsConstraintCommandTarget {
		PhysicsWorldHandle world;
		PhysicsConstraintHandle constraint;
		TerraEngine::UUID ownerObjectId{};
		PhysicsGeneration ownerObjectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
		TerraEngine::UUID recordId{};
		PhysicsGeneration recordGeneration = 0U;
	};

	// Character authoring stores configuration only. This target is used by
	// one-shot movement, jump, resize, and teleport commands, never by durable
	// component data, so gameplay intent cannot leak into a saved scene.
	struct PhysicsCharacterCommandTarget {
		PhysicsWorldHandle world;
		PhysicsCharacterHandle character;
		TerraEngine::UUID objectId{};
		PhysicsGeneration objectGeneration = 0U;
		PhysicsGeneration componentGeneration = 0U;
	};

	// Producer-owned metadata describes when a command becomes eligible and how
	// an external caller can correlate, deduplicate, or cancel that request.
	// The queue validates its string fields against configured byte limits before
	// retaining them, so untrusted automation cannot grow the command buffer.
	struct PhysicsCommandMetadata {
		PhysicsTick effectiveTick = 0;
		PhysicsSequence producerId = 0;
		PhysicsSequence producerSequence = 0;
		PhysicsSequence protectedBoundarySequence = 0;
		std::string correlationId;
		std::string idempotencyKey;
		std::string requestFingerprint;
		std::uint64_t cancellationToken = 0;
	};

	struct PhysicsForcePayload {
		PhysicsVector3 value;
		PhysicsVector3 point;
		PhysicsForceMode mode = PhysicsForceMode::Force;
		bool applyAtPoint = false;
	};

	struct PhysicsMotionPayload {
		PhysicsPose pose;
		PhysicsVector3 linearVelocity;
		PhysicsVector3 angularVelocity;
		// Kinematic targets need the configured fixed duration so the provider can
		// derive a velocity that reaches the target exactly at the next boundary.
		float duration = 0.0F;
	};

	struct PhysicsCharacterMotionPayload {
		PhysicsVector3 desiredVelocity;
		float jumpSpeed = 0.0F;
		float resizeHeight = 0.0F;
		PhysicsPose pose;
	};

	// A reset token is monotonic authoring intent. A runtime constraint remains
	// broken until a command with a strictly newer token reaches the boundary.
	struct PhysicsConstraintResetPayload {
		PhysicsGeneration resetGeneration = 0U;
	};

	using PhysicsCommandPayload = std::variant<
		std::monostate,
		PhysicsForcePayload,
		PhysicsMotionPayload,
		PhysicsCharacterMotionPayload,
		PhysicsConstraintResetPayload>;

	struct PhysicsCommand {
		PhysicsCommandType type = PhysicsCommandType::ApplyForce;
		PhysicsCommandId commandId = 0;
		PhysicsWorldId worldId;
		PhysicsGeneration worldGeneration = 0;
		TerraEngine::UUID targetObjectId{};
		PhysicsGeneration objectGeneration = 0;
		PhysicsGeneration componentGeneration = 0;
		TerraEngine::UUID recordId{};
		PhysicsGeneration recordGeneration = 0;
		// Runtime handles are copied into deferred packets for stale validation; they
		// are never serialized into constraint or character authoring components.
		PhysicsConstraintHandle constraint;
		PhysicsCharacterHandle character;
		PhysicsTick effectiveTick = 0;
		PhysicsSequence producerId = 0;
		PhysicsSequence producerSequence = 0;
		PhysicsSequence protectedBoundarySequence = 0;
		std::string correlationId;
		std::string idempotencyKey;
		std::string requestFingerprint;
		std::uint64_t cancellationToken = 0;
		PhysicsCommandPayload payload;

		static PhysicsCommand MakeWake(
			const TerraEngine::UUID& objectId,
			PhysicsGeneration worldGeneration,
			PhysicsGeneration bodyGeneration);
		static PhysicsCommand MakeCreateBody(
			const TerraEngine::UUID& objectId,
			PhysicsGeneration worldGeneration);
		static PhysicsCommand MakeApplyForce(
			const TerraEngine::UUID& objectId,
			PhysicsGeneration worldGeneration,
			PhysicsGeneration bodyGeneration);

		// The typed factories below are the preferred native gameplay surface. They
		// set the command kind and force semantics together, so a force mode cannot
		// accidentally be routed to the wrong provider operation. Callers still
		// assign a non-zero producer sequence through PhysicsCommandMetadata before
		// submitting to the bounded multi-producer queue.
		static PhysicsCommand MakeContinuousForce(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 force,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeAcceleration(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 acceleration,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeImpulse(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 impulse,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakePointImpulse(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 impulse,
			PhysicsVector3 worldPoint,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeVelocityChange(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 velocityChange,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeSetVelocity(
			const PhysicsBodyCommandTarget& target,
			PhysicsVector3 linearVelocity,
			PhysicsVector3 angularVelocity,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeTeleport(
			const PhysicsBodyCommandTarget& target,
			PhysicsPose pose,
			PhysicsVector3 linearVelocity = {},
			PhysicsVector3 angularVelocity = {},
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeKinematicTarget(
			const PhysicsBodyCommandTarget& target,
			PhysicsPose pose,
			float fixedDuration,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeSleep(
			const PhysicsBodyCommandTarget& target,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeWake(
			const PhysicsBodyCommandTarget& target,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeResetConstraint(
			const PhysicsConstraintCommandTarget& target,
			PhysicsGeneration resetGeneration,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeCharacterMove(
			const PhysicsCharacterCommandTarget& target,
			PhysicsVector3 desiredVelocity,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeCharacterJump(
			const PhysicsCharacterCommandTarget& target,
			float jumpSpeed,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeCharacterResize(
			const PhysicsCharacterCommandTarget& target,
			float height,
			PhysicsCommandMetadata metadata = {});
		static PhysicsCommand MakeCharacterTeleport(
			const PhysicsCharacterCommandTarget& target,
			PhysicsPose pose,
			PhysicsCommandMetadata metadata = {});
		// Configuration itself stays in the durable component/snapshot. This command
		// only schedules a generation-safe reread at the protected boundary.
		static PhysicsCommand MakeCharacterConfigurationUpdate(
			const PhysicsCharacterCommandTarget& target,
			PhysicsCommandMetadata metadata = {});
	};

	struct PhysicsCommandQueueLimits {
		std::size_t maximumCommands = 4096U;
		std::size_t maximumCommandsPerProducer = 1024U;
		std::size_t maximumIdempotencyRecords = 0U;
		std::size_t maximumCorrelationIdBytes = 256U;
		std::size_t maximumIdempotencyKeyBytes = 128U;
		std::size_t maximumRequestFingerprintBytes = 256U;
	};

	struct PhysicsCommandReceipt {
		bool accepted = false;
		bool duplicateIdempotency = false;
		bool idempotencyConflict = false;
		bool cancelled = false;
		PhysicsCommandId commandId = 0;
		PhysicsTick effectiveTick = 0;
		std::uint64_t cancellationToken = 0;
		std::string correlationId;
		PhysicsCommandRejectionReason rejectionReason = PhysicsCommandRejectionReason::None;
	};

	struct PhysicsCommandQueueDiagnostics {
		std::uint64_t acceptedCount = 0;
		std::uint64_t rejectedBackpressureCount = 0;
		std::uint64_t rejectedValidationCount = 0;
		std::uint64_t duplicateIdempotencyCount = 0;
		std::uint64_t idempotencyConflictCount = 0;
		std::uint64_t cancelledCount = 0;
		std::uint64_t staleWorldGenerationCount = 0;
		std::size_t queuedDepth = 0;
		std::size_t peakDepth = 0;
	};

	struct PhysicsCommandBatch {
		PhysicsTick boundaryTick = 0;
		PhysicsGeneration worldGeneration = 0;
		std::vector<PhysicsCommand> commands;
		std::size_t staleWorldGenerationCount = 0;
	};

	class TERRAENGINE_API PhysicsCommandQueue final {
	public:
		explicit PhysicsCommandQueue(PhysicsCommandQueueLimits limits = {});
		~PhysicsCommandQueue();

		PhysicsCommandQueue(const PhysicsCommandQueue&) = delete;
		PhysicsCommandQueue& operator=(const PhysicsCommandQueue&) = delete;
		PhysicsCommandQueue(PhysicsCommandQueue&&) = delete;
		PhysicsCommandQueue& operator=(PhysicsCommandQueue&&) = delete;

		PhysicsCommandReceipt Enqueue(PhysicsCommand command);
		bool Cancel(PhysicsCommandId commandId);
		// A receipt-capable cancellation path preserves correlation data for native
		// callers that submit work asynchronously. Cancel remains for compatibility.
		PhysicsCommandReceipt CancelWithReceipt(PhysicsCommandId commandId);
		PhysicsCommandBatch MergeForProtectedBoundary(
			PhysicsTick inclusiveTick,
			PhysicsGeneration activeWorldGeneration);
		void StopAccepting();
		std::size_t CancelAndDrain();
		void ResetForWorld(PhysicsWorldHandle world);
		bool IsAccepting() const;
		PhysicsCommandQueueDiagnostics GetDiagnostics() const;

	private:
		// Keep synchronization and Standard Library containers behind an opaque
		// implementation so the exported DLL boundary exposes a stable C++ ABI.
		struct Impl;
		Impl* impl = nullptr;
	};

	TERRAENGINE_API std::uint8_t PhysicsCommandTypePriority(PhysicsCommandType type) noexcept;
	TERRAENGINE_API std::string_view ToString(PhysicsCommandType type);
	TERRAENGINE_API std::string_view ToString(PhysicsCommandRejectionReason reason);
}
