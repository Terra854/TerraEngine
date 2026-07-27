// SyncGroupEvaluator.h - Fixed-tick marker phase matching without wall-clock time.

#pragma once

#include "Animation/Graph/EvaluationProgram.h"

#include <span>
#include <vector>

namespace TerraEngine::Animation {
// A runtime marker resolves a compiled marker name to its fixed tick in the
// currently sampled clip. Marker arrays must be sorted by tick.
struct GraphSyncRuntimeMarker {
	std::uint64_t nameStableHash = 0U;
	AnimationFixedTick tick = 0;
	auto operator<=>(const GraphSyncRuntimeMarker &) const = default;
};

// Runtime state is copied into a worker-owned evaluation input. The marker span
// remains read-only for the entire call, so synchronization never edits clips.
struct GraphSyncRuntimeState {
	std::uint64_t stateId = 0U;
	AnimationFixedTick tick = 0;
	AnimationFixedTick durationTicks = 0;
	bool looping = false;
	std::span<const GraphSyncRuntimeMarker> markers{};
};

// A correction is an absolute fixed tick for one follower. reducedFidelity is
// true when named-marker matching had to fall back to normalized clip time.
struct GraphSyncCorrection {
	std::uint64_t followerStateId = 0U;
	AnimationFixedTick correctedTick = 0;
	bool reducedFidelity = false;
	auto operator<=>(const GraphSyncCorrection &) const = default;
};

// Both overloads stage every correction before replacing output. A rejected
// group therefore leaves the caller's previously published corrections intact.
TERRAENGINE_API AnimationStatus EvaluateSyncGroup(const EvaluationSyncMarker &leader, std::span<const EvaluationSyncMarker> groupMarkers,
	std::span<const GraphSyncRuntimeState> states, std::vector<GraphSyncCorrection> &output);
TERRAENGINE_API AnimationStatus EvaluateSyncGroup(const EvaluationSyncGroup &group, std::span<const EvaluationSyncMarker> groupMarkers,
	std::span<const GraphSyncRuntimeState> states, std::vector<GraphSyncCorrection> &output);
} // namespace TerraEngine::Animation
