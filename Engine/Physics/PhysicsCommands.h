// PhysicsCommands.h - bounded typed commands for protected physics mutation.
//
// Any thread may submit an engine-owned command, but only the declared physics
// commit boundary consumes a canonically sorted batch. Worker arrival timing is
// never used as an observable ordering key.

#pragma once

#include "Core/Export.h"
#include "PhysicsTypes.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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
		bool operator==(const PhysicsCharacterCommandTarget&) const = default;
	};

	// The locomotion attachment is identified without importing locomotion or
	// provider headers into Physics. All generations are copied at admission.
	struct CharacterMotionOwner {
		std::uint64_t sceneLifetime = 0U;
		TerraEngine::UUID objectId{};
		std::uint64_t objectGeneration = 0U;
		std::uint64_t attachmentGeneration = 0U;
		std::uint32_t slotIndex = 0U;
		std::uint32_t slotGeneration = 0U;

		[[nodiscard]] bool IsValid() const noexcept;
		bool operator==(const CharacterMotionOwner&) const = default;
	};

	enum class CharacterMotionLeaseState : std::uint8_t { Unleased, LocomotionOwned, ExternalExclusive };
	enum class CharacterMotionMode : std::uint8_t { Velocity, RootMotion };
	enum class CharacterMotionCommandSource : std::uint8_t { Legacy, Locomotion };

	// An unleased record remains present until controller destruction so an old
	// command cannot become valid again when ownership is later released.
	struct CharacterMotionLease {
		PhysicsCharacterCommandTarget controller;
		CharacterMotionOwner owner;
		std::uint64_t authorityEpoch = 0U;
		std::uint64_t motionEpoch = 0U;
		std::uint64_t publicationEpoch = 0U;
		std::uint64_t externalOwnerId = 0U;
		CharacterMotionLeaseState state = CharacterMotionLeaseState::Unleased;
		CharacterMotionMode mode = CharacterMotionMode::Velocity;
		bool operator==(const CharacterMotionLease&) const = default;
	};

	struct CharacterMotionLeaseChange {
		CharacterMotionLease expected;
		CharacterMotionLease next;
	};

	enum class CharacterMotionLeaseError : std::uint8_t {
		None, InvalidArgument, StaleCharacter, StaleEpoch, OwnershipConflict, CapacityExceeded
	};

	struct CharacterMotionLeaseResult {
		bool accepted = false;
		CharacterMotionLeaseError error = CharacterMotionLeaseError::None;
		std::uint64_t publicationEpoch = 0U;
	};

	[[nodiscard]] CharacterMotionLeaseError ValidateCharacterMotionLeaseChange(
		const CharacterMotionLeaseChange& change, const CharacterMotionLease& current) noexcept;

	// A producer may attach immutable evidence whose current validity can be
	// checked at protected Physics boundaries without importing its subsystem.
	enum class CharacterMotionValidityReason : std::uint8_t {
		Valid, StaleLifetime, StaleConfiguration, StaleEvidence, Unavailable
	};

	class TERRAENGINE_API CharacterMotionValidity {
	public:
		virtual ~CharacterMotionValidity() = default;
		[[nodiscard]] virtual CharacterMotionValidityReason Validate() const noexcept = 0;
	};

	// Only PhysicsSystem's trusted protected ingress may publish this target.
	// Legacy callers keep using PhysicsCommand and are stamped as Legacy there.
	struct CharacterMotionTarget {
		PhysicsCharacterCommandTarget controller;
		CharacterMotionOwner owner;
		std::uint64_t authorityEpoch = 0U;
		std::uint64_t motionEpoch = 0U;
		std::uint64_t locomotionUpdateEpoch = 0U;
		CharacterMotionMode mode = CharacterMotionMode::Velocity;
		PhysicsFrameHandle frame;
		PhysicsVector3 planarVelocity;
		float acceleration = 0.0F;
		float braking = 0.0F;
		PhysicsVector3 desiredFacing;
		float turnRate = 0.0F;
		bool hasFacing = false;
		bool clearImmediately = false;
		std::shared_ptr<const CharacterMotionValidity> validityGuard;
		// -1 means no arrival budget. Navigation supplies route distance minus
		// tolerance; Physics spends it only on realized planar fixed-step travel.
		float approachDistanceMetres = -1.0F;
		bool operator==(const CharacterMotionTarget&) const = default;
	};

	// Animation's successful ragdoll recovery supplies one copied, frame-qualified
	// controller pose. This intent is inert until a protected Physics boundary
	// proves the exact external owner and completed ragdoll result.
	struct CharacterExternalRecoveryIntent {
		CharacterMotionLease expectedExternalLease;
		std::uint64_t externalOwnerId = 0U;
		PhysicsFrameHandle frame;
		PhysicsPose alignedControllerPose;
		std::uint64_t bindingGeneration = 0U;
		std::shared_ptr<const CharacterMotionValidity> bindingGuard;
		bool operator==(const CharacterExternalRecoveryIntent&) const = default;
	};

	// A retired animator has no valid binding guard after component invalidation.
	// Its protected producer pins the exact live external lease before retiring
	// the record; only an exact successful ragdoll Teardown may consume this
	// intent and clear the character at its current provider pose.
	struct CharacterExternalRetirementIntent {
		CharacterMotionLease expectedExternalLease;
		std::uint64_t externalOwnerId = 0U;
		PhysicsFrameHandle frame;
		std::uint64_t bindingGeneration = 0U;
	};

	// The provider and lease table receive the same prevalidated transition.
	// A separate recovery path never weakens ordinary ExternalExclusive rules.
	struct CharacterExternalRecoveryTransition {
		CharacterExternalRecoveryIntent intent;
		CharacterMotionLease releasedLease;
	};
	[[nodiscard]] CharacterMotionLeaseError ValidateCharacterExternalRecoveryTransition(
		const CharacterExternalRecoveryTransition& transition,
		const CharacterMotionLease& current) noexcept;

	// A surviving controller may receive a new physical handle when the scene's
	// shared component generation changes. Both full identities are checked in
	// one prepared root; the old ExternalExclusive row is retired, never released
	// through the ordinary locomotion lease API.
	struct CharacterExternalCarry {
		CharacterMotionLease expectedExternalLease;
		CharacterMotionLease nextExternalLease;
	};
	[[nodiscard]] CharacterMotionLeaseError ValidateCharacterExternalCarry(
		const CharacterExternalCarry& carry) noexcept;

	// Animation supplies copied numeric identities and a frame-space delta.
	// Physics does not import an Animation handle or call its application ledger.
	struct RootMotionAnimatorIdentity {
		std::uint32_t slotIndex = 0xffffffffU;
		std::uint32_t animatorGeneration = 0U;
		std::uint32_t poseGeneration = 0U;
		std::uint64_t sourceGeneration = 0U;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		std::uint64_t evaluationSequence = 0U;
		std::uint64_t bindingGeneration = 0U;
		std::uint64_t publicationGeneration = 0U;
		std::int64_t sourceBeginTick = 0;
		std::int64_t sourceEndTick = 0;
		std::int64_t accumulatedCycles = 0;
		bool operator==(const RootMotionAnimatorIdentity&) const = default;
	};

	struct RootMotionLedgerIdentity {
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		std::uint64_t idempotencyKey = 0U;
		std::uint64_t correlationId = 0U;
		bool operator==(const RootMotionLedgerIdentity&) const = default;
	};

	// Translation and yaw are already converted from the captured animator basis
	// into this physical frame exactly once by the protected producer. Local values
	// remain copied receipt evidence and must not be applied a second time.
	struct RootMotionInterval {
		CharacterMotionLease lease;
		PhysicsFrameHandle frame;
		RootMotionAnimatorIdentity animator;
		RootMotionLedgerIdentity ledger;
		PhysicsVector3 localTranslation;
		float localYawRadians = 0.0F;
		PhysicsVector3 frameTranslation;
		float frameYawRadians = 0.0F;
		float durationSeconds = 0.0F;
		float maximumSpeed = 0.0F;
		float maximumTurnRate = 0.0F;
		// Source or binding changes may revoke a retained interval. Physics invokes
		// this guard only at protected boundaries, never inside a provider worker.
		std::shared_ptr<const CharacterMotionValidity> validityGuard;
	};

	// An interval ID belongs to one world generation and never aliases an ordinary
	// PhysicsCommandId. A separate token generation names the reserved capacity.
	struct RootMotionIntervalId {
		PhysicsWorldHandle world;
		std::uint64_t sequence = 0U;
		[[nodiscard]] bool IsValid() const noexcept { return world.IsValid() && sequence != 0U; }
		bool operator==(const RootMotionIntervalId&) const = default;
	};

	struct RootMotionIntervalReservation {
		RootMotionIntervalId intervalId;
		std::uint64_t tokenGeneration = 0U;
		[[nodiscard]] bool IsValid() const noexcept { return intervalId.IsValid() && tokenGeneration != 0U; }
		bool operator==(const RootMotionIntervalReservation&) const = default;
	};

	enum class RootMotionIntervalError : std::uint8_t {
		None, InvalidArgument, StaleTarget, OwnershipConflict, UnsupportedController,
		UnsupportedFrame, Duplicate, OutOfOrder, StaleEvidence, CapacityExceeded,
		NotReady, Cancelled
	};

	struct RootMotionIntervalAdmission {
		bool accepted = false;
		RootMotionIntervalError error = RootMotionIntervalError::NotReady;
		RootMotionIntervalReservation reservation;
	};

	// Every tick record reports one whole fixed-step aggregate. Contributing IDs
	// share its measured result; Physics never invents per-fragment collision data.
	struct RootMotionTickResult {
		PhysicsCharacterCommandTarget controller;
		PhysicsTick tick = 0U;
		float completedStepDuration = 0.0F;
		PhysicsVector3 requestedTranslation;
		PhysicsVector3 realizedTranslation;
		float requestedYawRadians = 0.0F;
		float realizedYawRadians = 0.0F;
		std::array<RootMotionIntervalId, 8U> contributingIntervals{};
		std::uint8_t contributingCount = 0U;
		bool adjustedByLimit = false;
		bool adjustedByCollision = false;
	};

	enum class RootMotionTerminalDisposition : std::uint8_t {
		Consumed, Adjusted, Cancelled, Rejected
	};

	struct RootMotionTerminalResult {
		RootMotionIntervalId intervalId;
		RootMotionAnimatorIdentity animator;
		RootMotionLedgerIdentity ledger;
		RootMotionTerminalDisposition disposition = RootMotionTerminalDisposition::Rejected;
		RootMotionIntervalError reason = RootMotionIntervalError::NotReady;
		PhysicsTick finalTick = 0U;
		std::uint64_t firstTickResultSequence = 0U;
		std::uint64_t lastTickResultSequence = 0U;
		bool sharedTickAttribution = false;
	};

	struct RootMotionResultRecord {
		std::uint64_t sequence = 0U;
		std::variant<RootMotionTickResult, RootMotionTerminalResult> result;
	};

	// A copied immutable page is read by Animation at its protected boundary.
	// The cursor is acknowledged only after its existing ledger has resolved the
	// corresponding terminal applications; no callback runs under a Physics lock.
	struct RootMotionResultPacket {
		PhysicsWorldHandle world;
		std::uint64_t requestedAfterCursor = 0U;
		std::uint64_t nextCursor = 0U;
		bool expiredCursor = false;
		bool hasMore = false;
		std::vector<RootMotionResultRecord> records;
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
		// PhysicsWorld stamps these at queue admission. A caller cannot opt into
		// Locomotion by setting either field on the public legacy command path.
		std::uint64_t characterMotionEpoch = 0U;
		CharacterMotionCommandSource characterMotionSource = CharacterMotionCommandSource::Legacy;
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

	[[nodiscard]] bool IsCharacterMotionCommand(const PhysicsCommand& command) noexcept;

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
