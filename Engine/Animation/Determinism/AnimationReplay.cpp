// AnimationReplay.cpp - Generation-safe late-join and replay-seek policy.

#include "Animation/Determinism/AnimationReplay.h"

#include <limits>
#include <new>
#include <stdexcept>

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool IsSupported(AnimationReplayMode value) noexcept
		{
			return value >= AnimationReplayMode::LateJoin
				&& value <= AnimationReplayMode::ReplaySeek;
		}

		[[nodiscard]] bool IsSupported(AnimationReplayEventPolicy value) noexcept
		{
			return value >= AnimationReplayEventPolicy::SuppressHistorical
				&& value <= AnimationReplayEventPolicy::ReplayHistorical;
		}

		void CompleteResult(AnimationReplayResult& result) noexcept
		{
			result.selectedSnapshotTick = result.resumeSnapshot.tick;
			result.resumeEvaluationTick = result.resumeSnapshot.tick
				== std::numeric_limits<AnimationFixedTick>::max()
				? result.resumeSnapshot.tick
				: result.resumeSnapshot.tick + 1;
			result.nextEventSequence = result.resumeSnapshot.eventWatermark.deliveredSequence
				== std::numeric_limits<std::uint64_t>::max()
				? result.resumeSnapshot.eventWatermark.deliveredSequence
				: result.resumeSnapshot.eventWatermark.deliveredSequence + 1U;
		}
	}

	AnimationReplayResult ResolveAnimationReplay(
		const AnimationHistory& history,
		const AnimationReplayRequest& request) noexcept
	{
		AnimationReplayResult result;
		if(!IsSupported(request.mode) || !IsSupported(request.eventPolicy)
			|| !request.animator.IsValid() || request.seekTick < 0
			|| request.requirements.animator != request.animator
			|| !request.requirements.fingerprint.IsValid()
			|| request.requirements.publicationGeneration == 0U
			|| !request.requirements.generations.IsValid()
			|| request.maximumCatchUpTicks > kAnimationDeterminismMaximumHistorySnapshots) {
			return result;
		}
		try {
			AnimationRollbackSnapshot retained;
			result.status = history.LatestAtOrBefore(
				request.animator,
				request.seekTick,
				request.requirements.fingerprint,
				retained);
			if(result.status != AnimationStatus::Ok) {
				return result;
			}
			AnimationRollbackSnapshot restored;
			result.status = AnimationRollbackSnapshotCodec{}.Restore(
				retained, request.requirements, restored);
			if(result.status != AnimationStatus::Ok) {
				return result;
			}
			if(request.requireExactTick && restored.tick != request.seekTick) {
				result.status = AnimationStatus::NotReady;
				return result;
			}
			const std::uint64_t catchUp = static_cast<std::uint64_t>(
				request.seekTick - restored.tick);
			if(catchUp > request.maximumCatchUpTicks) {
				result.status = AnimationStatus::RangeOutOfBounds;
				return result;
			}

			result.resumeSnapshot = std::move(restored);
			result.requiredCatchUpTicks = static_cast<std::size_t>(catchUp);
			result.suppressHistoricalOneShotEvents = request.mode == AnimationReplayMode::LateJoin
				|| request.eventPolicy == AnimationReplayEventPolicy::SuppressHistorical;
			if(!result.suppressHistoricalOneShotEvents) {
				result.historicalEvents.reserve(result.resumeSnapshot.events.size());
				for(const AnimationEventCursorRollbackState& event : result.resumeSnapshot.events) {
					if(event.delivered && event.sourceTick <= result.resumeSnapshot.tick) {
						result.historicalEvents.push_back(event);
					}
				}
			}
			CompleteResult(result);
			result.status = AnimationStatus::Ok;
			return result;
		} catch(const std::bad_alloc&) {
			result = {};
			result.status = AnimationStatus::HardLimitExceeded;
			return result;
		} catch(const std::length_error&) {
			result = {};
			result.status = AnimationStatus::HardLimitExceeded;
			return result;
		}
	}

	AnimationReplayResult ResumeAnimationReplay(
		const AnimationHistory& history,
		AnimatorInstanceHandle animator,
		AnimationFixedTick seekTick,
		const AnimationDeterministicFingerprint& expectedFingerprint) noexcept
	{
		AnimationReplayResult result;
		result.status = history.LatestAtOrBefore(
			animator, seekTick, expectedFingerprint, result.resumeSnapshot);
		if(result.status != AnimationStatus::Ok) {
			return result;
		}
		result.requiredCatchUpTicks = static_cast<std::size_t>(
			seekTick - result.resumeSnapshot.tick);
		result.suppressHistoricalOneShotEvents = true;
		CompleteResult(result);
		return result;
	}
}
