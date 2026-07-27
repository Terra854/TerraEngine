// AnimatorPlayback.h - Deterministic fixed-tick clip playback state.
//
// This type owns only one animator's transient time cursor. Authored component
// data remains serializable by value, while AnimationSystem copies this state
// into worker packets and commits it only at the protected publication boundary.

#pragma once

#include "Animation/Runtime/AnimationClipView.h"

#include <algorithm>
#include <cstdint>

namespace TerraEngine::Animation {
	// A looping timeline owns the distinct local ticks [0, duration - 1].
	// The exact duration is the next loop's tick zero, so accepting it as an
	// authored start pose would make an Inspector value at "max" appear to wrap.
	// One-shot playback keeps the exact duration because its final pose is a
	// stable endpoint rather than an alias.
	[[nodiscard]] constexpr AnimationFixedTick ClampAnimatorInitialPlaybackTick(
		AnimationFixedTick requestedTick,
		AnimationFixedTick durationTicks,
		bool loop) noexcept {
		const AnimationFixedTick safeDuration = std::max<AnimationFixedTick>(
			durationTicks,
			0);
		const AnimationFixedTick maximumDistinctTick =
			loop && safeDuration > 0
				? safeDuration - 1
				: safeDuration;
		return std::clamp<AnimationFixedTick>(
			requestedTick,
			0,
			maximumDistinctTick);
	}

	enum class AnimatorPlaybackRunState : std::uint8_t {
		Stopped = 0U,
		Playing = 1U,
		Paused = 2U,
		Completed = 3U
	};

	struct AnimatorPlaybackConfiguration {
		AnimationFixedTick durationTicks = 0;
		bool loop = false;
		bool reverse = false;
		float speed = 1.0F;
		AnimatorTimePolicy timePolicy = AnimatorTimePolicy::Scaled;
		AnimationFixedTick fixedStepTicks = 1;

		auto operator<=>(const AnimatorPlaybackConfiguration&) const = default;
	};

	// The protected frame capture supplies both clocks. Fixed-time animators use
	// integer ticks directly, so their authority never depends on float rounding.
	struct AnimatorPlaybackTimeInput {
		double scaledDeltaSeconds = 0.0;
		double unscaledDeltaSeconds = 0.0;
		AnimationFixedTick fixedDeltaTicks = 0;
	};

	struct AnimatorPlaybackAdvanceResult {
		AnimationStatus status = AnimationStatus::Ok;
		AnimationFixedTick previousAbsoluteTick = 0;
		AnimationFixedTick currentAbsoluteTick = 0;
		AnimationFixedTick advancedTicks = 0;
		AnimationResolvedClipTime resolved{};
		std::int64_t loopsCrossed = 0;
		bool endpointReached = false;
	};

	struct AnimatorPlaybackStateSnapshot {
		AnimatorPlaybackRunState runState = AnimatorPlaybackRunState::Stopped;
		AnimatorTimePolicy timePolicy = AnimatorTimePolicy::Scaled;
		AnimationFixedTick absoluteTick = 0;
		AnimationFixedTick resolvedTick = 0;
		AnimationFixedTick durationTicks = 0;
		std::int64_t loopCount = 0;
		float normalizedTime = 0.0F;
		float effectiveSpeed = 0.0F;
		bool looping = false;
		bool reverse = false;
		AnimationStatus latestStatus = AnimationStatus::NotReady;

		auto operator<=>(const AnimatorPlaybackStateSnapshot&) const = default;
	};

	// Rollback needs the fixed-point remainders that the ordinary gameplay
	// snapshot intentionally hides. This copied value contains no runtime pointer
	// and can be staged and validated before replacing the live playback cursor.
	struct AnimatorPlaybackDeterministicState {
		AnimatorPlaybackConfiguration configuration{};
		AnimatorPlaybackRunState runState = AnimatorPlaybackRunState::Stopped;
		AnimationFixedTick absoluteTick = 0;
		std::uint32_t speedQ16 = 1U << 16U;
		std::uint64_t speedProductRemainder = 0U;
		std::uint32_t subTickRemainderQ16 = 0U;
		AnimationStatus latestStatus = AnimationStatus::NotReady;
		bool configured = false;

		auto operator<=>(const AnimatorPlaybackDeterministicState&) const = default;
	};

	class TERRAENGINE_API AnimatorPlayback {
	public:
		AnimationStatus Configure(AnimatorPlaybackConfiguration configuration) noexcept;
		AnimationStatus Play() noexcept;
		AnimationStatus Stop() noexcept;
		AnimationStatus Pause() noexcept;
		AnimationStatus Resume() noexcept;
		AnimationStatus Seek(AnimationFixedTick absoluteTick) noexcept;
		AnimationStatus SetLooping(bool looping) noexcept;
		AnimationStatus SetReverse(bool reverse) noexcept;
		AnimationStatus SetSpeed(float speed) noexcept;
		AnimationStatus SetTimePolicy(AnimatorTimePolicy policy, AnimationFixedTick fixedStepTicks = 1) noexcept;
		AnimationStatus Advance(
			const AnimatorPlaybackTimeInput& input,
			AnimatorPlaybackAdvanceResult& output) noexcept;

		[[nodiscard]] const AnimatorPlaybackConfiguration& Configuration() const noexcept { return configuration_; }
		[[nodiscard]] AnimatorPlaybackStateSnapshot Snapshot() const noexcept;
		[[nodiscard]] AnimatorPlaybackDeterministicState CaptureDeterministicState() const noexcept;
		// Restore performs a complete preflight and changes no member when the
		// copied state is malformed or incompatible with the playback contract.
		AnimationStatus RestoreDeterministicState(
			const AnimatorPlaybackDeterministicState& state) noexcept;
		[[nodiscard]] AnimatorPlaybackRunState RunState() const noexcept { return runState_; }
		[[nodiscard]] AnimationFixedTick AbsoluteTick() const noexcept { return absoluteTick_; }

	private:
		AnimationStatus ResolveCurrent(AnimationResolvedClipTime& output) const noexcept;
		AnimationStatus ConvertInputToTicks(
			const AnimatorPlaybackTimeInput& input,
			AnimationFixedTick& outputTicks) noexcept;
		void ClearFractionalTime() noexcept;

		AnimatorPlaybackConfiguration configuration_{};
		AnimatorPlaybackRunState runState_ = AnimatorPlaybackRunState::Stopped;
		AnimationFixedTick absoluteTick_ = 0;
		// Speed is quantized once to Q16.16. Integer accumulation makes fractional
		// speeds reproducible across frame grouping and worker-count changes.
		std::uint32_t speedQ16_ = 1U << 16U;
		std::uint64_t speedProductRemainder_ = 0U;
		std::uint32_t subTickRemainderQ16_ = 0U;
		AnimationStatus latestStatus_ = AnimationStatus::NotReady;
		// A default-constructed cursor has no clip-time contract yet. Keeping this
		// explicit prevents callers from accidentally playing a zero-duration
		// placeholder before the protected binding step has validated its assets.
		bool configured_ = false;
	};
}
