// AnimationRollback.cpp - Fixed-tick resimulation over immutable copied values.

#include "Animation/Determinism/AnimationRollback.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		[[nodiscard]] constexpr std::uint64_t Mix(
			std::uint64_t checksum,
			std::uint64_t value) noexcept
		{
			return (checksum ^ value) * kFnvPrime;
		}

		[[nodiscard]] bool IsSupported(AnimationDeterministicStateKind kind) noexcept
		{
			return kind >= AnimationDeterministicStateKind::Controller
				&& kind <= AnimationDeterministicStateKind::Physics;
		}

		[[nodiscard]] bool InputOrder(
			const AnimationRollbackExternalInput& left,
			const AnimationRollbackExternalInput& right) noexcept
		{
			if(left.tick != right.tick) {
				return left.tick < right.tick;
			}
			if(left.producerId != right.producerId) {
				return left.producerId < right.producerId;
			}
			return left.producerSequence < right.producerSequence;
		}

		[[nodiscard]] bool SameInputIdentity(
			const AnimationRollbackExternalInput& left,
			const AnimationRollbackExternalInput& right) noexcept
		{
			return left.tick == right.tick && left.producerId == right.producerId
				&& left.producerSequence == right.producerSequence;
		}

		[[nodiscard]] bool MutationOrder(
			const AnimationRollbackDeferredMutation& left,
			const AnimationRollbackDeferredMutation& right) noexcept
		{
			if(left.tick != right.tick) {
				return left.tick < right.tick;
			}
			if(left.domain != right.domain) {
				return left.domain < right.domain;
			}
			if(left.operation != right.operation) {
				return left.operation < right.operation;
			}
			if(left.correlationId != right.correlationId) {
				return left.correlationId < right.correlationId;
			}
			if(left.state.kind != right.state.kind) {
				return left.state.kind < right.state.kind;
			}
			if(left.state.stableId != right.state.stableId) {
				return left.state.stableId < right.state.stableId;
			}
			return left.state.ordinal < right.state.ordinal;
		}

		struct DeliveredStreamWatermark {
			std::uint64_t streamId = 0U;
			std::uint64_t sequence = 0U;
			AnimationFixedTick throughTick = 0;
		};

		DeliveredStreamWatermark& FindOrAppendWatermark(
			std::vector<DeliveredStreamWatermark>& watermarks,
			std::uint64_t streamId)
		{
			const auto found = std::find_if(watermarks.begin(), watermarks.end(),
				[streamId](const DeliveredStreamWatermark& value) {
					return value.streamId == streamId;
				});
			if(found != watermarks.end()) {
				return *found;
			}
			watermarks.push_back({streamId, 0U, 0});
			return watermarks.back();
		}

		void SeedDeliveredWatermarks(
			const AnimationRollbackSnapshot& snapshot,
			std::vector<DeliveredStreamWatermark>& watermarks)
		{
			if(snapshot.eventWatermark.streamId != 0U) {
				watermarks.push_back({
					snapshot.eventWatermark.streamId,
					snapshot.eventWatermark.deliveredSequence,
					snapshot.eventWatermark.deliveredThroughTick});
			}
			for(const AnimationEventCursorRollbackState& event : snapshot.events) {
				if(!event.delivered || event.streamId == 0U) {
					continue;
				}
				DeliveredStreamWatermark& watermark = FindOrAppendWatermark(
					watermarks, event.streamId);
				watermark.sequence = std::max(watermark.sequence, event.deliveredSequence);
				watermark.throughTick = std::max(
					watermark.throughTick, event.deliveredThroughTick);
			}
		}

		[[nodiscard]] AnimationStatus CollectNewEvents(
			const AnimationRollbackSnapshot& snapshot,
			std::vector<DeliveredStreamWatermark>& watermarks,
			AnimationRollbackResult& result) noexcept
		{
			try {
				for(const AnimationEventCursorRollbackState& event : snapshot.events) {
					if(!event.delivered) {
						continue;
					}
					if(event.deliveredSequence == 0U || event.sourceTick > snapshot.tick
						|| event.deliveredThroughTick > snapshot.tick) {
						return AnimationStatus::DeterminismViolation;
					}
					DeliveredStreamWatermark& watermark = FindOrAppendWatermark(
						watermarks, event.streamId);
					if(event.deliveredSequence <= watermark.sequence) {
						++result.suppressedHistoricalEventCount;
						continue;
					}
					result.eventsToPublish.push_back(event);
					watermark.sequence = event.deliveredSequence;
					watermark.throughTick = std::max(
						watermark.throughTick, event.deliveredThroughTick);
				}
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(const std::length_error&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		[[nodiscard]] AnimationRollbackDeferredMutation MakeMutation(
			const AnimationRollbackSnapshot& restorePoint,
			AnimationFixedTick tick,
			AnimationRollbackMutationDomain domain,
			AnimationRollbackMutationOperation operation) noexcept
		{
			AnimationRollbackDeferredMutation mutation;
			mutation.domain = domain;
			mutation.operation = operation;
			mutation.animator = restorePoint.animator;
			mutation.tick = tick;
			mutation.expectedSceneGeneration = restorePoint.generations.sceneGeneration;
			mutation.expectedPhysicsWorldGeneration =
				restorePoint.generations.physicsWorldGeneration;
			return mutation;
		}

		[[nodiscard]] AnimationStatus AppendRootCorrection(
			const AnimationRollbackSnapshot& restorePoint,
			AnimationFixedTick tick,
			const AnimationRootMotionCorrection& correction,
			AnimationRollbackResult& result) noexcept
		{
			if(correction.sourceTick == 0 && correction.correlationId == 0U
				&& !correction.requiresSceneTransaction
				&& !correction.requiresPhysicsCommand) {
				return AnimationStatus::Ok;
			}
			if(correction.sourceTick != tick || correction.correlationId == 0U) {
				return AnimationStatus::DeterminismViolation;
			}
			const auto prior = std::find_if(result.rootMotionCorrections.begin(),
				result.rootMotionCorrections.end(),
				[&correction](const AnimationRootMotionCorrection& value) {
					return value.correlationId == correction.correlationId;
				});
			if(prior != result.rootMotionCorrections.end()) {
				return *prior == correction
					? AnimationStatus::Ok
					: AnimationStatus::DeterminismViolation;
			}
			try {
				result.rootMotionCorrections.push_back(correction);
				if(correction.requiresSceneTransaction) {
					AnimationRollbackDeferredMutation mutation = MakeMutation(
						restorePoint,
						tick,
						AnimationRollbackMutationDomain::Scene,
						AnimationRollbackMutationOperation::ApplyRootMotion);
					mutation.correlationId = correction.correlationId;
					mutation.rootMotion = correction;
					result.deferredMutations.commands.push_back(mutation);
					result.deferredMutations.requiresProtectedSceneFlush = true;
				}
				if(correction.requiresPhysicsCommand) {
					AnimationRollbackDeferredMutation mutation = MakeMutation(
						restorePoint,
						tick,
						AnimationRollbackMutationDomain::Physics,
						AnimationRollbackMutationOperation::ApplyRootMotion);
					mutation.correlationId = correction.correlationId;
					mutation.rootMotion = correction;
					result.deferredMutations.commands.push_back(mutation);
					result.deferredMutations.requiresPhysicsBoundary = true;
				}
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(const std::length_error&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		[[nodiscard]] AnimationStatus AppendStateMutation(
			const AnimationRollbackSnapshot& restorePoint,
			const AnimationRollbackExternalInput& input,
			AnimationRollbackResult& result) noexcept
		{
			AnimationRollbackMutationDomain domain;
			switch(input.value.kind) {
			case AnimationDeterministicStateKind::Attachment:
				domain = AnimationRollbackMutationDomain::Attachment;
				result.deferredMutations.requiresProtectedSceneFlush = true;
				break;
			case AnimationDeterministicStateKind::Physics:
				domain = AnimationRollbackMutationDomain::Physics;
				result.deferredMutations.requiresPhysicsBoundary = true;
				break;
			case AnimationDeterministicStateKind::Ragdoll:
				domain = AnimationRollbackMutationDomain::Ragdoll;
				result.deferredMutations.requiresPhysicsBoundary = true;
				break;
			default:
				return AnimationStatus::Ok;
			}
			if(result.deferredMutations.commands.size()
				>= kAnimationRollbackMaximumDeferredMutations) {
				return AnimationStatus::HardLimitExceeded;
			}
			try {
				AnimationRollbackDeferredMutation mutation = MakeMutation(
					restorePoint,
					input.tick,
					domain,
					AnimationRollbackMutationOperation::SynchronizeState);
				mutation.correlationId = input.producerSequence;
				mutation.state = input.value;
				result.deferredMutations.commands.push_back(mutation);
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(const std::length_error&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}
	}

	std::uint64_t ComputeAnimationRollbackMutationChecksum(
		const AnimationRollbackMutationBatch& batch) noexcept
	{
		std::uint64_t checksum = kFnvOffset;
		checksum = Mix(checksum, batch.version);
		checksum = Mix(checksum, batch.animator.slotIndex);
		checksum = Mix(checksum, batch.animator.generation);
		checksum = Mix(checksum, static_cast<std::uint64_t>(batch.restoredTick));
		checksum = Mix(checksum, static_cast<std::uint64_t>(batch.throughTick));
		checksum = Mix(checksum, batch.generations.objectGeneration);
		checksum = Mix(checksum, batch.generations.componentGeneration);
		checksum = Mix(checksum, batch.generations.sceneGeneration);
		checksum = Mix(checksum, batch.generations.assetGeneration);
		checksum = Mix(checksum, batch.generations.programGeneration);
		checksum = Mix(checksum, batch.generations.physicsWorldGeneration);
		checksum = Mix(checksum, batch.generations.historyGeneration);
		checksum = Mix(checksum, batch.commands.size());
		for(const AnimationRollbackDeferredMutation& command : batch.commands) {
			checksum = Mix(checksum, static_cast<std::uint8_t>(command.domain));
			checksum = Mix(checksum, static_cast<std::uint8_t>(command.operation));
			checksum = Mix(checksum, command.animator.slotIndex);
			checksum = Mix(checksum, command.animator.generation);
			checksum = Mix(checksum, static_cast<std::uint64_t>(command.tick));
			checksum = Mix(checksum, command.expectedSceneGeneration);
			checksum = Mix(checksum, command.expectedPhysicsWorldGeneration);
			checksum = Mix(checksum, command.correlationId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(command.rootMotion.sourceTick));
			for(const std::uint32_t word : command.rootMotion.translationAndYawBits) {
				checksum = Mix(checksum, word);
			}
			checksum = Mix(checksum, command.rootMotion.correlationId);
			checksum = Mix(checksum, command.rootMotion.requiresSceneTransaction ? 1U : 0U);
			checksum = Mix(checksum, command.rootMotion.requiresPhysicsCommand ? 1U : 0U);
			checksum = Mix(checksum, static_cast<std::uint16_t>(command.state.kind));
			checksum = Mix(checksum, command.state.flags);
			checksum = Mix(checksum, command.state.ordinal);
			checksum = Mix(checksum, command.state.stableId);
			for(const std::uint64_t value : command.state.values) {
				checksum = Mix(checksum, value);
			}
		}
		checksum = Mix(checksum, batch.requiresProtectedSceneFlush ? 1U : 0U);
		return Mix(checksum, batch.requiresPhysicsBoundary ? 1U : 0U);
	}

	AnimationStatus AnimationRollback::Resimulate(
		const AnimationRollbackSnapshot& restorePoint,
		AnimationFixedTick targetTick,
		std::span<const AnimationRollbackExternalInput> inputs,
		const AnimationRollbackStep& step,
		AnimationRollbackResult& output) const noexcept
	{
		if(targetTick < restorePoint.tick || !step) {
			return AnimationStatus::InvalidArgument;
		}
		if(inputs.size() > kAnimationRollbackMaximumExternalInputs) {
			return AnimationStatus::HardLimitExceeded;
		}

		AnimationRollbackSnapshot restored;
		const AnimationStatus restoreStatus = AnimationRollbackSnapshotCodec{}.Restore(
			restorePoint,
			restorePoint.fingerprint,
			restorePoint.publicationGeneration,
			restored);
		if(restoreStatus != AnimationStatus::Ok) {
			return restoreStatus;
		}

		try {
			std::vector<AnimationRollbackExternalInput> ordered(inputs.begin(), inputs.end());
			for(const AnimationRollbackExternalInput& input : ordered) {
				if(input.tick <= restored.tick || input.tick > targetTick
					|| input.producerId == 0U || input.producerSequence == 0U
					|| input.value.stableId == 0U || !IsSupported(input.value.kind)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::sort(ordered.begin(), ordered.end(), InputOrder);
			for(std::size_t index = 1U; index < ordered.size(); ++index) {
				if(SameInputIdentity(ordered[index - 1U], ordered[index])
					&& ordered[index - 1U].value != ordered[index].value) {
					// Arrival order cannot decide between two values carrying one
					// authoritative producer identity.
					return AnimationStatus::DeterminismViolation;
				}
			}
			ordered.erase(std::unique(ordered.begin(), ordered.end(), SameInputIdentity),
				ordered.end());

			AnimationRollbackResult candidate;
			candidate.status = AnimationStatus::Ok;
			candidate.snapshot = restored;
			candidate.eventWatermark = restored.eventWatermark;
			candidate.rootMotionCorrection = restored.rootMotion;
			candidate.deferredMutations.animator = restored.animator;
			candidate.deferredMutations.restoredTick = restored.tick;
			candidate.deferredMutations.throughTick = targetTick;
			candidate.deferredMutations.generations = restored.generations;
			std::vector<DeliveredStreamWatermark> watermarks;
			watermarks.reserve(restored.events.size() + 1U);
			SeedDeliveredWatermarks(restored, watermarks);

			std::size_t firstInput = 0U;
			AnimationRollbackSnapshot current = restored;
			if(targetTick != restored.tick) {
				AnimationFixedTick tick = restored.tick + 1;
				for(;;) {
					const std::size_t begin = firstInput;
					while(firstInput < ordered.size() && ordered[firstInput].tick == tick) {
						++firstInput;
					}
					std::vector<AnimationDeterministicStateRecord> tickInputs;
					tickInputs.reserve(firstInput - begin);
					for(std::size_t index = begin; index < firstInput; ++index) {
						tickInputs.push_back(ordered[index].value);
						candidate.appliedInputs.push_back(ordered[index].value);
						const AnimationStatus mutationStatus = AppendStateMutation(
							restored, ordered[index], candidate);
						if(mutationStatus != AnimationStatus::Ok) {
							return mutationStatus;
						}
					}

					AnimationRollbackSnapshot produced;
					const AnimationStatus stepStatus = step(tick, tickInputs, produced);
					if(stepStatus != AnimationStatus::Ok) {
						return stepStatus;
					}
					if(produced.animator != restored.animator || produced.tick != tick
						|| produced.fingerprint != restored.fingerprint
						|| produced.publicationGeneration != restored.publicationGeneration
						|| produced.generations != restored.generations) {
						return AnimationStatus::DeterminismViolation;
					}

					AnimationRollbackSnapshot canonicalProduced;
					const AnimationStatus captureStatus = AnimationRollbackSnapshotCodec{}.Capture(
						produced, canonicalProduced);
					if(captureStatus != AnimationStatus::Ok) {
						return captureStatus == AnimationStatus::HardLimitExceeded
							? captureStatus
							: AnimationStatus::DeterminismViolation;
					}
					if(canonicalProduced.eventWatermark.streamId
						== current.eventWatermark.streamId
						&& (canonicalProduced.eventWatermark.deliveredSequence
							< current.eventWatermark.deliveredSequence
							|| canonicalProduced.eventWatermark.deliveredThroughTick
								< current.eventWatermark.deliveredThroughTick)) {
						return AnimationStatus::DeterminismViolation;
					}
					const AnimationStatus eventStatus = CollectNewEvents(
						canonicalProduced, watermarks, candidate);
					if(eventStatus != AnimationStatus::Ok) {
						return eventStatus;
					}
					const AnimationStatus correctionStatus = AppendRootCorrection(
						restored, tick, canonicalProduced.rootMotion, candidate);
					if(correctionStatus != AnimationStatus::Ok) {
						return correctionStatus;
					}
					current = std::move(canonicalProduced);
					if(tick == targetTick) {
						break;
					}
					if(tick == std::numeric_limits<AnimationFixedTick>::max()) {
						return AnimationStatus::RangeOverflow;
					}
					++tick;
				}
			}

			if(candidate.deferredMutations.commands.size()
				> kAnimationRollbackMaximumDeferredMutations) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::sort(candidate.deferredMutations.commands.begin(),
				candidate.deferredMutations.commands.end(), MutationOrder);
			candidate.deferredMutations.checksum =
				ComputeAnimationRollbackMutationChecksum(candidate.deferredMutations);
			candidate.snapshot = std::move(current);
			candidate.eventWatermark = candidate.snapshot.eventWatermark;
			candidate.rootMotionCorrection = candidate.snapshot.rootMotion;
			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(...) {
			// User-supplied step callbacks are contained so worker failures cannot
			// unwind through the scheduler or publish a partial mutation batch.
			return AnimationStatus::InternalError;
		}
	}
}
