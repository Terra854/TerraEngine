// AnimatorPlayback.cpp - Fixed-point playback control and catch-up semantics.

#include "Animation/Runtime/AnimatorPlayback.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint64_t kPlaybackFraction = 1ULL << 16U;
		constexpr float kMaximumPlaybackSpeed = 100.0F;

		bool IsValidTimePolicy(AnimatorTimePolicy policy) noexcept
		{
			return policy == AnimatorTimePolicy::Scaled
				|| policy == AnimatorTimePolicy::Unscaled
				|| policy == AnimatorTimePolicy::Fixed;
		}

		bool CheckedAdd(AnimationFixedTick left, AnimationFixedTick right, AnimationFixedTick& output) noexcept
		{
			if((right > 0 && left > std::numeric_limits<AnimationFixedTick>::max() - right)
				|| (right < 0 && left < std::numeric_limits<AnimationFixedTick>::min() - right)) {
				return false;
			}
			output = left + right;
			return true;
		}

		std::int64_t SignedLoopDifference(
			const AnimationResolvedClipTime& before,
			const AnimationResolvedClipTime& after) noexcept
		{
			if(after.loopIndex > before.loopIndex && before.loopIndex < 0
				&& after.loopIndex > std::numeric_limits<std::int64_t>::max() + before.loopIndex) {
				return std::numeric_limits<std::int64_t>::max();
			}
			if(after.loopIndex < before.loopIndex && before.loopIndex > 0
				&& after.loopIndex < std::numeric_limits<std::int64_t>::min() + before.loopIndex) {
				return std::numeric_limits<std::int64_t>::min();
			}
			return after.loopIndex - before.loopIndex;
		}
	}

	AnimationStatus AnimatorPlayback::Configure(AnimatorPlaybackConfiguration configuration) noexcept
	{
		if(configuration.durationTicks < 0
			|| !std::isfinite(configuration.speed)
			|| configuration.speed < 0.0F
			|| configuration.speed > kMaximumPlaybackSpeed
			|| !IsValidTimePolicy(configuration.timePolicy)
			|| configuration.fixedStepTicks <= 0) {
			return AnimationStatus::InvalidArgument;
		}

		const std::uint64_t quantizedSpeed = static_cast<std::uint64_t>(
			std::llround(static_cast<double>(configuration.speed) * static_cast<double>(kPlaybackFraction)));
		if(quantizedSpeed > std::numeric_limits<std::uint32_t>::max()) {
			return AnimationStatus::InvalidArgument;
		}

		configuration_ = configuration;
		speedQ16_ = static_cast<std::uint32_t>(quantizedSpeed);
		absoluteTick_ = std::clamp<AnimationFixedTick>(absoluteTick_, 0, configuration_.durationTicks);
		runState_ = AnimatorPlaybackRunState::Stopped;
		latestStatus_ = AnimationStatus::Ok;
		ClearFractionalTime();
		configured_ = true;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Play() noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(runState_ == AnimatorPlaybackRunState::Completed) {
			// Replaying from a completed boundary starts at the direction's source
			// edge. Pause/resume never rewinds because Paused is a distinct state.
			absoluteTick_ = configuration_.reverse ? configuration_.durationTicks : 0;
			ClearFractionalTime();
		}
		runState_ = AnimatorPlaybackRunState::Playing;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Stop() noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		absoluteTick_ = 0;
		runState_ = AnimatorPlaybackRunState::Stopped;
		latestStatus_ = AnimationStatus::Ok;
		ClearFractionalTime();
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Pause() noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(runState_ != AnimatorPlaybackRunState::Playing) return AnimationStatus::Rejected;
		runState_ = AnimatorPlaybackRunState::Paused;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Resume() noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(runState_ != AnimatorPlaybackRunState::Paused) return AnimationStatus::Rejected;
		runState_ = AnimatorPlaybackRunState::Playing;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Seek(AnimationFixedTick absoluteTick) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		AnimationResolvedClipTime resolved;
		const AnimationStatus status = ResolveAnimationClipTime(
			configuration_.durationTicks, absoluteTick, configuration_.loop, resolved);
		if(status != AnimationStatus::Ok) return status;
		absoluteTick_ = configuration_.loop ? absoluteTick : resolved.resolvedTick;
		latestStatus_ = AnimationStatus::Ok;
		ClearFractionalTime();
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::SetLooping(bool looping) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(configuration_.loop == looping) return AnimationStatus::Ok;
		configuration_.loop = looping;
		if(!looping) {
			AnimationResolvedClipTime resolved;
			if(const AnimationStatus status = ResolveCurrent(resolved); status != AnimationStatus::Ok) return status;
			absoluteTick_ = resolved.resolvedTick;
		}
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::SetReverse(bool reverse) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		configuration_.reverse = reverse;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::SetSpeed(float speed) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(!std::isfinite(speed) || speed < 0.0F || speed > kMaximumPlaybackSpeed) {
			return AnimationStatus::InvalidArgument;
		}
		const std::uint64_t quantized = static_cast<std::uint64_t>(
			std::llround(static_cast<double>(speed) * static_cast<double>(kPlaybackFraction)));
		if(quantized > std::numeric_limits<std::uint32_t>::max()) return AnimationStatus::InvalidArgument;
		configuration_.speed = speed;
		speedQ16_ = static_cast<std::uint32_t>(quantized);
		speedProductRemainder_ = 0U;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::SetTimePolicy(
		AnimatorTimePolicy policy,
		AnimationFixedTick fixedStepTicks) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		if(!IsValidTimePolicy(policy) || fixedStepTicks <= 0) return AnimationStatus::InvalidArgument;
		configuration_.timePolicy = policy;
		configuration_.fixedStepTicks = fixedStepTicks;
		ClearFractionalTime();
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::Advance(
		const AnimatorPlaybackTimeInput& input,
		AnimatorPlaybackAdvanceResult& output) noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		AnimatorPlaybackAdvanceResult candidate;
		candidate.previousAbsoluteTick = absoluteTick_;
		candidate.currentAbsoluteTick = absoluteTick_;
		if(const AnimationStatus status = ResolveCurrent(candidate.resolved); status != AnimationStatus::Ok) {
			candidate.status = status;
			latestStatus_ = status;
			return status;
		}

		if(runState_ != AnimatorPlaybackRunState::Playing || speedQ16_ == 0U) {
			candidate.status = AnimationStatus::Ok;
			output = candidate;
			latestStatus_ = AnimationStatus::Ok;
			return AnimationStatus::Ok;
		}

		AnimationFixedTick unsignedTicks = 0;
		if(const AnimationStatus status = ConvertInputToTicks(input, unsignedTicks); status != AnimationStatus::Ok) {
			latestStatus_ = status;
			return status;
		}
		const AnimationFixedTick signedTicks = configuration_.reverse ? -unsignedTicks : unsignedTicks;
		AnimationFixedTick nextAbsolute = 0;
		if(!CheckedAdd(absoluteTick_, signedTicks, nextAbsolute)) {
			latestStatus_ = AnimationStatus::RangeOverflow;
			return AnimationStatus::RangeOverflow;
		}

		AnimationResolvedClipTime before;
		AnimationResolvedClipTime after;
		ResolveCurrent(before);
		if(const AnimationStatus status = ResolveAnimationClipTime(
			configuration_.durationTicks, nextAbsolute, configuration_.loop, after);
			status != AnimationStatus::Ok) {
			latestStatus_ = status;
			return status;
		}

		if(configuration_.loop) {
			absoluteTick_ = nextAbsolute;
		} else {
			absoluteTick_ = after.resolvedTick;
			const bool hitForwardEnd = !configuration_.reverse && nextAbsolute >= configuration_.durationTicks;
			const bool hitReverseEnd = configuration_.reverse && nextAbsolute <= 0;
			if((hitForwardEnd || hitReverseEnd) && signedTicks != 0) {
				runState_ = AnimatorPlaybackRunState::Completed;
				candidate.endpointReached = true;
			}
		}

		candidate.status = AnimationStatus::Ok;
		candidate.currentAbsoluteTick = absoluteTick_;
		candidate.advancedTicks = absoluteTick_ - candidate.previousAbsoluteTick;
		candidate.resolved = after;
		candidate.loopsCrossed = configuration_.loop ? SignedLoopDifference(before, after) : 0;
		output = candidate;
		latestStatus_ = AnimationStatus::Ok;
		return AnimationStatus::Ok;
	}

	AnimatorPlaybackStateSnapshot AnimatorPlayback::Snapshot() const noexcept
	{
		AnimatorPlaybackStateSnapshot snapshot;
		snapshot.runState = runState_;
		snapshot.timePolicy = configuration_.timePolicy;
		snapshot.absoluteTick = absoluteTick_;
		snapshot.durationTicks = configuration_.durationTicks;
		snapshot.looping = configuration_.loop;
		snapshot.reverse = configuration_.reverse;
		snapshot.effectiveSpeed = runState_ == AnimatorPlaybackRunState::Playing
			? (configuration_.reverse ? -configuration_.speed : configuration_.speed)
			: 0.0F;
		snapshot.latestStatus = latestStatus_;
		AnimationResolvedClipTime resolved;
		if(ResolveCurrent(resolved) == AnimationStatus::Ok) {
			snapshot.resolvedTick = resolved.resolvedTick;
			snapshot.loopCount = resolved.loopIndex;
			snapshot.normalizedTime = configuration_.durationTicks > 0
				? static_cast<float>(static_cast<double>(resolved.resolvedTick)
					/ static_cast<double>(configuration_.durationTicks))
				: 0.0F;
		}
		return snapshot;
	}

	AnimatorPlaybackDeterministicState AnimatorPlayback::CaptureDeterministicState() const noexcept
	{
		return {
			configuration_,
			runState_,
			absoluteTick_,
			speedQ16_,
			speedProductRemainder_,
			subTickRemainderQ16_,
			latestStatus_,
			configured_};
	}

	AnimationStatus AnimatorPlayback::RestoreDeterministicState(
		const AnimatorPlaybackDeterministicState& state) noexcept
	{
		if(!state.configured) return AnimationStatus::InvalidArgument;
		AnimatorPlayback candidate;
		const AnimationStatus configureStatus = candidate.Configure(state.configuration);
		if(configureStatus != AnimationStatus::Ok
			|| state.runState < AnimatorPlaybackRunState::Stopped
			|| state.runState > AnimatorPlaybackRunState::Completed
			|| state.subTickRemainderQ16 >= kPlaybackFraction) {
			return AnimationStatus::InvalidArgument;
		}

		const std::uint64_t expectedSpeed = static_cast<std::uint64_t>(std::llround(
			static_cast<double>(state.configuration.speed)
				* static_cast<double>(kPlaybackFraction)));
		if(expectedSpeed != state.speedQ16) return AnimationStatus::InvalidArgument;
		AnimationResolvedClipTime resolved;
		if(ResolveAnimationClipTime(
				state.configuration.durationTicks,
				state.absoluteTick,
				state.configuration.loop,
				resolved) != AnimationStatus::Ok) {
			return AnimationStatus::InvalidArgument;
		}
		if(!state.configuration.loop
			&& state.absoluteTick != resolved.resolvedTick) {
			return AnimationStatus::InvalidArgument;
		}

		candidate.absoluteTick_ = state.absoluteTick;
		candidate.runState_ = state.runState;
		candidate.speedQ16_ = state.speedQ16;
		candidate.speedProductRemainder_ = state.speedProductRemainder;
		candidate.subTickRemainderQ16_ = state.subTickRemainderQ16;
		candidate.latestStatus_ = state.latestStatus;
		*this = candidate;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimatorPlayback::ResolveCurrent(AnimationResolvedClipTime& output) const noexcept
	{
		if(!configured_) return AnimationStatus::NotReady;
		return ResolveAnimationClipTime(
			configuration_.durationTicks, absoluteTick_, configuration_.loop, output);
	}

	AnimationStatus AnimatorPlayback::ConvertInputToTicks(
		const AnimatorPlaybackTimeInput& input,
		AnimationFixedTick& outputTicks) noexcept
	{
		std::uint64_t sourceQ16 = 0U;
		if(configuration_.timePolicy == AnimatorTimePolicy::Fixed) {
			const AnimationFixedTick fixedTicks = input.fixedDeltaTicks > 0
				? input.fixedDeltaTicks
				: configuration_.fixedStepTicks;
			if(fixedTicks <= 0
				|| static_cast<std::uint64_t>(fixedTicks)
					> std::numeric_limits<std::uint64_t>::max() / kPlaybackFraction) {
				return AnimationStatus::RangeOverflow;
			}
			sourceQ16 = static_cast<std::uint64_t>(fixedTicks) * kPlaybackFraction;
		} else {
			const double seconds = configuration_.timePolicy == AnimatorTimePolicy::Unscaled
				? input.unscaledDeltaSeconds
				: input.scaledDeltaSeconds;
			if(!std::isfinite(seconds) || seconds < 0.0) return AnimationStatus::InvalidArgument;
			const long double source = static_cast<long double>(seconds)
				* static_cast<long double>(kAnimationFixedTicksPerSecond)
				* static_cast<long double>(kPlaybackFraction);
			if(source > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
				return AnimationStatus::RangeOverflow;
			}
			sourceQ16 = static_cast<std::uint64_t>(std::llround(source));
		}

		if(speedQ16_ != 0U
			&& sourceQ16 > (std::numeric_limits<std::uint64_t>::max() - speedProductRemainder_) / speedQ16_) {
			return AnimationStatus::RangeOverflow;
		}
		const std::uint64_t speedProduct = sourceQ16 * speedQ16_ + speedProductRemainder_;
		const std::uint64_t scaledQ16 = speedProduct / kPlaybackFraction;
		const std::uint64_t nextSpeedRemainder = speedProduct % kPlaybackFraction;
		if(scaledQ16 > std::numeric_limits<std::uint64_t>::max() - subTickRemainderQ16_) {
			return AnimationStatus::RangeOverflow;
		}
		const std::uint64_t accumulatedQ16 = scaledQ16 + subTickRemainderQ16_;
		const std::uint64_t wholeTicks = accumulatedQ16 / kPlaybackFraction;
		if(wholeTicks > static_cast<std::uint64_t>(std::numeric_limits<AnimationFixedTick>::max())) {
			return AnimationStatus::RangeOverflow;
		}

		// Commit fractional state only after every checked operation succeeded.
		speedProductRemainder_ = nextSpeedRemainder;
		subTickRemainderQ16_ = static_cast<std::uint32_t>(accumulatedQ16 % kPlaybackFraction);
		outputTicks = static_cast<AnimationFixedTick>(wholeTicks);
		return AnimationStatus::Ok;
	}

	void AnimatorPlayback::ClearFractionalTime() noexcept
	{
		speedProductRemainder_ = 0U;
		subTickRemainderQ16_ = 0U;
	}
}
