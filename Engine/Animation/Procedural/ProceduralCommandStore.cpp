// ProceduralCommandStore.cpp - Deterministic deferred procedural command store.

#include "Animation/Procedural/ProceduralCommandStore.h"

#include "Animation/Procedural/ProceduralTypes.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint32_t kGoalFlagMask = static_cast<std::uint32_t>(NativeAnimationProceduralGoalFlag::UsePole)
			| static_cast<std::uint32_t>(NativeAnimationProceduralGoalFlag::AllowStretch)
			| static_cast<std::uint32_t>(NativeAnimationProceduralGoalFlag::Authoritative)
			| static_cast<std::uint32_t>(NativeAnimationProceduralGoalFlag::Persistent);
		constexpr std::uint32_t kResultFlagMask = static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Converged)
			| static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Clamped)
			| static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Singular)
			| static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::Unreachable)
			| static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::StaleInput)
			| static_cast<std::uint32_t>(NativeAnimationProceduralResultFlag::CpuAuthoritative);

		NativeAnimationAbiHeader Header(NativeAnimationRecordKind kind, std::uint32_t size) noexcept {
			return {size, kNativeAnimationAbiVersion, static_cast<std::uint16_t>(kind)};
		}

		bool IsHeader(const NativeAnimationAbiHeader& header, NativeAnimationRecordKind kind, std::size_t size) noexcept {
			return header.size == size && header.version == kNativeAnimationAbiVersion
				&& header.kind == static_cast<std::uint16_t>(kind);
		}

		bool SameAnimator(NativeAnimationAnimatorHandle left, NativeAnimationAnimatorHandle right) noexcept {
			return left.slotIndex == right.slotIndex && left.generation == right.generation;
		}

		bool SameVector(NativeAnimationVector3 left, NativeAnimationVector3 right) noexcept {
			return left.x == right.x && left.y == right.y && left.z == right.z;
		}

		bool SameQuaternion(NativeAnimationQuaternion left, NativeAnimationQuaternion right) noexcept {
			return left.x == right.x && left.y == right.y && left.z == right.z && left.w == right.w;
		}

		bool SameTransform(NativeAnimationTransform left, NativeAnimationTransform right) noexcept {
			return SameVector(left.translation, right.translation)
				&& SameQuaternion(left.rotation, right.rotation)
				&& SameVector(left.scale, right.scale);
		}

		AnimatorInstanceHandle ToRuntimeHandle(NativeAnimationAnimatorHandle value) noexcept {
			return {value.slotIndex, value.generation};
		}

		bool IsFinite(NativeAnimationVector3 value) noexcept {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		bool IsFinite(NativeAnimationQuaternion value) noexcept {
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
				&& std::isfinite(value.w);
		}

		bool IsNormalized(NativeAnimationQuaternion value) noexcept {
			if(!IsFinite(value)) return false;
			const double lengthSquared = static_cast<double>(value.x) * value.x
				+ static_cast<double>(value.y) * value.y + static_cast<double>(value.z) * value.z
				+ static_cast<double>(value.w) * value.w;
			return std::isfinite(lengthSquared) && std::abs(lengthSquared - 1.0) <= 0.02;
		}

		bool IsFiniteTransform(NativeAnimationTransform value) noexcept {
			return IsFinite(value.translation) && IsNormalized(value.rotation) && IsFinite(value.scale)
				&& std::abs(value.scale.x) > 1.0e-6F && std::abs(value.scale.y) > 1.0e-6F
				&& std::abs(value.scale.z) > 1.0e-6F;
		}

		bool IsSupported(NativeAnimationProceduralCommandKind value) noexcept {
			return value == NativeAnimationProceduralCommandKind::SetGoal
				|| value == NativeAnimationProceduralCommandKind::ClearGoal;
		}

		bool IsSupported(NativeAnimationProceduralGoalKind value) noexcept {
			return static_cast<std::uint32_t>(value) <= static_cast<std::uint32_t>(NativeAnimationProceduralGoalKind::Twist);
		}

		bool IsSupported(NativeAnimationProceduralCoordinateSpace value) noexcept {
			return static_cast<std::uint32_t>(value) <= static_cast<std::uint32_t>(NativeAnimationProceduralCoordinateSpace::World);
		}

		bool IsSupported(NativeAnimationProceduralSolverKind value) noexcept {
			return static_cast<std::uint32_t>(value) <= static_cast<std::uint32_t>(NativeAnimationProceduralSolverKind::PelvisCompensation);
		}

		bool IsSupported(NativeAnimationProceduralSolverState value) noexcept {
			return static_cast<std::uint32_t>(value) <= static_cast<std::uint32_t>(NativeAnimationProceduralSolverState::InvalidInput);
		}

		bool IsSupported(NativeAnimationStatus value) noexcept {
			return static_cast<std::uint16_t>(value) <= static_cast<std::uint16_t>(NativeAnimationStatus::ContentHashMismatch);
		}

		bool IsTerminal(NativeAnimationStatus status) noexcept {
			return status != NativeAnimationStatus::Queued && status != NativeAnimationStatus::NotReady;
		}

		NativeAnimationStatus ToNative(AnimationStatus status) noexcept {
			return static_cast<NativeAnimationStatus>(static_cast<std::uint16_t>(status));
		}

		bool SameCommand(const NativeAnimationProceduralGoalCommand& left,
			const NativeAnimationProceduralGoalCommand& right) noexcept {
			// Compare authored fields instead of object bytes. This keeps idempotency
			// deterministic without making ABI padding or signed zero observable.
			return left.header.size == right.header.size
				&& left.header.version == right.header.version
				&& left.header.kind == right.header.kind
				&& SameAnimator(left.animator, right.animator)
				&& left.command == right.command && left.goalKind == right.goalKind
				&& left.coordinateSpace == right.coordinateSpace
				&& left.layerIndex == right.layerIndex && left.goalId == right.goalId
				&& left.targetJointStableId == right.targetJointStableId
				&& left.targetObjectId.first == right.targetObjectId.first
				&& left.targetObjectId.second == right.targetObjectId.second
				&& left.targetObjectGeneration == right.targetObjectGeneration
				&& left.targetComponentGeneration == right.targetComponentGeneration
				&& SameTransform(left.goalTransform, right.goalTransform)
				&& SameVector(left.poleVector, right.poleVector)
				&& left.priority == right.priority
				&& left.positionWeight == right.positionWeight
				&& left.rotationWeight == right.rotationWeight
				&& left.poleWeight == right.poleWeight
				&& left.maximumStretchScale == right.maximumStretchScale
				&& left.flags == right.flags && left.producerId == right.producerId
				&& left.producerSequence == right.producerSequence
				&& left.correlationId == right.correlationId
				&& left.idempotencyKey == right.idempotencyKey
				&& left.cancellationToken == right.cancellationToken
				&& left.targetTick == right.targetTick
				&& left.expectedPublicationGeneration == right.expectedPublicationGeneration
				&& left.expectedPoseGeneration == right.expectedPoseGeneration
				&& left.reserved == right.reserved;
		}

		bool HasZeroClearPayload(const NativeAnimationProceduralGoalCommand& command) noexcept {
			const NativeAnimationTransform zeroTransform{};
			const NativeAnimationVector3 zeroVector{};
			return static_cast<std::uint32_t>(command.goalKind) == 0U
				&& static_cast<std::uint32_t>(command.coordinateSpace) == 0U
				&& command.layerIndex == 0U && command.targetJointStableId == 0U
				&& command.targetObjectId.first == 0U && command.targetObjectId.second == 0U
				&& command.targetObjectGeneration == 0U && command.targetComponentGeneration == 0U
				&& SameTransform(command.goalTransform, zeroTransform)
				&& SameVector(command.poleVector, zeroVector) && command.priority == 0
				&& command.positionWeight == 0.0F && command.rotationWeight == 0.0F
				&& command.poleWeight == 0.0F && command.maximumStretchScale == 0.0F
				&& static_cast<std::uint32_t>(command.flags) == 0U;
		}

		NativeAnimationProceduralGoalReceipt MakeReceipt(
			const NativeAnimationProceduralGoalCommand& command,
			NativeAnimationStatus status) noexcept {
			NativeAnimationProceduralGoalReceipt receipt{};
			receipt.header = Header(NativeAnimationRecordKind::ProceduralGoalReceipt,
				static_cast<std::uint32_t>(sizeof(receipt)));
			receipt.status = status;
			receipt.animator = command.animator;
			receipt.command = command.command;
			receipt.producerId = command.producerId;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.idempotencyKey = command.idempotencyKey;
			receipt.cancellationToken = command.cancellationToken;
			receipt.goalId = command.goalId;
			receipt.observableTick = command.targetTick;
			return receipt;
		}

		bool IsValidCommand(const NativeAnimationProceduralGoalCommand& command) noexcept {
			if(!IsHeader(command.header, NativeAnimationRecordKind::ProceduralGoalCommand, sizeof(command))
				|| !command.animator.generation || !IsSupported(command.command) || command.goalId == 0U
				|| command.producerId == 0U || command.producerSequence == 0U
				|| command.correlationId == 0U || command.targetTick < 0
				|| command.reserved != 0U) {
				return false;
			}
			if(command.command == NativeAnimationProceduralCommandKind::ClearGoal) {
				// Clear deliberately accepts an absent generation precondition: it still
				// targets the animator's generation-safe handle and only removes one id.
				// All ignored payload must remain zero so future ABI versions can safely
				// assign meaning to those bytes without accepting ambiguous old requests.
				return HasZeroClearPayload(command);
			}
			if(!IsSupported(command.goalKind) || !IsSupported(command.coordinateSpace)
				|| command.targetJointStableId == 0U || command.expectedPoseGeneration == 0U
				|| command.expectedPublicationGeneration == 0U || !IsFiniteTransform(command.goalTransform)
				|| !IsFinite(command.poleVector) || !std::isfinite(command.positionWeight)
				|| !std::isfinite(command.rotationWeight) || !std::isfinite(command.poleWeight)
				|| !std::isfinite(command.maximumStretchScale)
				|| command.positionWeight < 0.0F || command.positionWeight > 1.0F
				|| command.rotationWeight < 0.0F || command.rotationWeight > 1.0F
				|| command.poleWeight < 0.0F || command.poleWeight > 1.0F
				|| command.maximumStretchScale < 1.0F || command.maximumStretchScale > 4.0F
				|| (static_cast<std::uint32_t>(command.flags) & ~kGoalFlagMask) != 0U) {
				return false;
			}
			const bool hasObject = command.targetObjectId.first != 0U || command.targetObjectId.second != 0U;
			return !hasObject || (command.targetObjectGeneration != 0U && command.targetComponentGeneration != 0U);
		}

		bool IsValidResult(const NativeAnimationProceduralResultSnapshot& result,
			NativeAnimationAnimatorHandle animator) noexcept {
			return IsHeader(result.header, NativeAnimationRecordKind::ProceduralResultSnapshot, sizeof(result))
				&& IsSupported(result.status) && SameAnimator(result.animator, animator)
				&& IsSupported(result.solver) && IsSupported(result.state) && result.goalId != 0U
				&& result.targetJointStableId != 0U && IsFiniteTransform(result.resultTransform)
				&& std::isfinite(result.targetErrorMeters) && result.targetErrorMeters >= 0.0F
				&& std::isfinite(result.targetErrorRadians) && result.targetErrorRadians >= 0.0F
				&& std::isfinite(result.inputWeight) && result.inputWeight >= 0.0F && result.inputWeight <= 1.0F
				&& std::isfinite(result.finalContribution) && result.finalContribution >= 0.0F
				&& result.finalContribution <= 1.0F && result.iterationCount <= result.iterationLimit
				&& result.poseGeneration != 0U && result.componentGeneration != 0U
				&& result.skeletonGeneration != 0U && result.programGeneration != 0U
				&& result.observationTick >= 0 && result.publicationGeneration != 0U
				&& result.reserved == 0U && result.reserved2 == 0U
				&& (static_cast<std::uint32_t>(result.flags) & ~kResultFlagMask) == 0U;
		}

		bool CommandLess(const NativeAnimationProceduralGoalCommand& left,
			const NativeAnimationProceduralGoalCommand& right) noexcept {
			return std::tie(left.targetTick, left.animator.slotIndex, left.animator.generation,
				left.priority, left.producerId, left.producerSequence, left.goalId, left.correlationId)
				< std::tie(right.targetTick, right.animator.slotIndex, right.animator.generation,
					right.priority, right.producerId, right.producerSequence, right.goalId, right.correlationId);
		}
	}

	ProceduralCommandStore::ProceduralCommandStore(
		std::size_t commandCapacity,
		std::size_t receiptCapacity,
		std::size_t retainedResultCapacity)
		: commandCapacity_(std::max<std::size_t>(1U, commandCapacity))
		, receiptCapacity_(std::max<std::size_t>(1U, receiptCapacity))
		, retainedResultCapacity_(std::max<std::size_t>(1U, retainedResultCapacity)) {}

	NativeAnimationProceduralGoalReceipt ProceduralCommandStore::Submit(
		const NativeAnimationProceduralGoalCommand& command,
		const AnimatorInstanceStore& instances) {
		NativeAnimationProceduralGoalReceipt receipt = MakeReceipt(command, NativeAnimationStatus::Queued);
		if(!IsValidCommand(command)) {
			receipt.status = NativeAnimationStatus::InvalidArgument;
			return receipt;
		}
		if(!instances.IsAlive(ToRuntimeHandle(command.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
			return receipt;
		}
		try {
			std::scoped_lock lock(mutex_);
			if(command.idempotencyKey != 0U) {
				const auto repeated = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
					return record.command.producerId == command.producerId
						&& record.command.idempotencyKey == command.idempotencyKey;
				});
				if(repeated != receipts_.end()) {
					receipt = repeated->receipt;
					receipt.duplicate = 1U;
					if(!SameCommand(repeated->command, command)) {
						receipt.status = NativeAnimationStatus::Rejected;
						receipt.idempotencyConflict = 1U;
					}
					return receipt;
				}
			}
			if(pending_.size() >= commandCapacity_) {
				receipt.status = NativeAnimationStatus::HardLimitExceeded;
				return receipt;
			}
			if(std::any_of(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
				return record.command.producerId == command.producerId
					&& record.command.producerSequence == command.producerSequence;
			}) || (command.cancellationToken != 0U && std::any_of(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
				return record.command.cancellationToken == command.cancellationToken;
			}))) {
				receipt.status = NativeAnimationStatus::Rejected;
				return receipt;
			}
			if(receipts_.size() >= receiptCapacity_) ReclaimTerminalReceiptLocked();
			if(receipts_.size() >= receiptCapacity_) {
				receipt.status = NativeAnimationStatus::HardLimitExceeded;
				return receipt;
			}
			receipts_.push_back({command, receipt});
			pending_.push_back(command);
			return receipt;
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::InternalError;
			return receipt;
		} catch(...) {
			receipt.status = NativeAnimationStatus::InternalError;
			return receipt;
		}
	}

	NativeAnimationProceduralGoalReceipt ProceduralCommandStore::Cancel(
		std::uint64_t cancellationToken,
		std::uint64_t correlationId) {
		NativeAnimationProceduralGoalCommand placeholder{};
		placeholder.correlationId = correlationId;
		placeholder.cancellationToken = cancellationToken;
		NativeAnimationProceduralGoalReceipt missing = MakeReceipt(placeholder, NativeAnimationStatus::InvalidArgument);
		if(cancellationToken == 0U || correlationId == 0U) return missing;

		std::scoped_lock lock(mutex_);
		const auto receipt = std::find_if(receipts_.begin(), receipts_.end(), [cancellationToken](const ReceiptRecord& record) {
			return record.command.cancellationToken == cancellationToken;
		});
		if(receipt == receipts_.end()) {
			missing.status = NativeAnimationStatus::NotReady;
			return missing;
		}
		if(IsTerminal(receipt->receipt.status)) return receipt->receipt;
		const auto pending = std::find_if(pending_.begin(), pending_.end(), [cancellationToken](const auto& command) {
			return command.cancellationToken == cancellationToken;
		});
		if(pending == pending_.end()) {
			NativeAnimationProceduralGoalReceipt tooLate = receipt->receipt;
			tooLate.status = NativeAnimationStatus::NotReady;
			tooLate.correlationId = correlationId;
			return tooLate;
		}
		pending_.erase(pending);
		receipt->receipt.status = NativeAnimationStatus::Cancelled;
		NativeAnimationProceduralGoalReceipt cancelled = receipt->receipt;
		cancelled.correlationId = correlationId;
		return cancelled;
	}

	NativeAnimationProceduralGoalReceipt ProceduralCommandStore::ReadReceipt(
		std::uint32_t producerId,
		std::uint64_t producerSequence,
		std::uint64_t correlationId) const {
		NativeAnimationProceduralGoalCommand placeholder{};
		placeholder.producerId = producerId;
		placeholder.producerSequence = producerSequence;
		placeholder.correlationId = correlationId;
		NativeAnimationProceduralGoalReceipt missing = MakeReceipt(placeholder,
			producerSequence == 0U || correlationId == 0U ? NativeAnimationStatus::InvalidArgument : NativeAnimationStatus::NotReady);
		if(missing.status == NativeAnimationStatus::InvalidArgument) return missing;
		std::scoped_lock lock(mutex_);
		const auto receipt = std::find_if(receipts_.begin(), receipts_.end(), [=](const ReceiptRecord& record) {
			return record.command.producerId == producerId && record.command.producerSequence == producerSequence
				&& record.command.correlationId == correlationId;
		});
		return receipt == receipts_.end() ? missing : receipt->receipt;
	}

	void ProceduralCommandStore::DrainThrough(
		AnimationFixedTick tick,
		std::vector<NativeAnimationProceduralGoalCommand>& output,
		const AnimatorInstanceStore& instances) {
		output.clear();
		std::scoped_lock lock(mutex_);
		std::vector<NativeAnimationProceduralGoalCommand> future;
		future.reserve(pending_.size());
		for(const NativeAnimationProceduralGoalCommand& command : pending_) {
			if(!instances.IsAlive(ToRuntimeHandle(command.animator))) {
				const auto receipt = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
					return record.command.producerId == command.producerId
						&& record.command.producerSequence == command.producerSequence;
				});
				if(receipt != receipts_.end()) {
					receipt->receipt.status = NativeAnimationStatus::StaleHandle;
					receipt->receipt.observableTick = tick;
					receipt->receipt.publicationGeneration = 0U;
				}
				continue;
			}
			if(command.targetTick <= tick) output.push_back(command);
			else future.push_back(command);
		}
		pending_ = std::move(future);
		// Result and active-goal snapshots are never allowed to outlive their
		// animator generation, even when that animator had no pending work this frame.
		states_.erase(std::remove_if(states_.begin(), states_.end(), [&instances](const AnimatorState& state) {
			return !instances.IsAlive(ToRuntimeHandle(state.animator));
		}), states_.end());
		std::sort(output.begin(), output.end(), CommandLess);
	}

	void ProceduralCommandStore::PublishCompletion(
		const NativeAnimationProceduralGoalCommand& command,
		AnimationStatus status,
		AnimationFixedTick observableTick,
		std::uint64_t publicationGeneration) {
		std::scoped_lock lock(mutex_);
		const auto receipt = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
			return record.command.producerId == command.producerId
				&& record.command.producerSequence == command.producerSequence
				&& record.command.correlationId == command.correlationId;
		});
		if(receipt == receipts_.end() || IsTerminal(receipt->receipt.status)) return;

		NativeAnimationStatus finalStatus = ToNative(status);
		if(status == AnimationStatus::Ok) {
			try {
				AnimatorState* state = FindOrCreateStateLocked(command.animator);
				if(state == nullptr) finalStatus = NativeAnimationStatus::InternalError;
				else if(command.command == NativeAnimationProceduralCommandKind::ClearGoal) {
					state->activeGoals.erase(std::remove_if(state->activeGoals.begin(), state->activeGoals.end(), [&command](const auto& goal) {
						return goal.goalId == command.goalId;
					}), state->activeGoals.end());
				} else {
					const auto existing = std::find_if(state->activeGoals.begin(), state->activeGoals.end(), [&command](const auto& goal) {
						return goal.goalId == command.goalId;
					});
					if(existing != state->activeGoals.end()) *existing = command;
					else if(state->activeGoals.size() >= kProceduralGoalHardMaximum) finalStatus = NativeAnimationStatus::HardLimitExceeded;
					else state->activeGoals.push_back(command);
				}
			} catch(...) {
				finalStatus = NativeAnimationStatus::InternalError;
			}
		}
		receipt->receipt.status = finalStatus;
		receipt->receipt.observableTick = observableTick;
		receipt->receipt.publicationGeneration = publicationGeneration;
	}

	AnimationStatus ProceduralCommandStore::CopyActiveGoals(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationProceduralGoalCommand> destination,
		std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		if(animator.generation == 0U) return AnimationStatus::InvalidArgument;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		if(state == nullptr) return AnimationStatus::NotReady;
		if(state->activeGoals.size() > UINT32_MAX) return AnimationStatus::RangeOverflow;
		requiredCount = static_cast<std::uint32_t>(state->activeGoals.size());
		if(destination.size() < state->activeGoals.size()) return AnimationStatus::BufferTooSmall;
		std::copy(state->activeGoals.begin(), state->activeGoals.end(), destination.begin());
		return AnimationStatus::Ok;
	}

	AnimationStatus ProceduralCommandStore::PublishResults(
		NativeAnimationAnimatorHandle animator,
		std::span<const NativeAnimationProceduralResultSnapshot> results,
		const AnimatorInstanceStore& instances) {
		if(animator.generation == 0U) return AnimationStatus::InvalidArgument;
		if(!instances.IsAlive(ToRuntimeHandle(animator))) return AnimationStatus::StaleHandle;
		if(results.size() > retainedResultCapacity_) {
			return AnimationStatus::HardLimitExceeded;
		}
		for(const auto& result : results) if(!IsValidResult(result, animator)) return AnimationStatus::InvalidArgument;
		try {
			std::vector<NativeAnimationProceduralResultSnapshot> candidate(results.begin(), results.end());
			std::sort(candidate.begin(), candidate.end(), [](const auto& left, const auto& right) {
				return std::tie(left.observationTick, left.producerOrder, left.goalId, left.targetJointStableId,
					left.correlationId) < std::tie(right.observationTick, right.producerOrder, right.goalId,
						right.targetJointStableId, right.correlationId);
			});
			if(std::adjacent_find(candidate.begin(), candidate.end(), [](const auto& left, const auto& right) {
				return left.goalId == right.goalId && left.targetJointStableId == right.targetJointStableId
					&& left.producerOrder == right.producerOrder;
			}) != candidate.end()) return AnimationStatus::Rejected;
			std::scoped_lock lock(mutex_);
			AnimatorState* state = FindOrCreateStateLocked(animator);
			if(state == nullptr) return AnimationStatus::InternalError;
			state->results = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus ProceduralCommandStore::CopyResults(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationProceduralResultSnapshot> destination,
		std::uint32_t& requiredCount) const {
		requiredCount = 0U;
		if(animator.generation == 0U) return AnimationStatus::InvalidArgument;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		if(state == nullptr) return AnimationStatus::NotReady;
		if(state->results.size() > UINT32_MAX) return AnimationStatus::RangeOverflow;
		requiredCount = static_cast<std::uint32_t>(state->results.size());
		if(destination.size() < state->results.size()) return AnimationStatus::BufferTooSmall;
		std::copy(state->results.begin(), state->results.end(), destination.begin());
		return AnimationStatus::Ok;
	}

	void ProceduralCommandStore::RetireAnimator(NativeAnimationAnimatorHandle animator) {
		std::scoped_lock lock(mutex_);
		pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&animator](const auto& command) {
			return SameAnimator(command.animator, animator);
		}), pending_.end());
		receipts_.erase(std::remove_if(receipts_.begin(), receipts_.end(), [&animator](const ReceiptRecord& record) {
			return SameAnimator(record.command.animator, animator);
		}), receipts_.end());
		states_.erase(std::remove_if(states_.begin(), states_.end(), [&animator](const AnimatorState& state) {
			return SameAnimator(state.animator, animator);
		}), states_.end());
	}

	void ProceduralCommandStore::Clear() {
		std::scoped_lock lock(mutex_);
		pending_.clear();
		receipts_.clear();
		states_.clear();
	}

	std::size_t ProceduralCommandStore::PendingCount() const {
		std::scoped_lock lock(mutex_);
		return pending_.size();
	}

	ProceduralCommandStore::AnimatorState* ProceduralCommandStore::FindStateLocked(NativeAnimationAnimatorHandle animator) {
		const auto state = std::find_if(states_.begin(), states_.end(), [&animator](const AnimatorState& value) {
			return SameAnimator(value.animator, animator);
		});
		return state == states_.end() ? nullptr : &*state;
	}

	const ProceduralCommandStore::AnimatorState* ProceduralCommandStore::FindStateLocked(NativeAnimationAnimatorHandle animator) const {
		const auto state = std::find_if(states_.begin(), states_.end(), [&animator](const AnimatorState& value) {
			return SameAnimator(value.animator, animator);
		});
		return state == states_.end() ? nullptr : &*state;
	}

	ProceduralCommandStore::AnimatorState* ProceduralCommandStore::FindOrCreateStateLocked(NativeAnimationAnimatorHandle animator) {
		if(AnimatorState* state = FindStateLocked(animator)) return state;
		// One state is the smallest retained unit for an animator. Bounding state
		// count by command capacity prevents a stream of short-lived handles from
		// turning copied receipts/results into an unbounded allocation source.
		if(states_.size() >= commandCapacity_) return nullptr;
		states_.push_back({animator});
		return &states_.back();
	}

	void ProceduralCommandStore::ReclaimTerminalReceiptLocked() {
		const auto terminal = std::find_if(receipts_.begin(), receipts_.end(), [](const ReceiptRecord& record) {
			return IsTerminal(record.receipt.status);
		});
		if(terminal != receipts_.end()) receipts_.erase(terminal);
	}
}
