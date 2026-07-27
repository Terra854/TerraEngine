// PoseBlend.h - Deterministic CPU reference pose blending and crossfade timing.
//
// The CPU path defines the animation semantics that graph and GPU evaluators must
// match. It consumes immutable transform spans so worker jobs can blend a
// PoseBuffer snapshot without retaining mutable storage owned by another system.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// A stable order makes floating-point accumulation reproducible even when
	// worker jobs discover their inputs in a different order. Orders must be
	// unique inside one BlendLocalPoses call; an ambiguous order is rejected.
	struct WeightedLocalPose {
		std::span<const AnimationTransform> localTransforms;
		float weight = 0.0F;
		std::uint64_t stableOrder = 0U;
	};

	// Blends every compatible local transform after normalizing non-negative
	// weights. If the effective total is exactly zero, the explicit fallback pose
	// is copied instead. output changes only after the complete candidate is valid.
	TERRAENGINE_API AnimationStatus BlendLocalPoses(
		std::span<const WeightedLocalPose> inputs,
		std::span<const AnimationTransform> fallbackPose,
		std::vector<AnimationTransform>& output) noexcept;

	// FixedTicks expresses an authored transition in simulation ticks.
	// NormalizedSource multiplies normalizedDuration by the source clip length so
	// the same authored fraction scales predictably with different source clips.
	enum class PoseCrossfadeDurationMode : std::uint8_t {
		FixedTicks = 0U,
		NormalizedSource = 1U
	};

	// Curves operate on clamped linear progress. These append-only values are
	// suitable for later controller serialization and GPU parity checks.
	enum class PoseCrossfadeCurve : std::uint8_t {
		Linear = 0U,
		SmoothStep = 1U,
		EaseInQuadratic = 2U,
		EaseOutQuadratic = 3U
	};

	// Independent clocks advance each clip by its own duration. A synchronized
	// crossfade advances one normalized phase using the source duration and applies
	// separate offsets to both clips, preserving gait phase across unequal lengths.
	enum class PoseCrossfadeSyncMode : std::uint8_t {
		Independent = 0U,
		PreserveNormalizedPhase = 1U
	};

	// RejectWhileActive leaves the existing transition untouched. RestartFromCurrentPose
	// captures the currently blended pose and uses it as the next transition source.
	enum class PoseCrossfadeInterruptionPolicy : std::uint8_t {
		RejectWhileActive = 0U,
		RestartFromCurrentPose = 1U
	};

	struct PoseCrossfadeRequest {
		PoseCrossfadeDurationMode durationMode = PoseCrossfadeDurationMode::FixedTicks;
		PoseCrossfadeCurve curve = PoseCrossfadeCurve::Linear;
		PoseCrossfadeSyncMode syncMode = PoseCrossfadeSyncMode::Independent;
		PoseCrossfadeInterruptionPolicy interruptionPolicy =
			PoseCrossfadeInterruptionPolicy::RestartFromCurrentPose;
		AnimationFixedTick sourceDurationTicks = 0;
		AnimationFixedTick destinationDurationTicks = 0;
		AnimationFixedTick durationTicks = 0;
		float normalizedDuration = 0.0F;
		// initialNormalizedPhase is the caller's current synchronization phase.
		// Both finite offsets are added to it, then looping clips wrap into
		// [0, 1) while non-looping clips clamp to the closed [0, 1] interval.
		float initialNormalizedPhase = 0.0F;
		float sourceOffsetNormalized = 0.0F;
		float destinationOffsetNormalized = 0.0F;
		// Loop policy is explicit because a non-looping clip owns a closed end
		// sample at durationTicks, while a looping clip owns a half-open cycle.
		bool sourceLoop = true;
		bool destinationLoop = true;
	};

	// This copied record is safe to publish in diagnostics or an immutable runtime
	// snapshot. Sample ticks are derived from the normalized times and never point
	// beyond a clip's declared duration.
	struct PoseCrossfadeSample {
		AnimationFixedTick sourceSampleTick = 0;
		AnimationFixedTick destinationSampleTick = 0;
		AnimationFixedTick elapsedTicks = 0;
		AnimationFixedTick durationTicks = 0;
		float synchronizationPhase = 0.0F;
		float sourceNormalizedTime = 0.0F;
		float destinationNormalizedTime = 0.0F;
		float linearProgress = 0.0F;
		float sourceWeight = 1.0F;
		float destinationWeight = 0.0F;
		bool complete = false;
	};

	// PoseCrossfadeState owns only bounded transition history: its timing request
	// and, after interruption, one captured local pose. Callers still provide the
	// live source/destination samples, keeping clip evaluation task-local.
	class TERRAENGINE_API PoseCrossfadeState final {
	public:
		AnimationStatus Begin(const PoseCrossfadeRequest& request) noexcept;
		AnimationStatus Advance(AnimationFixedTick deltaTicks) noexcept;
		AnimationStatus Sample(PoseCrossfadeSample& output) const noexcept;

		// Blend uses the current curve weight and explicit fallback. When an earlier
		// transition was interrupted, sourcePose is intentionally replaced by the
		// captured prior output so progress zero remains visually continuous.
		AnimationStatus Blend(
			std::span<const AnimationTransform> sourcePose,
			std::span<const AnimationTransform> destinationPose,
			std::span<const AnimationTransform> fallbackPose,
			std::vector<AnimationTransform>& output) const noexcept;

		// Interruption is transactional: invalid timing or pose data returns an error
		// without changing this state or the caller's previous output.
		AnimationStatus Interrupt(
			const PoseCrossfadeRequest& request,
			std::span<const AnimationTransform> currentSourcePose,
			std::span<const AnimationTransform> currentDestinationPose,
			std::span<const AnimationTransform> fallbackPose) noexcept;

		bool IsInitialized() const noexcept { return initialized_; }
		bool IsActive() const noexcept { return initialized_ && active_; }
		bool UsesCapturedSourcePose() const noexcept { return usesCapturedSourcePose_; }
		AnimationFixedTick ResolvedDurationTicks() const noexcept { return resolvedDurationTicks_; }

	private:
		// BeginAtPhase is shared by a fresh request and an interruption that inherits
		// the previous synchronization phase.
		AnimationStatus BeginAtPhase(
			const PoseCrossfadeRequest& request,
			float initialPhase) noexcept;

		PoseCrossfadeRequest request_{};
		AnimationFixedTick resolvedDurationTicks_ = 0;
		AnimationFixedTick elapsedTicks_ = 0;
		float initialPhase_ = 0.0F;
		bool initialized_ = false;
		bool active_ = false;
		bool usesCapturedSourcePose_ = false;
		std::vector<AnimationTransform> capturedSourcePose_;
	};
}
