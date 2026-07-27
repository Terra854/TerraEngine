// AnimationReplay.h - Late-join and replay-seek policy over copied snapshots.

#pragma once

#include "Animation/Determinism/AnimationHistory.h"

namespace TerraEngine::Animation {
	enum class AnimationReplayMode : std::uint8_t {
		LateJoin = 0U,
		ReplaySeek = 1U
	};

	enum class AnimationReplayEventPolicy : std::uint8_t {
		SuppressHistorical = 0U,
		ReplayHistorical = 1U
	};

	struct AnimationReplayRequest {
		AnimationReplayMode mode = AnimationReplayMode::ReplaySeek;
		AnimatorInstanceHandle animator{};
		AnimationFixedTick seekTick = 0;
		AnimationRollbackRestoreRequirements requirements{};
		AnimationReplayEventPolicy eventPolicy = AnimationReplayEventPolicy::SuppressHistorical;
		std::size_t maximumCatchUpTicks = kAnimationDeterminismMaximumHistorySnapshots;
		bool requireExactTick = false;
		auto operator<=>(const AnimationReplayRequest&) const = default;
	};

	struct AnimationReplayResult {
		AnimationStatus status = AnimationStatus::InvalidArgument;
		AnimationRollbackSnapshot resumeSnapshot{};
		AnimationFixedTick selectedSnapshotTick = 0;
		AnimationFixedTick resumeEvaluationTick = 0;
		std::size_t requiredCatchUpTicks = 0U;
		std::uint64_t nextEventSequence = 0U;
		bool suppressHistoricalOneShotEvents = true;
		std::vector<AnimationEventCursorRollbackState> historicalEvents;
		auto operator<=>(const AnimationReplayResult&) const = default;
	};

	[[nodiscard]] TERRAENGINE_API AnimationReplayResult ResolveAnimationReplay(
		const AnimationHistory& history,
		const AnimationReplayRequest& request) noexcept;

	// Compatibility helper for existing callers that only have the v1 fingerprint.
	[[nodiscard]] TERRAENGINE_API AnimationReplayResult ResumeAnimationReplay(
		const AnimationHistory& history, AnimatorInstanceHandle animator, AnimationFixedTick seekTick,
		const AnimationDeterministicFingerprint& expectedFingerprint) noexcept;
}
