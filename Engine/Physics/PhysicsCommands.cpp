// PhysicsCommands.cpp - deterministic command admission, merge, and lifecycle.

#include "PhysicsCommands.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Physics {
	bool CharacterMotionOwner::IsValid() const noexcept {
		return sceneLifetime != 0U && objectId != TerraEngine::EMPTY_UUID
			&& objectGeneration != 0U && attachmentGeneration != 0U
			&& slotGeneration != 0U;
	}

	bool IsCharacterMotionCommand(const PhysicsCommand& command) noexcept {
		return command.type == PhysicsCommandType::MoveCharacter
			|| command.type == PhysicsCommandType::JumpCharacter
			|| command.type == PhysicsCommandType::ResizeCharacter
			|| command.type == PhysicsCommandType::UpdateCharacter
			|| (command.type == PhysicsCommandType::Teleport && command.character.IsValid());
	}

	CharacterMotionLeaseError ValidateCharacterMotionLeaseChange(
		const CharacterMotionLeaseChange& change, const CharacterMotionLease& current) noexcept {
		if(change.expected != current) return CharacterMotionLeaseError::StaleEpoch;
		const CharacterMotionLease& next = change.next;
		if(next.controller != current.controller || !next.controller.character.IsValid()
			|| next.controller.world != PhysicsWorldHandle{next.controller.character.worldId,
				next.controller.character.worldGeneration}
			|| next.controller.objectId == TerraEngine::EMPTY_UUID
			|| next.controller.objectGeneration == 0U || next.controller.componentGeneration == 0U
			|| next.motionEpoch <= current.motionEpoch
			|| next.publicationEpoch <= current.publicationEpoch) {
			return CharacterMotionLeaseError::InvalidArgument;
		}
		// Locomotion authority epochs belong to one attachment lifetime. A new
		// attachment may start at one even though this physical controller keeps
		// a higher tombstone motion epoch from its previous owner.
		if(next.state == CharacterMotionLeaseState::LocomotionOwned
			&& (next.authorityEpoch == 0U
				|| (current.state == CharacterMotionLeaseState::LocomotionOwned
					&& next.owner == current.owner && next.authorityEpoch <= current.authorityEpoch)))
			return CharacterMotionLeaseError::StaleEpoch;
		// This ordinary publication surface cannot release another subsystem's
		// exclusion. The external owner will need a separate protected handoff
		// operation bound to its live ragdoll/external identity.
		if(current.state == CharacterMotionLeaseState::ExternalExclusive)
			return CharacterMotionLeaseError::OwnershipConflict;
		if(next.state == CharacterMotionLeaseState::LocomotionOwned) {
			if(!next.owner.IsValid() || next.owner.objectId != next.controller.objectId
				|| next.owner.objectGeneration != next.controller.objectGeneration
				|| next.externalOwnerId != 0U) return CharacterMotionLeaseError::InvalidArgument;
		} else if(next.state == CharacterMotionLeaseState::ExternalExclusive) {
			if(next.externalOwnerId == 0U) return CharacterMotionLeaseError::InvalidArgument;
		} else if(next.state == CharacterMotionLeaseState::Unleased) {
			if(next.owner.IsValid() || next.externalOwnerId != 0U) return CharacterMotionLeaseError::InvalidArgument;
		} else {
			return CharacterMotionLeaseError::InvalidArgument;
		}
		return CharacterMotionLeaseError::None;
	}

	CharacterMotionLeaseError ValidateCharacterExternalRecoveryTransition(
		const CharacterExternalRecoveryTransition& transition,
		const CharacterMotionLease& current) noexcept {
		const CharacterExternalRecoveryIntent& intent = transition.intent;
		const CharacterMotionLease& next = transition.releasedLease;
		if(intent.expectedExternalLease != current)
			return CharacterMotionLeaseError::StaleEpoch;
		if(current.state != CharacterMotionLeaseState::ExternalExclusive
			|| current.externalOwnerId == 0U
			|| intent.externalOwnerId != current.externalOwnerId)
			return CharacterMotionLeaseError::OwnershipConflict;
		if(!current.controller.character.IsValid() || current.controller.objectId == TerraEngine::EMPTY_UUID
			|| current.controller.objectGeneration == 0U || current.controller.componentGeneration == 0U
			|| intent.bindingGeneration == 0U
			|| !intent.bindingGuard
			|| intent.frame != PhysicsFrameHandle{kPrimaryPhysicsFrameId,
				current.controller.world.worldGeneration}
			|| !intent.alignedControllerPose.IsFinite())
			return CharacterMotionLeaseError::InvalidArgument;
		const PhysicsQuaternion& rotation = intent.alignedControllerPose.rotation;
		const double lengthSquared = static_cast<double>(rotation.x) * rotation.x
			+ static_cast<double>(rotation.y) * rotation.y
			+ static_cast<double>(rotation.z) * rotation.z
			+ static_cast<double>(rotation.w) * rotation.w;
		if(!std::isfinite(lengthSquared) || std::fabs(lengthSquared - 1.0) > 0.001)
			return CharacterMotionLeaseError::InvalidArgument;
		if(next.controller != current.controller || next.state != CharacterMotionLeaseState::Unleased
			|| next.owner.IsValid() || next.externalOwnerId != 0U
			|| next.mode != CharacterMotionMode::Velocity
			|| next.authorityEpoch <= current.authorityEpoch
			|| next.motionEpoch <= current.motionEpoch
			|| next.publicationEpoch <= current.publicationEpoch)
			return CharacterMotionLeaseError::InvalidArgument;
		return CharacterMotionLeaseError::None;
	}

	CharacterMotionLeaseError ValidateCharacterExternalCarry(
		const CharacterExternalCarry& carry) noexcept {
		const CharacterMotionLease& old = carry.expectedExternalLease;
		const CharacterMotionLease& next = carry.nextExternalLease;
		if(old.state != CharacterMotionLeaseState::ExternalExclusive
			|| old.externalOwnerId == 0U || next.state != CharacterMotionLeaseState::ExternalExclusive
			|| next.externalOwnerId != old.externalOwnerId || next.owner.IsValid()
			|| next.controller.world != old.controller.world
			|| next.controller.objectId != old.controller.objectId
			|| next.controller.objectGeneration != old.controller.objectGeneration
			|| next.controller.componentGeneration == old.controller.componentGeneration
			|| next.controller.character == old.controller.character
			|| !next.controller.character.IsValid()
			|| next.authorityEpoch <= old.authorityEpoch
			|| next.motionEpoch <= old.motionEpoch
			|| next.publicationEpoch <= old.publicationEpoch)
			return CharacterMotionLeaseError::InvalidArgument;
		return CharacterMotionLeaseError::None;
	}

	namespace {
		bool RequiresObjectTarget(PhysicsCommandType type) {
			switch(type) {
			case PhysicsCommandType::ScheduleQuery:
			case PhysicsCommandType::CancelQuery:
			case PhysicsCommandType::UpdateWorldSettings:
			case PhysicsCommandType::ReplaceWorld:
				return false;
			default:
				return true;
			}
		}

		std::optional<PhysicsForceMode> ExpectedForceMode(PhysicsCommandType type) {
			switch(type) {
			case PhysicsCommandType::ApplyForce: return PhysicsForceMode::Force;
			case PhysicsCommandType::ApplyAcceleration: return PhysicsForceMode::Acceleration;
			case PhysicsCommandType::ApplyImpulse: return PhysicsForceMode::Impulse;
			case PhysicsCommandType::ApplyVelocityChange: return PhysicsForceMode::VelocityChange;
			default: return std::nullopt;
			}
		}

		PhysicsCommand MakeBodyCommand(
			const PhysicsBodyCommandTarget& target,
			PhysicsCommandType type,
			PhysicsCommandPayload payload,
			PhysicsCommandMetadata metadata) {
			PhysicsCommand command;
			command.type = type;
			command.worldId = target.world.worldId;
			command.worldGeneration = target.world.worldGeneration;
			command.targetObjectId = target.objectId;
			command.objectGeneration = target.objectGeneration;
			command.componentGeneration = target.componentGeneration;
			command.effectiveTick = metadata.effectiveTick;
			command.producerId = metadata.producerId;
			command.producerSequence = metadata.producerSequence;
			command.protectedBoundarySequence = metadata.protectedBoundarySequence;
			command.correlationId = std::move(metadata.correlationId);
			command.idempotencyKey = std::move(metadata.idempotencyKey);
			command.requestFingerprint = std::move(metadata.requestFingerprint);
			command.cancellationToken = metadata.cancellationToken;
			command.payload = std::move(payload);
			return command;
		}

		PhysicsCommand MakeConstraintCommand(
			const PhysicsConstraintCommandTarget& target,
			PhysicsCommandType type,
			PhysicsCommandPayload payload,
			PhysicsCommandMetadata metadata) {
			PhysicsCommand command;
			command.type = type;
			command.worldId = target.world.worldId;
			command.worldGeneration = target.world.worldGeneration;
			command.targetObjectId = target.ownerObjectId;
			command.objectGeneration = target.ownerObjectGeneration;
			command.componentGeneration = target.componentGeneration;
			command.recordId = target.recordId;
			command.recordGeneration = target.recordGeneration;
			command.constraint = target.constraint;
			command.effectiveTick = metadata.effectiveTick;
			command.producerId = metadata.producerId;
			command.producerSequence = metadata.producerSequence;
			command.protectedBoundarySequence = metadata.protectedBoundarySequence;
			command.correlationId = std::move(metadata.correlationId);
			command.idempotencyKey = std::move(metadata.idempotencyKey);
			command.requestFingerprint = std::move(metadata.requestFingerprint);
			command.cancellationToken = metadata.cancellationToken;
			command.payload = std::move(payload);
			return command;
		}

		PhysicsCommand MakeCharacterCommand(
			const PhysicsCharacterCommandTarget& target,
			PhysicsCommandType type,
			PhysicsCommandPayload payload,
			PhysicsCommandMetadata metadata) {
			PhysicsCommand command;
			command.type = type;
			command.worldId = target.world.worldId;
			command.worldGeneration = target.world.worldGeneration;
			command.targetObjectId = target.objectId;
			command.objectGeneration = target.objectGeneration;
			command.componentGeneration = target.componentGeneration;
			// A character handle is runtime-only and copied only into this deferred
			// packet; authoring retains configuration, not provider-facing handles.
			command.character = target.character;
			command.effectiveTick = metadata.effectiveTick;
			command.producerId = metadata.producerId;
			command.producerSequence = metadata.producerSequence;
			command.protectedBoundarySequence = metadata.protectedBoundarySequence;
			command.correlationId = std::move(metadata.correlationId);
			command.idempotencyKey = std::move(metadata.idempotencyKey);
			command.requestFingerprint = std::move(metadata.requestFingerprint);
			command.cancellationToken = metadata.cancellationToken;
			command.payload = std::move(payload);
			return command;
		}

		bool StableUuidLess(const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		bool CanonicalCommandLess(const PhysicsCommand& left, const PhysicsCommand& right) {
			if(left.effectiveTick != right.effectiveTick) {
				return left.effectiveTick < right.effectiveTick;
			}

			const bool leftHasBoundarySequence = left.protectedBoundarySequence != 0;
			const bool rightHasBoundarySequence = right.protectedBoundarySequence != 0;
			if(leftHasBoundarySequence != rightHasBoundarySequence) {
				// Commands already sequenced by a protected producer precede worker
				// batches that require canonical sequencing at this boundary.
				return leftHasBoundarySequence;
			}
			if(leftHasBoundarySequence
				&& left.protectedBoundarySequence != right.protectedBoundarySequence) {
				return left.protectedBoundarySequence < right.protectedBoundarySequence;
			}
			if(!leftHasBoundarySequence) {
				if(left.producerId != right.producerId) {
					return left.producerId < right.producerId;
				}
				if(left.producerSequence != right.producerSequence) {
					return left.producerSequence < right.producerSequence;
				}
			}

			const std::uint8_t leftPriority = PhysicsCommandTypePriority(left.type);
			const std::uint8_t rightPriority = PhysicsCommandTypePriority(right.type);
			if(leftPriority != rightPriority) {
				return leftPriority < rightPriority;
			}
			if(left.targetObjectId != right.targetObjectId) {
				return StableUuidLess(left.targetObjectId, right.targetObjectId);
			}
			if(left.recordId != right.recordId) {
				return StableUuidLess(left.recordId, right.recordId);
			}
			return std::tie(left.objectGeneration, left.componentGeneration, left.recordGeneration,
				left.producerId, left.producerSequence, left.commandId)
				< std::tie(right.objectGeneration, right.componentGeneration, right.recordGeneration,
					right.producerId, right.producerSequence, right.commandId);
		}
	}

	struct PhysicsCommandQueue::Impl {
		struct Entry {
			PhysicsCommand command;
			std::string idempotencyScope;
		};

		struct IdempotencyRecord {
			std::string fingerprint;
			PhysicsCommandId commandId = 0;
			bool terminal = false;
		};

		bool ValidateCommand(const PhysicsCommand& command) const {
			if(!command.worldId.IsValid()
				|| command.worldGeneration == 0U
				|| command.producerSequence == 0U) {
				return false;
			}
			if(RequiresObjectTarget(command.type)
				&& command.targetObjectId == TerraEngine::EMPTY_UUID) {
				return false;
			}
			if(!command.idempotencyKey.empty() && command.requestFingerprint.empty()) {
				return false;
			}
			if(command.correlationId.size() > limits.maximumCorrelationIdBytes
				|| command.idempotencyKey.size() > limits.maximumIdempotencyKeyBytes
				|| command.requestFingerprint.size() > limits.maximumRequestFingerprintBytes) {
				return false;
			}
			if(const std::optional<PhysicsForceMode> expected = ExpectedForceMode(command.type)) {
				const PhysicsForcePayload* payload = std::get_if<PhysicsForcePayload>(&command.payload);
				// Legacy callers may still fill a payload after admission. When a payload
				// is present, however, the envelope must not contradict its command kind.
				if(payload != nullptr && payload->mode != *expected) {
					return false;
				}
			}
			if(command.type == PhysicsCommandType::ResetConstraint) {
				const PhysicsConstraintResetPayload* payload = std::get_if<PhysicsConstraintResetPayload>(&command.payload);
				if(command.recordId == TerraEngine::EMPTY_UUID || command.recordGeneration == 0U
					|| !command.constraint.IsValid() || payload == nullptr || payload->resetGeneration == 0U) return false;
				if(command.constraint.worldId != command.worldId
					|| command.constraint.worldGeneration != command.worldGeneration) return false;
			}
			const bool characterTeleport = command.type == PhysicsCommandType::Teleport
				&& command.character.IsValid();
			if(command.type == PhysicsCommandType::MoveCharacter
				|| command.type == PhysicsCommandType::JumpCharacter
				|| command.type == PhysicsCommandType::ResizeCharacter
				|| characterTeleport) {
				const PhysicsCharacterMotionPayload* payload = std::get_if<PhysicsCharacterMotionPayload>(&command.payload);
				if(payload == nullptr || !command.character.IsValid()) return false;
				if(command.character.worldId != command.worldId
					|| command.character.worldGeneration != command.worldGeneration) return false;
				// Each character command intentionally validates only the fields it
				// consumes. Move, jump, and resize packets carry a default pose over
				// the fixed-width ABI, so requiring a usable rotation there would reject
				// valid gameplay intent before it reaches the protected boundary.
				if(command.type == PhysicsCommandType::MoveCharacter && !payload->desiredVelocity.IsFinite()) return false;
				if(command.type == PhysicsCommandType::JumpCharacter
					&& (!std::isfinite(payload->jumpSpeed) || payload->jumpSpeed < 0.0F)) return false;
				if(command.type == PhysicsCommandType::ResizeCharacter
					&& (!std::isfinite(payload->resizeHeight) || payload->resizeHeight <= 0.0F)) return false;
				if(characterTeleport && !payload->pose.IsFinite()) return false;
			}
			if(command.type == PhysicsCommandType::UpdateCharacter) {
				if(!command.character.IsValid() || command.character.worldId != command.worldId
					|| command.character.worldGeneration != command.worldGeneration) return false;
			}
			return true;
		}

		std::string MakeIdempotencyScope(const PhysicsCommand& command) const {
			return std::to_string(command.worldId.value)
				+ ":" + std::to_string(command.worldGeneration)
				+ ":" + TerraEngine::ToHexString(command.targetObjectId)
				+ ":" + command.idempotencyKey;
		}

		void MarkIdempotencyTerminal(const Entry& entry) {
			if(entry.idempotencyScope.empty()) {
				return;
			}
			const auto record = idempotencyRecords.find(entry.idempotencyScope);
			if(record == idempotencyRecords.end() || record->second.terminal) {
				return;
			}
			record->second.terminal = true;
			terminalIdempotencyOrder.push_back(entry.idempotencyScope);
		}

		bool ReserveIdempotencyCapacity() {
			while(idempotencyRecords.size() >= limits.maximumIdempotencyRecords
				&& !terminalIdempotencyOrder.empty()) {
				const std::string oldest = std::move(terminalIdempotencyOrder.front());
				terminalIdempotencyOrder.pop_front();
				const auto record = idempotencyRecords.find(oldest);
				if(record != idempotencyRecords.end() && record->second.terminal) {
					idempotencyRecords.erase(record);
				}
			}
			return idempotencyRecords.size() < limits.maximumIdempotencyRecords;
		}

		void DecrementProducerDepth(PhysicsSequence producerId) {
			const auto producer = producerDepth.find(producerId);
			if(producer == producerDepth.end()) {
				return;
			}
			if(producer->second > 1U) {
				--producer->second;
			} else {
				producerDepth.erase(producer);
				// Producer ordering state is needed only while that producer has queued
				// commands. Releasing it prevents hostile IDs growing history forever.
				lastProducerSequence.erase(producerId);
			}
		}

		PhysicsCommandQueueLimits limits;
		PhysicsWorldHandle activeWorld;
		bool accepting = true;
		PhysicsCommandId nextCommandId = 1;
		PhysicsSequence nextBoundarySequence = 0;
		std::deque<Entry> entries;
		std::unordered_map<PhysicsSequence, std::size_t> producerDepth;
		std::unordered_map<PhysicsSequence, PhysicsSequence> lastProducerSequence;
		std::unordered_map<std::string, IdempotencyRecord> idempotencyRecords;
		std::deque<std::string> terminalIdempotencyOrder;
		PhysicsCommandQueueDiagnostics diagnostics;
		mutable std::mutex mutex;
	};

	PhysicsCommand PhysicsCommand::MakeWake(
		const TerraEngine::UUID& objectId,
		PhysicsGeneration worldGeneration,
		PhysicsGeneration bodyGeneration) {
		PhysicsCommand command;
		command.type = PhysicsCommandType::WakeBody;
		command.worldId = PhysicsWorldId{1U};
		command.worldGeneration = worldGeneration;
		command.targetObjectId = objectId;
		command.componentGeneration = bodyGeneration;
		return command;
	}

	PhysicsCommand PhysicsCommand::MakeCreateBody(
		const TerraEngine::UUID& objectId,
		PhysicsGeneration worldGeneration) {
		PhysicsCommand command;
		command.type = PhysicsCommandType::CreateBody;
		command.worldId = PhysicsWorldId{1U};
		command.worldGeneration = worldGeneration;
		command.targetObjectId = objectId;
		return command;
	}

	PhysicsCommand PhysicsCommand::MakeApplyForce(
		const TerraEngine::UUID& objectId,
		PhysicsGeneration worldGeneration,
		PhysicsGeneration bodyGeneration) {
		PhysicsCommand command;
		command.type = PhysicsCommandType::ApplyForce;
		command.worldId = PhysicsWorldId{1U};
		command.worldGeneration = worldGeneration;
		command.targetObjectId = objectId;
		command.componentGeneration = bodyGeneration;
		command.payload = PhysicsForcePayload{};
		return command;
	}

	PhysicsCommand PhysicsCommand::MakeContinuousForce(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 force,
		PhysicsCommandMetadata metadata) {
		PhysicsForcePayload payload;
		payload.value = force;
		payload.mode = PhysicsForceMode::Force;
		return MakeBodyCommand(target, PhysicsCommandType::ApplyForce, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeAcceleration(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 acceleration,
		PhysicsCommandMetadata metadata) {
		PhysicsForcePayload payload;
		payload.value = acceleration;
		payload.mode = PhysicsForceMode::Acceleration;
		return MakeBodyCommand(target, PhysicsCommandType::ApplyAcceleration, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeImpulse(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 impulse,
		PhysicsCommandMetadata metadata) {
		PhysicsForcePayload payload;
		payload.value = impulse;
		payload.mode = PhysicsForceMode::Impulse;
		return MakeBodyCommand(target, PhysicsCommandType::ApplyImpulse, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakePointImpulse(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 impulse,
		PhysicsVector3 worldPoint,
		PhysicsCommandMetadata metadata) {
		PhysicsForcePayload payload;
		payload.value = impulse;
		payload.point = worldPoint;
		payload.mode = PhysicsForceMode::Impulse;
		payload.applyAtPoint = true;
		return MakeBodyCommand(target, PhysicsCommandType::ApplyImpulse, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeVelocityChange(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 velocityChange,
		PhysicsCommandMetadata metadata) {
		PhysicsForcePayload payload;
		payload.value = velocityChange;
		payload.mode = PhysicsForceMode::VelocityChange;
		return MakeBodyCommand(target, PhysicsCommandType::ApplyVelocityChange, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeSetVelocity(
		const PhysicsBodyCommandTarget& target,
		PhysicsVector3 linearVelocity,
		PhysicsVector3 angularVelocity,
		PhysicsCommandMetadata metadata) {
		PhysicsMotionPayload payload;
		payload.linearVelocity = linearVelocity;
		payload.angularVelocity = angularVelocity;
		return MakeBodyCommand(target, PhysicsCommandType::SetVelocity, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeTeleport(
		const PhysicsBodyCommandTarget& target,
		PhysicsPose pose,
		PhysicsVector3 linearVelocity,
		PhysicsVector3 angularVelocity,
		PhysicsCommandMetadata metadata) {
		PhysicsMotionPayload payload;
		payload.pose = pose;
		payload.linearVelocity = linearVelocity;
		payload.angularVelocity = angularVelocity;
		return MakeBodyCommand(target, PhysicsCommandType::Teleport, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeKinematicTarget(
		const PhysicsBodyCommandTarget& target,
		PhysicsPose pose,
		float fixedDuration,
		PhysicsCommandMetadata metadata) {
		PhysicsMotionPayload payload;
		payload.pose = pose;
		payload.duration = fixedDuration;
		return MakeBodyCommand(target, PhysicsCommandType::SetKinematicTarget, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeSleep(
		const PhysicsBodyCommandTarget& target,
		PhysicsCommandMetadata metadata) {
		return MakeBodyCommand(target, PhysicsCommandType::SleepBody, {}, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeWake(
		const PhysicsBodyCommandTarget& target,
		PhysicsCommandMetadata metadata) {
		return MakeBodyCommand(target, PhysicsCommandType::WakeBody, {}, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeResetConstraint(
		const PhysicsConstraintCommandTarget& target,
		PhysicsGeneration resetGeneration,
		PhysicsCommandMetadata metadata) {
		return MakeConstraintCommand(target, PhysicsCommandType::ResetConstraint,
			PhysicsConstraintResetPayload{resetGeneration}, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeCharacterMove(
		const PhysicsCharacterCommandTarget& target,
		PhysicsVector3 desiredVelocity,
		PhysicsCommandMetadata metadata) {
		PhysicsCharacterMotionPayload payload;
		payload.desiredVelocity = desiredVelocity;
		return MakeCharacterCommand(target, PhysicsCommandType::MoveCharacter, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeCharacterJump(
		const PhysicsCharacterCommandTarget& target,
		float jumpSpeed,
		PhysicsCommandMetadata metadata) {
		PhysicsCharacterMotionPayload payload;
		payload.jumpSpeed = jumpSpeed;
		return MakeCharacterCommand(target, PhysicsCommandType::JumpCharacter, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeCharacterResize(
		const PhysicsCharacterCommandTarget& target,
		float height,
		PhysicsCommandMetadata metadata) {
		PhysicsCharacterMotionPayload payload;
		payload.resizeHeight = height;
		return MakeCharacterCommand(target, PhysicsCommandType::ResizeCharacter, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeCharacterTeleport(
		const PhysicsCharacterCommandTarget& target,
		PhysicsPose pose,
		PhysicsCommandMetadata metadata) {
		PhysicsCharacterMotionPayload payload;
		payload.pose = pose;
		return MakeCharacterCommand(target, PhysicsCommandType::Teleport, payload, std::move(metadata));
	}

	PhysicsCommand PhysicsCommand::MakeCharacterConfigurationUpdate(
		const PhysicsCharacterCommandTarget& target,
		PhysicsCommandMetadata metadata) {
		return MakeCharacterCommand(target, PhysicsCommandType::UpdateCharacter, {}, std::move(metadata));
	}

	PhysicsCommandQueue::PhysicsCommandQueue(PhysicsCommandQueueLimits limits)
		: impl(new Impl()) {
		impl->limits = limits;
		if(impl->limits.maximumCommands == 0U) {
			impl->limits.maximumCommands = 1U;
		}
		if(impl->limits.maximumCommandsPerProducer == 0U) {
			impl->limits.maximumCommandsPerProducer = 1U;
		}
		if(impl->limits.maximumCommandsPerProducer > impl->limits.maximumCommands) {
			impl->limits.maximumCommandsPerProducer = impl->limits.maximumCommands;
		}
		if(impl->limits.maximumIdempotencyRecords == 0U) {
			impl->limits.maximumIdempotencyRecords = impl->limits.maximumCommands * 2U;
		}
	}

	PhysicsCommandQueue::~PhysicsCommandQueue() {
		delete impl;
	}

	PhysicsCommandReceipt PhysicsCommandQueue::Enqueue(PhysicsCommand command) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		PhysicsCommandReceipt receipt;
		receipt.effectiveTick = command.effectiveTick;
		receipt.cancellationToken = command.cancellationToken;
		if(!impl->accepting) {
			receipt.rejectionReason = PhysicsCommandRejectionReason::QueueStopped;
			++impl->diagnostics.rejectedValidationCount;
			return receipt;
		}

		if(!impl->ValidateCommand(command)) {
			receipt.rejectionReason = (command.correlationId.size() > impl->limits.maximumCorrelationIdBytes
				|| command.idempotencyKey.size() > impl->limits.maximumIdempotencyKeyBytes
				|| command.requestFingerprint.size() > impl->limits.maximumRequestFingerprintBytes)
				? PhysicsCommandRejectionReason::MetadataTooLarge
				: PhysicsCommandRejectionReason::InvalidCommand;
			++impl->diagnostics.rejectedValidationCount;
			return receipt;
		}
		receipt.correlationId = command.correlationId;

		std::string idempotencyScope;
		if(!command.idempotencyKey.empty()) {
			idempotencyScope = impl->MakeIdempotencyScope(command);
			const auto existing = impl->idempotencyRecords.find(idempotencyScope);
			if(existing != impl->idempotencyRecords.end()) {
				receipt.commandId = existing->second.commandId;
				if(existing->second.fingerprint == command.requestFingerprint) {
					receipt.duplicateIdempotency = true;
					++impl->diagnostics.duplicateIdempotencyCount;
				} else {
					receipt.idempotencyConflict = true;
					receipt.rejectionReason = PhysicsCommandRejectionReason::IdempotencyConflict;
					++impl->diagnostics.idempotencyConflictCount;
				}
				return receipt;
			}
		}
		if(impl->entries.size() >= impl->limits.maximumCommands) {
			receipt.rejectionReason = PhysicsCommandRejectionReason::GlobalCapacity;
			++impl->diagnostics.rejectedBackpressureCount;
			return receipt;
		}
		const auto producerDepthEntry = impl->producerDepth.find(command.producerId);
		const std::size_t producerQueued = producerDepthEntry != impl->producerDepth.end()
			? producerDepthEntry->second
			: 0U;
		if(producerQueued >= impl->limits.maximumCommandsPerProducer) {
			receipt.rejectionReason = PhysicsCommandRejectionReason::ProducerCapacity;
			++impl->diagnostics.rejectedBackpressureCount;
			return receipt;
		}
		const auto lastSequence = impl->lastProducerSequence.find(command.producerId);
		if(lastSequence != impl->lastProducerSequence.end()
			&& command.producerSequence <= lastSequence->second) {
			receipt.rejectionReason = PhysicsCommandRejectionReason::ProducerSequence;
			++impl->diagnostics.rejectedValidationCount;
			return receipt;
		}
		if(!idempotencyScope.empty() && !impl->ReserveIdempotencyCapacity()) {
			receipt.rejectionReason = PhysicsCommandRejectionReason::IdempotencyCapacity;
			++impl->diagnostics.rejectedBackpressureCount;
			return receipt;
		}

		command.commandId = impl->nextCommandId++;
		if(impl->nextCommandId == 0U) {
			impl->nextCommandId = 1U;
		}
		receipt.accepted = true;
		receipt.commandId = command.commandId;
		impl->lastProducerSequence[command.producerId] = command.producerSequence;
		impl->producerDepth[command.producerId] = producerQueued + 1U;
		if(!idempotencyScope.empty()) {
			impl->idempotencyRecords.emplace(idempotencyScope, Impl::IdempotencyRecord{
				command.requestFingerprint, command.commandId, false});
		}
		impl->entries.push_back(Impl::Entry{std::move(command), std::move(idempotencyScope)});
		++impl->diagnostics.acceptedCount;
		impl->diagnostics.queuedDepth = impl->entries.size();
		impl->diagnostics.peakDepth = std::max(impl->diagnostics.peakDepth, impl->diagnostics.queuedDepth);
		return receipt;
	}

	bool PhysicsCommandQueue::Cancel(PhysicsCommandId commandId) {
		return CancelWithReceipt(commandId).cancelled;
	}

	PhysicsCommandReceipt PhysicsCommandQueue::CancelWithReceipt(PhysicsCommandId commandId) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		PhysicsCommandReceipt receipt;
		receipt.commandId = commandId;
		const auto entry = std::find_if(impl->entries.begin(), impl->entries.end(), [commandId](const Impl::Entry& candidate) {
			return candidate.command.commandId == commandId;
		});
		if(entry == impl->entries.end()) {
			return receipt;
		}

		receipt.cancelled = true;
		receipt.effectiveTick = entry->command.effectiveTick;
		receipt.cancellationToken = entry->command.cancellationToken;
		receipt.correlationId = entry->command.correlationId;
		impl->DecrementProducerDepth(entry->command.producerId);
		impl->MarkIdempotencyTerminal(*entry);
		impl->entries.erase(entry);
		++impl->diagnostics.cancelledCount;
		impl->diagnostics.queuedDepth = impl->entries.size();
		return receipt;
	}

	PhysicsCommandBatch PhysicsCommandQueue::MergeForProtectedBoundary(
		PhysicsTick inclusiveTick,
		PhysicsGeneration activeWorldGeneration) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		PhysicsCommandBatch batch;
		batch.boundaryTick = inclusiveTick;
		batch.worldGeneration = activeWorldGeneration;

		for(auto entry = impl->entries.begin(); entry != impl->entries.end();) {
			if(entry->command.effectiveTick > inclusiveTick) {
				++entry;
				continue;
			}

			impl->DecrementProducerDepth(entry->command.producerId);
			impl->MarkIdempotencyTerminal(*entry);
			if(entry->command.worldGeneration != activeWorldGeneration
				|| (impl->activeWorld.IsValid() && entry->command.worldId != impl->activeWorld.worldId)) {
				++batch.staleWorldGenerationCount;
				++impl->diagnostics.staleWorldGenerationCount;
			} else {
				batch.commands.push_back(std::move(entry->command));
			}
			entry = impl->entries.erase(entry);
		}

		std::stable_sort(batch.commands.begin(), batch.commands.end(), CanonicalCommandLess);
		for(const PhysicsCommand& command : batch.commands) {
			if(command.protectedBoundarySequence != 0U) {
				impl->nextBoundarySequence = std::max(impl->nextBoundarySequence, command.protectedBoundarySequence);
			}
		}
		for(PhysicsCommand& command : batch.commands) {
			if(command.protectedBoundarySequence == 0U) {
				command.protectedBoundarySequence = ++impl->nextBoundarySequence;
				if(impl->nextBoundarySequence == 0U) {
					command.protectedBoundarySequence = ++impl->nextBoundarySequence;
				}
			}
		}
		impl->diagnostics.queuedDepth = impl->entries.size();
		return batch;
	}

	void PhysicsCommandQueue::StopAccepting() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		impl->accepting = false;
	}

	std::size_t PhysicsCommandQueue::CancelAndDrain() {
		std::lock_guard<std::mutex> lock(impl->mutex);
		const std::size_t cancelled = impl->entries.size();
		for(const Impl::Entry& entry : impl->entries) {
			impl->MarkIdempotencyTerminal(entry);
		}
		impl->entries.clear();
		impl->producerDepth.clear();
		impl->lastProducerSequence.clear();
		impl->diagnostics.cancelledCount += cancelled;
		impl->diagnostics.queuedDepth = 0U;
		return cancelled;
	}

	void PhysicsCommandQueue::ResetForWorld(PhysicsWorldHandle world) {
		std::lock_guard<std::mutex> lock(impl->mutex);
		for(const Impl::Entry& entry : impl->entries) {
			impl->MarkIdempotencyTerminal(entry);
		}
		impl->entries.clear();
		impl->producerDepth.clear();
		impl->lastProducerSequence.clear();
		impl->activeWorld = world;
		impl->accepting = world.IsValid();
		impl->diagnostics.queuedDepth = 0U;
		impl->nextBoundarySequence = 0U;
	}

	bool PhysicsCommandQueue::IsAccepting() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->accepting;
	}

	PhysicsCommandQueueDiagnostics PhysicsCommandQueue::GetDiagnostics() const {
		std::lock_guard<std::mutex> lock(impl->mutex);
		return impl->diagnostics;
	}

	std::uint8_t PhysicsCommandTypePriority(PhysicsCommandType type) noexcept {
		switch(type) {
		case PhysicsCommandType::CreateBody:
		case PhysicsCommandType::CreateConstraint:
		case PhysicsCommandType::CreateCharacter:
			return 10U;
		case PhysicsCommandType::RebuildBody:
		case PhysicsCommandType::RebuildColliderSet:
		case PhysicsCommandType::UpdateConstraint:
		case PhysicsCommandType::UpdateCharacter:
		case PhysicsCommandType::UpdateMaterialFilter:
			return 20U;
		case PhysicsCommandType::EnableBody:
		case PhysicsCommandType::WakeBody:
		case PhysicsCommandType::SleepBody:
		case PhysicsCommandType::Teleport:
		case PhysicsCommandType::SetKinematicTarget:
		case PhysicsCommandType::SetVelocity:
		case PhysicsCommandType::ResetConstraint:
		case PhysicsCommandType::MoveCharacter:
		case PhysicsCommandType::JumpCharacter:
		case PhysicsCommandType::ResizeCharacter:
			return 30U;
		case PhysicsCommandType::ApplyForce:
		case PhysicsCommandType::ApplyAcceleration:
		case PhysicsCommandType::ApplyImpulse:
		case PhysicsCommandType::ApplyVelocityChange:
			return 40U;
		case PhysicsCommandType::ScheduleQuery:
		case PhysicsCommandType::CancelQuery:
			return 50U;
		case PhysicsCommandType::UpdateWorldSettings:
		case PhysicsCommandType::ReplaceWorld:
			return 60U;
		case PhysicsCommandType::DestroyCharacter:
		case PhysicsCommandType::DestroyConstraint:
		case PhysicsCommandType::DestroyBody:
			return 90U;
		}
		return std::numeric_limits<std::uint8_t>::max();
	}

	std::string_view ToString(PhysicsCommandType type) {
		switch(type) {
		case PhysicsCommandType::CreateBody: return "CreateBody";
		case PhysicsCommandType::RebuildBody: return "RebuildBody";
		case PhysicsCommandType::EnableBody: return "EnableBody";
		case PhysicsCommandType::WakeBody: return "WakeBody";
		case PhysicsCommandType::SleepBody: return "SleepBody";
		case PhysicsCommandType::ApplyForce: return "ApplyForce";
		case PhysicsCommandType::ApplyAcceleration: return "ApplyAcceleration";
		case PhysicsCommandType::ApplyImpulse: return "ApplyImpulse";
		case PhysicsCommandType::ApplyVelocityChange: return "ApplyVelocityChange";
		case PhysicsCommandType::SetVelocity: return "SetVelocity";
		case PhysicsCommandType::Teleport: return "Teleport";
		case PhysicsCommandType::SetKinematicTarget: return "SetKinematicTarget";
		case PhysicsCommandType::RebuildColliderSet: return "RebuildColliderSet";
		case PhysicsCommandType::UpdateMaterialFilter: return "UpdateMaterialFilter";
		case PhysicsCommandType::CreateConstraint: return "CreateConstraint";
		case PhysicsCommandType::UpdateConstraint: return "UpdateConstraint";
		case PhysicsCommandType::ResetConstraint: return "ResetConstraint";
		case PhysicsCommandType::CreateCharacter: return "CreateCharacter";
		case PhysicsCommandType::MoveCharacter: return "MoveCharacter";
		case PhysicsCommandType::JumpCharacter: return "JumpCharacter";
		case PhysicsCommandType::ResizeCharacter: return "ResizeCharacter";
		case PhysicsCommandType::UpdateCharacter: return "UpdateCharacter";
		case PhysicsCommandType::ScheduleQuery: return "ScheduleQuery";
		case PhysicsCommandType::CancelQuery: return "CancelQuery";
		case PhysicsCommandType::UpdateWorldSettings: return "UpdateWorldSettings";
		case PhysicsCommandType::ReplaceWorld: return "ReplaceWorld";
		case PhysicsCommandType::DestroyCharacter: return "DestroyCharacter";
		case PhysicsCommandType::DestroyConstraint: return "DestroyConstraint";
		case PhysicsCommandType::DestroyBody: return "DestroyBody";
		}
		return "Unknown";
	}

	std::string_view ToString(PhysicsCommandRejectionReason reason) {
		switch(reason) {
		case PhysicsCommandRejectionReason::None: return "None";
		case PhysicsCommandRejectionReason::QueueStopped: return "QueueStopped";
		case PhysicsCommandRejectionReason::InvalidCommand: return "InvalidCommand";
		case PhysicsCommandRejectionReason::GlobalCapacity: return "GlobalCapacity";
		case PhysicsCommandRejectionReason::ProducerCapacity: return "ProducerCapacity";
		case PhysicsCommandRejectionReason::ProducerSequence: return "ProducerSequence";
		case PhysicsCommandRejectionReason::IdempotencyCapacity: return "IdempotencyCapacity";
		case PhysicsCommandRejectionReason::IdempotencyConflict: return "IdempotencyConflict";
		case PhysicsCommandRejectionReason::MetadataTooLarge: return "MetadataTooLarge";
		}
		return "Unknown";
	}
}
