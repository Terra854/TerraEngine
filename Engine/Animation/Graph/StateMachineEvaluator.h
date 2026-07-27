// StateMachineEvaluator.h - Fixed-tick deterministic transition selection.

#pragma once

#include "Animation/Graph/EvaluationProgram.h"

#include <memory>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
// A trigger edge has a stable producer-assigned sequence.  Sequence identity,
// rather than a parameter name alone, lets several consumers reason about the
// same edge without accidentally consuming a later edge for that parameter.
struct StateMachineTriggerEdge {
	std::uint64_t parameterId = 0U;
	std::uint64_t sequenceId = 0U;
	bool raised = false;
	auto operator<=>(const StateMachineTriggerEdge &) const = default;
};

struct StateMachineHistoryEntry {
	AnimationFixedTick tick = 0;
	std::uint64_t transitionStableId = 0U;
	std::uint64_t sourceStateId = 0U;
	std::uint64_t destinationStateId = 0U;
	auto operator<=>(const StateMachineHistoryEntry &) const = default;
};

struct StateMachineSnapshot {
	// activeStateId is always the resolved leaf when it is non-zero.
	std::uint64_t activeStateId = 0U;
	AnimationFixedTick stateEnteredTick = 0;

	// A non-zero activeTransitionStableId represents a real in-progress blend.
	// source/destination remain immutable for its lifetime so interruption policy
	// can make an unambiguous Current-versus-Next decision on every fixed tick.
	std::uint64_t activeTransitionStableId = 0U;
	std::uint64_t transitionSourceStateId = 0U;
	std::uint64_t transitionDestinationStateId = 0U;
	AnimationFixedTick transitionStartTick = 0;
	AnimationFixedTick transitionDurationTicks = 0;
	float transitionProgressNormalized = 0.0F;
	float retainedDestinationOffsetNormalized = 0.0F;
	AnimationFixedTick transitionDestinationEnteredTick = 0;

	// Replacement preserves this metadata for pose evaluators that need to keep
	// the old blend continuous while the new blend starts at the same tick.
	std::uint64_t interruptedTransitionStableId = 0U;
	float interruptedTransitionProgressNormalized = 0.0F;

	AnimationFixedTick lastTick = 0;
	std::vector<StateMachineHistoryEntry> transitionHistory;
	std::vector<StateMachineTriggerEdge> retainedTriggerEdges;
	std::vector<EvaluationSyncMarker> syncHistory;
	auto operator<=>(const StateMachineSnapshot &) const = default;
};

struct StateMachineStepInput {
	AnimationFixedTick tick = 0;
	std::span<const StateMachineTriggerEdge> triggerEdges{};
	// Reset requests are explicit producer actions.  They are processed after
	// latching new input, so a reset can intentionally clear a just-raised edge.
	std::span<const std::uint64_t> resetTriggerSequenceIds{};
};

// Performs an allocation-safe structural preflight for an immutable program.
// Runtime callers use it before staging mutable state so malformed publication
// cannot partially advance a graph instance.
TERRAENGINE_API AnimationStatus ValidateStateMachineProgram(const EvaluationProgram &program);
TERRAENGINE_API AnimationStatus ValidateStateMachineSnapshot(const EvaluationProgram &program,
	const StateMachineSnapshot &snapshot);

// Selects one candidate from copied inputs and publishes output only when every
// policy, tick, trigger, and hierarchy constraint is valid.  On failure output
// is left untouched, allowing the caller to retain its last-good snapshot.
TERRAENGINE_API AnimationStatus StepStateMachine(const EvaluationProgram &program, const StateMachineSnapshot &prior,
	const StateMachineStepInput &input, StateMachineSnapshot &output);

// Instances call this after Publish has structurally validated the immutable
// program.  It avoids repeating graph-map allocation on every worker tick.
TERRAENGINE_API AnimationStatus StepStateMachineTrusted(const EvaluationProgram &program, const StateMachineSnapshot &prior,
	const StateMachineStepInput &input, StateMachineSnapshot &output);
} // namespace TerraEngine::Animation
