// AnimationCommandQueue.cpp - Deterministic command ordering and bounded validation.

#include "Animation/Runtime/AnimationCommandQueue.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		bool HasExpectedValue(const AnimationCommand& command) {
			switch(command.kind) {
			case AnimationCommandKind::FireTrigger:
				return command.parameterType == AnimationParameterType::Trigger
					&& std::holds_alternative<std::monostate>(command.value);
			case AnimationCommandKind::SetSpeed:
				return std::isfinite(command.playbackSpeed)
					&& command.playbackSpeed >= 0.0F && command.playbackSpeed <= 100.0F;
			case AnimationCommandKind::Play:
				// Zero resumes the already-bound state for ABI-v1 compatibility; a
				// non-zero value requests an explicit controller-state selection.
				return command.playbackTick >= 0;
			case AnimationCommandKind::CrossFade:
				return command.stateId != 0U && command.transitionTicks >= 0;
			case AnimationCommandKind::SetLayerWeight:
				return std::isfinite(command.layerWeight)
					&& command.layerWeight >= 0.0F && command.layerWeight <= 1.0F;
			case AnimationCommandKind::Stop:
			case AnimationCommandKind::Pause:
			case AnimationCommandKind::Resume:
			case AnimationCommandKind::Seek:
			case AnimationCommandKind::SetLooping:
			case AnimationCommandKind::SetReverse:
				return true;
			case AnimationCommandKind::SetParameter:
				switch(command.parameterType) {
				case AnimationParameterType::Boolean: return std::holds_alternative<bool>(command.value);
				case AnimationParameterType::Integer: return std::holds_alternative<std::int32_t>(command.value);
				case AnimationParameterType::Float: return std::holds_alternative<float>(command.value);
				case AnimationParameterType::Vector3:
					return std::holds_alternative<AnimationFloat3>(command.value)
						&& IsFinite(std::get<AnimationFloat3>(command.value));
				case AnimationParameterType::Quaternion:
					return std::holds_alternative<AnimationQuaternion>(command.value)
						&& IsFinite(std::get<AnimationQuaternion>(command.value));
				case AnimationParameterType::Transform:
					return std::holds_alternative<AnimationTransform>(command.value)
						&& IsFinite(std::get<AnimationTransform>(command.value));
				case AnimationParameterType::Trigger: return false;
				default: return false;
				}
			default: return false;
			}
		}
		bool CommandLess(const AnimationCommand& left, const AnimationCommand& right) {
			if(left.targetTick != right.targetTick) return left.targetTick < right.targetTick;
			if(left.producerId != right.producerId) return left.producerId < right.producerId;
			if(left.producerSequence != right.producerSequence) return left.producerSequence < right.producerSequence;
			return left.correlationId < right.correlationId;
		}

		bool SameRequest(const AnimationCommand& left, const AnimationCommand& right) noexcept {
			return left.target == right.target
				&& left.kind == right.kind
				&& left.parameterType == right.parameterType
				&& left.parameterId == right.parameterId
				&& left.value == right.value
				&& left.playbackTick == right.playbackTick
				&& left.playbackSpeed == right.playbackSpeed
				&& left.playbackFlag == right.playbackFlag
				&& left.stateId == right.stateId
				&& left.transitionTicks == right.transitionTicks
				&& left.layerIndex == right.layerIndex
				&& left.layerWeight == right.layerWeight
				&& left.targetTick == right.targetTick
				&& left.producerId == right.producerId
				&& left.producerSequence == right.producerSequence
				&& left.correlationId == right.correlationId
				&& left.idempotencyKey == right.idempotencyKey
				&& left.cancellationToken == right.cancellationToken;
		}

		bool IsTerminal(AnimationStatus status) noexcept {
			return status != AnimationStatus::Queued && status != AnimationStatus::NotReady;
		}

		AnimationCommandReceipt MakeReceipt(const AnimationCommand& command) noexcept
		{
			AnimationCommandReceipt receipt;
			receipt.target = command.target;
			receipt.producerId = command.producerId;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.idempotencyKey = command.idempotencyKey;
			receipt.cancellationToken = command.cancellationToken;
			receipt.status = AnimationStatus::Queued;
			receipt.targetTick = command.targetTick;
			return receipt;
		}
	}

	AnimationCommandReceipt AnimationCommandQueue::Enqueue(
		AnimationCommand command,
		const AnimatorInstanceStore& store) noexcept
	{
		AnimationCommandReceipt receipt = MakeReceipt(command);
		try {
			if(!command.target.IsValid() || command.producerSequence == 0U || command.correlationId == 0U || !HasExpectedValue(command)) {
				receipt.status = AnimationStatus::InvalidArgument;
				return receipt;
			}
			if(!store.IsAlive(command.target)) { receipt.status = AnimationStatus::StaleHandle; return receipt; }
			std::scoped_lock lock(mutex_);
			if(command.idempotencyKey != 0U) {
				const auto repeated = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
					return record.command.producerId == command.producerId
						&& record.command.idempotencyKey == command.idempotencyKey;
				});
				if(repeated != receipts_.end()) {
					AnimationCommandReceipt repeatedReceipt = repeated->receipt;
					repeatedReceipt.duplicate = true;
					if(!SameRequest(repeated->command, command)) repeatedReceipt.status = AnimationStatus::Rejected;
					return repeatedReceipt;
				}
			}
			if(pending_.size() >= capacity_) { receipt.status = AnimationStatus::Rejected; return receipt; }
			if(std::any_of(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& existing) {
				return existing.command.producerId == command.producerId
					&& existing.command.producerSequence == command.producerSequence;
			})) { receipt.status = AnimationStatus::Rejected; return receipt; }
			if(command.cancellationToken != 0U && std::any_of(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& existing) {
				return existing.command.cancellationToken == command.cancellationToken;
			})) { receipt.status = AnimationStatus::Rejected; return receipt; }

			bool reclaimTerminalReceipt = false;
			if(receipts_.size() >= receiptCapacity_) {
				reclaimTerminalReceipt = std::any_of(
					receipts_.begin(),
					receipts_.end(),
					[](const ReceiptRecord& record) { return IsTerminal(record.receipt.status); });
				if(!reclaimTerminalReceipt) { receipt.status = AnimationStatus::Rejected; return receipt; }
			}
			if(failNextEnqueueReservationForTesting_) {
				failNextEnqueueReservationForTesting_ = false;
				receipt.status = AnimationStatus::InternalError;
				return receipt;
			}

			// Allocate and stage every potentially-throwing object before reclaiming
			// a receipt or inserting into either observable vector.
			pending_.reserve(pending_.size() + 1U);
			receipts_.reserve(std::min(receiptCapacity_, receipts_.size() + 1U));
			ReceiptRecord stagedRecord{command, receipt};
			if(reclaimTerminalReceipt) {
				const auto reclaimable = std::find_if(receipts_.begin(), receipts_.end(), [](const ReceiptRecord& record) {
					return IsTerminal(record.receipt.status);
				});
				receipts_.erase(reclaimable);
			}
			receipts_.push_back(std::move(stagedRecord));
			pending_.push_back(std::move(command));
			return receipt;
		}
		catch(...) {
			// Producer-facing entry points return a copied failure receipt. No
			// allocator or mutex exception is allowed to cross the native ABI.
			receipt.status = AnimationStatus::InternalError;
			return receipt;
		}
	}

	void AnimationCommandQueue::FailNextEnqueueReservationForTesting() noexcept
	{
		std::scoped_lock lock(mutex_);
		failNextEnqueueReservationForTesting_ = true;
	}

	AnimationCommandBatchReceipt AnimationCommandQueue::EnqueueBatch(
		std::span<const AnimationCommand> commands,
		const AnimatorInstanceStore& store)
	{
		constexpr std::size_t kMaximumBatchCommands = 256U;
		AnimationCommandBatchReceipt batch;
		if(commands.empty() || commands.size() > kMaximumBatchCommands) {
			batch.status = commands.size() > kMaximumBatchCommands
				? AnimationStatus::HardLimitExceeded
				: AnimationStatus::InvalidArgument;
			return batch;
		}
		try {
			batch.commands.reserve(commands.size());
			for(const AnimationCommand& command : commands) {
				batch.commands.push_back(MakeReceipt(command));
			}
		}
		catch(const std::bad_alloc&) {
			batch.commands.clear();
			batch.status = AnimationStatus::InternalError;
			return batch;
		}

		const auto reject = [&batch](AnimationStatus status) {
			batch.status = status;
			for(AnimationCommandReceipt& receipt : batch.commands) {
				receipt.status = status;
			}
			return batch;
		};
		for(std::size_t index = 0U; index < commands.size(); ++index) {
			const AnimationCommand& command = commands[index];
			if(!command.target.IsValid()
				|| command.producerSequence == 0U
				|| command.correlationId == 0U
				|| !HasExpectedValue(command)) {
				return reject(AnimationStatus::InvalidArgument);
			}
			if(!store.IsAlive(command.target)) {
				return reject(AnimationStatus::StaleHandle);
			}
			for(std::size_t priorIndex = 0U; priorIndex < index; ++priorIndex) {
				const AnimationCommand& prior = commands[priorIndex];
				if((command.producerId == prior.producerId
						&& command.producerSequence == prior.producerSequence)
					|| (command.idempotencyKey != 0U
						&& command.producerId == prior.producerId
						&& command.idempotencyKey == prior.idempotencyKey)
					|| (command.cancellationToken != 0U
						&& command.cancellationToken == prior.cancellationToken)) {
					return reject(AnimationStatus::Rejected);
				}
			}
		}

		std::scoped_lock lock(mutex_);
		std::size_t replayCount = 0U;
		for(std::size_t index = 0U; index < commands.size(); ++index) {
			const AnimationCommand& command = commands[index];
			if(command.idempotencyKey != 0U) {
				const auto repeated = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& record) {
					return record.command.producerId == command.producerId
						&& record.command.idempotencyKey == command.idempotencyKey;
				});
				if(repeated != receipts_.end()) {
					if(!SameRequest(repeated->command, command)) {
						return reject(AnimationStatus::Rejected);
					}
					batch.commands[index] = repeated->receipt;
					batch.commands[index].duplicate = true;
					++replayCount;
					continue;
				}
			}
			if (std::any_of(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& existing) {
				return (existing.command.producerId == command.producerId
						&& existing.command.producerSequence == command.producerSequence)
					|| (command.cancellationToken != 0U
						&& existing.command.cancellationToken == command.cancellationToken);
			})) {
				return reject(AnimationStatus::Rejected);
			}
		}
		if(replayCount != 0U) {
			// A retry must describe the entire prior logical batch. Mixing replayed
			// commands with new work would make the visibility boundary ambiguous.
			if(replayCount != commands.size()) {
				return reject(AnimationStatus::Rejected);
			}
			batch.status = AnimationStatus::Ok;
			return batch;
		}

		// Compare with subtraction so even a pathological capacity value cannot
		// overflow while deciding whether the entire batch fits.
		const std::size_t availablePending = pending_.size() >= capacity_
			? 0U
			: capacity_ - pending_.size();
		if(commands.size() > availablePending) {
			return reject(AnimationStatus::Rejected);
		}
		const std::size_t terminalCount = static_cast<std::size_t>(std::count_if(
			receipts_.begin(),
			receipts_.end(),
			[](const ReceiptRecord& record) { return IsTerminal(record.receipt.status); }));
		const std::size_t retainedReceiptCount = receipts_.size() - terminalCount;
		const std::size_t availableReceipts = retainedReceiptCount >= receiptCapacity_
			? 0U
			: receiptCapacity_ - retainedReceiptCount;
		if(commands.size() > availableReceipts) {
			return reject(AnimationStatus::Rejected);
		}
		try {
			// Both allocations happen before terminal receipt reclamation or logical
			// insertion. Allocation failure therefore leaves observable queue state intact.
			pending_.reserve(pending_.size() + commands.size());
			receipts_.reserve(std::min(receiptCapacity_, receipts_.size() + commands.size()));
		}
		catch(const std::bad_alloc&) {
			return reject(AnimationStatus::InternalError);
		}
		while(receipts_.size() > receiptCapacity_ - commands.size()) {
			const auto reclaimable = std::find_if(receipts_.begin(), receipts_.end(), [](const ReceiptRecord& record) {
				return IsTerminal(record.receipt.status);
			});
			if(reclaimable == receipts_.end()) {
				return reject(AnimationStatus::Rejected);
			}
			receipts_.erase(reclaimable);
		}
		for(std::size_t index = 0U; index < commands.size(); ++index) {
			receipts_.push_back({commands[index], batch.commands[index]});
			pending_.push_back(commands[index]);
		}
		batch.status = AnimationStatus::Queued;
		return batch;
	}

	AnimationCommandReceipt AnimationCommandQueue::Cancel(
		std::uint64_t cancellationToken,
		std::uint64_t correlationId) {
		AnimationCommandReceipt invalid;
		invalid.cancellationToken = cancellationToken;
		invalid.correlationId = correlationId;
		if(cancellationToken == 0U || correlationId == 0U) return invalid;

		std::scoped_lock lock(mutex_);
		const auto record = std::find_if(receipts_.begin(), receipts_.end(), [cancellationToken](const ReceiptRecord& value) {
			return value.command.cancellationToken == cancellationToken;
		});
		if(record == receipts_.end()) {
			invalid.status = AnimationStatus::NotReady;
			return invalid;
		}
		if(IsTerminal(record->receipt.status)) return record->receipt;
		const auto pending = std::find_if(pending_.begin(), pending_.end(), [cancellationToken](const AnimationCommand& value) {
			return value.cancellationToken == cancellationToken;
		});
		if(pending == pending_.end()) {
			// Protected capture already owns this copied request, so cancellation can
			// no longer claim it was removed before application.
			AnimationCommandReceipt tooLate = record->receipt;
			tooLate.status = AnimationStatus::NotReady;
			return tooLate;
		}
		pending_.erase(pending);
		record->receipt.status = AnimationStatus::Cancelled;
		AnimationCommandReceipt cancellationReceipt = record->receipt;
		// The retained command receipt keeps its immutable submission correlation.
		// Only this copied response carries the cancellation-call correlation.
		cancellationReceipt.correlationId = correlationId;
		return cancellationReceipt;
	}

	AnimationCommandReceipt AnimationCommandQueue::ReadReceipt(
		std::uint32_t producerId,
		std::uint64_t producerSequence,
		std::uint64_t correlationId) const {
		AnimationCommandReceipt missing;
		missing.producerId = producerId;
		missing.producerSequence = producerSequence;
		missing.correlationId = correlationId;
		missing.status = producerSequence == 0U || correlationId == 0U
			? AnimationStatus::InvalidArgument
			: AnimationStatus::NotReady;
		if(producerSequence == 0U || correlationId == 0U) return missing;
		std::scoped_lock lock(mutex_);
		const auto record = std::find_if(receipts_.begin(), receipts_.end(), [=](const ReceiptRecord& value) {
			return value.command.producerId == producerId
				&& value.command.producerSequence == producerSequence
				&& value.command.correlationId == correlationId;
		});
		return record == receipts_.end() ? missing : record->receipt;
	}

	void AnimationCommandQueue::PublishCompletion(
		const AnimationCommand& command,
		AnimationStatus status,
		AnimationFixedTick observableTick,
		std::uint64_t publicationGeneration) {
		std::scoped_lock lock(mutex_);
		const auto record = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& value) {
			return value.command.producerId == command.producerId
				&& value.command.producerSequence == command.producerSequence
				&& value.command.correlationId == command.correlationId;
		});
		if(record == receipts_.end() || IsTerminal(record->receipt.status)) return;
		record->receipt.status = status;
		record->receipt.observableTick = observableTick;
		record->receipt.publicationGeneration = publicationGeneration;
	}

	void AnimationCommandQueue::DrainThrough(AnimationFixedTick tick, std::vector<AnimationCommand>& output, const AnimatorInstanceStore& store) {
		output.clear();
		std::scoped_lock lock(mutex_);
		std::vector<AnimationCommand> future;
		future.reserve(pending_.size());
		for(AnimationCommand& command : pending_) {
			// A generation check happens before deciding whether this item is due.
			// This also discards stale future work before a slot can be reused.
			if(!store.IsAlive(command.target)) {
				const auto receipt = std::find_if(receipts_.begin(), receipts_.end(), [&command](const ReceiptRecord& value) {
					return value.command.producerId == command.producerId
						&& value.command.producerSequence == command.producerSequence;
				});
				if(receipt != receipts_.end()) {
					receipt->receipt.status = AnimationStatus::StaleHandle;
					receipt->receipt.observableTick = tick;
					// Drain is a capture boundary, not an authority publication. Generation
					// zero truthfully means no pose/parameter generation observed this drop.
					receipt->receipt.publicationGeneration = 0U;
				}
				continue;
			}
			if(command.targetTick <= tick) output.push_back(std::move(command));
			else future.push_back(std::move(command));
		}
		pending_ = std::move(future);
		std::sort(output.begin(), output.end(), CommandLess);
	}

	std::size_t AnimationCommandQueue::PendingCount() const { std::scoped_lock lock(mutex_); return pending_.size(); }
}
