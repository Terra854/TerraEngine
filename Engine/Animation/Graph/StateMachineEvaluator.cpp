// StateMachineEvaluator.cpp - Fixed-tick transition selection over immutable tables.

#include "Animation/Graph/StateMachineEvaluator.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <set>

namespace TerraEngine::Animation {
namespace {
constexpr std::size_t kHistoryLimit =
	static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);

bool IsKnownOpcode(EvaluationOpcode opcode) noexcept {
	return opcode <= EvaluationOpcode::ProceduralPose;
}

bool IsKnownPolicy(const EvaluationTransition &transition) noexcept {
	return transition.interruptionSource <= EvaluationTransitionInterruptionSource::Next &&
		transition.interruptionPolicy <= EvaluationTransitionInterruptionPolicy::Replace &&
		transition.triggerConsumption <= EvaluationTriggerConsumption::MultiConsumer;
}

bool CheckedSubtract(AnimationFixedTick left, AnimationFixedTick right, AnimationFixedTick &result) noexcept {
	if ((right > 0 && left < std::numeric_limits<AnimationFixedTick>::min() + right) ||
		(right < 0 && left > std::numeric_limits<AnimationFixedTick>::max() + right)) {
		return false;
	}
	result = left - right;
	return true;
}

const EvaluationState *FindState(const EvaluationProgram &program, std::uint64_t stableId) noexcept {
	const auto iterator = std::find_if(program.states.begin(), program.states.end(),
		[stableId](const EvaluationState &state) { return state.stableId == stableId; });
	return iterator != program.states.end() ? &*iterator : nullptr;
}

bool IsDescendantOrSame(const EvaluationProgram &program, std::uint64_t stateId, std::uint64_t scopeId,
	std::size_t &distance) noexcept {
	distance = 0U;
	const EvaluationState *current = FindState(program, stateId);
	while (current != nullptr) {
		if (current->stableId == scopeId) {
			return true;
		}
		if (++distance > GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum || current->parentStateId == 0U) {
			break;
		}
		current = FindState(program, current->parentStateId);
	}
	return false;
}

// An any-state record belongs to its parent state-machine scope.  A root scope
// is represented by ID zero, so every root subtree is inside that scope.
bool IsInsideAnyStateScope(const EvaluationProgram &program, std::uint64_t leafId, std::uint64_t scopeId,
	std::size_t &distance) noexcept {
	distance = 0U;
	const EvaluationState *current = FindState(program, leafId);
	while (current != nullptr) {
		if ((scopeId == 0U && current->parentStateId == 0U) || current->stableId == scopeId) {
			return true;
		}
		if (++distance > GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum || current->parentStateId == 0U) {
			break;
		}
		current = FindState(program, current->parentStateId);
	}
	return false;
}

bool ResolveEntryLeaf(const EvaluationProgram &program, std::uint64_t scopeId, std::uint64_t &leaf) noexcept {
	const EvaluationState *current = FindState(program, scopeId);
	if (current == nullptr) {
		return false;
	}
	for (std::size_t depth = 0U; depth <= GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum; ++depth) {
		const EvaluationState *entryChild = nullptr;
		for (const EvaluationState &candidate : program.states) {
			if (candidate.parentStateId == current->stableId && candidate.entry) {
				if (entryChild != nullptr) {
					return false;
				}
				entryChild = &candidate;
			}
		}
		if (entryChild == nullptr) {
			leaf = current->stableId;
			return true;
		}
		current = entryChild;
	}
	return false;
}

// An exit node is a sub-state-machine control node.  Reaching it leaves that
// node's parent scope and resumes at the ancestor state without re-entering its
// child default.  Chained exits bubble further until a real ancestor remains.
bool ResolveDestinationLeaf(const EvaluationProgram &program, std::uint64_t destinationId, std::uint64_t &leaf) noexcept {
	const EvaluationState *current = FindState(program, destinationId);
	if (current == nullptr) {
		return false;
	}
	for (std::size_t depth = 0U; current->exit; ++depth) {
		if (depth >= GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum || current->parentStateId == 0U) {
			return false;
		}
		current = FindState(program, current->parentStateId);
		if (current == nullptr) {
			return false;
		}
	}
	if (destinationId != current->stableId) {
		leaf = current->stableId;
		return true;
	}
	return ResolveEntryLeaf(program, destinationId, leaf);
}

bool ValidateInputEdges(std::span<const StateMachineTriggerEdge> edges) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, edges.size()) != AnimationStatus::Ok) {
		return false;
	}
	for (std::size_t left = 0U; left < edges.size(); ++left) {
		if (!edges[left].raised || edges[left].parameterId == 0U || edges[left].sequenceId == 0U) {
			return false;
		}
		for (std::size_t right = left + 1U; right < edges.size(); ++right) {
			if (edges[left].sequenceId == edges[right].sequenceId) {
				return false;
			}
		}
	}
	return true;
}

bool ValidateResetRequests(std::span<const std::uint64_t> resetIds) noexcept {
	if (ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, resetIds.size()) != AnimationStatus::Ok) {
		return false;
	}
	for (std::size_t left = 0U; left < resetIds.size(); ++left) {
		if (resetIds[left] == 0U) {
			return false;
		}
		for (std::size_t right = left + 1U; right < resetIds.size(); ++right) {
			if (resetIds[left] == resetIds[right]) {
				return false;
			}
		}
	}
	return true;
}

AnimationStatus PreflightIncomingEdges(const StateMachineSnapshot &prior, std::span<const StateMachineTriggerEdge> input) noexcept {
	std::size_t newCount = 0U;
	for (const StateMachineTriggerEdge &edge : input) {
		const auto retained = std::find_if(prior.retainedTriggerEdges.begin(), prior.retainedTriggerEdges.end(),
			[&edge](const StateMachineTriggerEdge &candidate) { return candidate.sequenceId == edge.sequenceId; });
		if (retained == prior.retainedTriggerEdges.end()) {
			++newCount;
		} else if (*retained != edge) {
			return AnimationStatus::InvalidArgument;
		}
	}
	return prior.retainedTriggerEdges.size() > kHistoryLimit - newCount ? AnimationStatus::HardLimitExceeded : AnimationStatus::Ok;
}

template <typename T> void TrimOldest(std::vector<T> &history) {
	if (history.size() > kHistoryLimit) {
		history.erase(history.begin(), history.begin() + static_cast<std::ptrdiff_t>(history.size() - kHistoryLimit));
	}
}

void LatchTriggerEdges(StateMachineSnapshot &staged, std::span<const StateMachineTriggerEdge> input) {
	for (const StateMachineTriggerEdge &edge : input) {
		const auto retained = std::find_if(staged.retainedTriggerEdges.begin(), staged.retainedTriggerEdges.end(),
			[&edge](const StateMachineTriggerEdge &candidate) { return candidate.sequenceId == edge.sequenceId; });
		if (retained == staged.retainedTriggerEdges.end()) {
			staged.retainedTriggerEdges.push_back(edge);
		}
	}
	std::sort(staged.retainedTriggerEdges.begin(), staged.retainedTriggerEdges.end(),
		[](const StateMachineTriggerEdge &left, const StateMachineTriggerEdge &right) { return left.sequenceId < right.sequenceId; });
}

const StateMachineTriggerEdge *FindTrigger(const StateMachineSnapshot &snapshot, std::uint64_t parameterId) noexcept {
	const auto iterator = std::find_if(snapshot.retainedTriggerEdges.begin(), snapshot.retainedTriggerEdges.end(),
		[parameterId](const StateMachineTriggerEdge &edge) { return edge.parameterId == parameterId; });
	return iterator == snapshot.retainedTriggerEdges.end() ? nullptr : &*iterator;
}

void ApplyTriggerConsumption(StateMachineSnapshot &snapshot, const EvaluationTransition &transition) {
	if (transition.conditionParameterId == 0U) {
		return;
	}
	const StateMachineTriggerEdge *selected = FindTrigger(snapshot, transition.conditionParameterId);
	if (selected == nullptr) {
		return;
	}
	const std::uint64_t selectedSequence = selected->sequenceId;
	if (transition.triggerConsumption == EvaluationTriggerConsumption::SingleConsumer) {
		snapshot.retainedTriggerEdges.erase(std::remove_if(snapshot.retainedTriggerEdges.begin(), snapshot.retainedTriggerEdges.end(),
			[selectedSequence](const StateMachineTriggerEdge &edge) { return edge.sequenceId == selectedSequence; }), snapshot.retainedTriggerEdges.end());
	} else if (transition.triggerResetAtEndStep) {
		snapshot.retainedTriggerEdges.erase(std::remove_if(snapshot.retainedTriggerEdges.begin(), snapshot.retainedTriggerEdges.end(),
			[&transition](const StateMachineTriggerEdge &edge) { return edge.parameterId == transition.conditionParameterId; }), snapshot.retainedTriggerEdges.end());
	}
}

struct Candidate {
	const EvaluationTransition *transition = nullptr;
	std::uint64_t sourceLeaf = 0U;
	std::size_t scopeDistance = 0U;
};

bool CandidateBefore(const Candidate &left, const Candidate &right) noexcept {
	if (left.transition->priority != right.transition->priority) {
		return left.transition->priority > right.transition->priority;
	}
	if (left.transition->anyState != right.transition->anyState) {
		return !left.transition->anyState;
	}
	if (left.scopeDistance != right.scopeDistance) {
		return left.scopeDistance < right.scopeDistance;
	}
	return left.transition->stableId < right.transition->stableId;
}

void ConsiderCandidates(const EvaluationProgram &program, const StateMachineSnapshot &staged, AnimationFixedTick tick,
	std::uint64_t leaf, bool requireInterruption, EvaluationTransitionInterruptionSource interruptionSource,
	Candidate &best) noexcept {
	for (const EvaluationTransition &transition : program.transitions) {
		if (requireInterruption && transition.interruptionSource != interruptionSource) {
			continue;
		}
		const EvaluationState *source = FindState(program, transition.sourceStateId);
		std::size_t distance = 0U;
		const bool applies = source != nullptr && (transition.anyState
			? IsInsideAnyStateScope(program, leaf, source->parentStateId, distance)
			: IsDescendantOrSame(program, leaf, transition.sourceStateId, distance));
		if (!applies) {
			continue;
		}
		AnimationFixedTick elapsed = 0;
		if (!CheckedSubtract(tick, staged.stateEnteredTick, elapsed) || elapsed < 0 || elapsed < transition.exitTimeTicks ||
			(transition.conditionParameterId != 0U && FindTrigger(staged, transition.conditionParameterId) == nullptr)) {
			continue;
		}
		const Candidate candidate{&transition, leaf, distance};
		if (best.transition == nullptr || CandidateBefore(candidate, best)) {
			best = candidate;
		}
	}
}

bool ValidatePriorSnapshot(const EvaluationProgram &program, const StateMachineSnapshot &prior, bool full) noexcept {
	if ((prior.activeStateId != 0U && FindState(program, prior.activeStateId) == nullptr) ||
		(prior.activeStateId != 0U && prior.stateEnteredTick > prior.lastTick) ||
		prior.transitionHistory.size() > kHistoryLimit || prior.retainedTriggerEdges.size() > kHistoryLimit || prior.syncHistory.size() > kHistoryLimit) {
		return false;
	}
	if (prior.activeTransitionStableId != 0U &&
		(FindState(program, prior.transitionSourceStateId) == nullptr || FindState(program, prior.transitionDestinationStateId) == nullptr ||
		prior.activeStateId != prior.transitionSourceStateId || prior.transitionStartTick > prior.lastTick || prior.transitionDurationTicks <= 0 ||
		!std::isfinite(prior.transitionProgressNormalized) || prior.transitionProgressNormalized < 0.0F || prior.transitionProgressNormalized > 1.0F)) {
		return false;
	}
	if (!full) {
		return true;
	}
	AnimationFixedTick previousTick = std::numeric_limits<AnimationFixedTick>::min();
	for (const StateMachineHistoryEntry &entry : prior.transitionHistory) {
		if (entry.tick < previousTick || entry.tick > prior.lastTick || FindState(program, entry.sourceStateId) == nullptr ||
			FindState(program, entry.destinationStateId) == nullptr) {
			return false;
		}
		previousTick = entry.tick;
	}
	for (std::size_t left = 0U; left < prior.retainedTriggerEdges.size(); ++left) {
		const StateMachineTriggerEdge &edge = prior.retainedTriggerEdges[left];
		if (!edge.raised || edge.parameterId == 0U || edge.sequenceId == 0U) {
			return false;
		}
		for (std::size_t right = left + 1U; right < prior.retainedTriggerEdges.size(); ++right) {
			if (edge.sequenceId == prior.retainedTriggerEdges[right].sequenceId) {
				return false;
			}
		}
	}
	for (const EvaluationSyncMarker &marker : prior.syncHistory) {
		if (marker.markerStableId == 0U || FindState(program, marker.stateId) == nullptr) {
			return false;
		}
	}
	return true;
}

void UpdateTransitionProgress(StateMachineSnapshot &staged, AnimationFixedTick tick) noexcept {
	AnimationFixedTick elapsed = 0;
	if (CheckedSubtract(tick, staged.transitionStartTick, elapsed)) {
		staged.transitionProgressNormalized = std::clamp(
			static_cast<float>(static_cast<long double>(elapsed) / static_cast<long double>(staged.transitionDurationTicks)), 0.0F, 1.0F);
	}
}

void AppendSyncHistory(const EvaluationProgram &program, StateMachineSnapshot &staged) {
	for (const EvaluationSyncMarker &marker : program.syncMarkers) {
		if (marker.stateId == staged.activeStateId) {
			staged.syncHistory.push_back(marker);
		}
	}
	TrimOldest(staged.syncHistory);
}

AnimationStatus StepInternal(const EvaluationProgram &program, const StateMachineSnapshot &prior, const StateMachineStepInput &input,
	StateMachineSnapshot &output, bool preflightProgram) {
	try {
		if ((preflightProgram && ValidateStateMachineProgram(program) != AnimationStatus::Ok) ||
			!ValidatePriorSnapshot(program, prior, preflightProgram) || input.tick < prior.lastTick ||
			!ValidateInputEdges(input.triggerEdges) || !ValidateResetRequests(input.resetTriggerSequenceIds)) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus edgeStatus = PreflightIncomingEdges(prior, input.triggerEdges);
		if (edgeStatus != AnimationStatus::Ok) {
			return edgeStatus;
		}

		StateMachineSnapshot staged = prior;
		LatchTriggerEdges(staged, input.triggerEdges);
		for (const std::uint64_t resetId : input.resetTriggerSequenceIds) {
			staged.retainedTriggerEdges.erase(std::remove_if(staged.retainedTriggerEdges.begin(), staged.retainedTriggerEdges.end(),
				[resetId](const StateMachineTriggerEdge &edge) { return edge.sequenceId == resetId; }), staged.retainedTriggerEdges.end());
		}
		if (staged.activeStateId == 0U) {
			const auto entry = std::find_if(program.states.begin(), program.states.end(),
				[](const EvaluationState &state) { return state.parentStateId == 0U && state.entry; });
			if (entry == program.states.end() || !ResolveEntryLeaf(program, entry->stableId, staged.activeStateId)) {
				return AnimationStatus::Rejected;
			}
			staged.stateEnteredTick = input.tick;
		}
		if (staged.activeTransitionStableId != 0U) {
			UpdateTransitionProgress(staged, input.tick);
			if (staged.transitionProgressNormalized >= 1.0F) {
				staged.activeStateId = staged.transitionDestinationStateId;
				staged.stateEnteredTick = staged.transitionDestinationEnteredTick;
				staged.activeTransitionStableId = 0U;
			}
		}

		Candidate selected{};
		if (staged.activeTransitionStableId == 0U) {
			ConsiderCandidates(program, staged, input.tick, staged.activeStateId, false, EvaluationTransitionInterruptionSource::None, selected);
		} else {
			ConsiderCandidates(program, staged, input.tick, staged.transitionSourceStateId, true,
				EvaluationTransitionInterruptionSource::Current, selected);
			ConsiderCandidates(program, staged, input.tick, staged.transitionDestinationStateId, true,
				EvaluationTransitionInterruptionSource::Next, selected);
		}
		if (selected.transition != nullptr) {
			const EvaluationTransition &transition = *selected.transition;
			if (staged.activeTransitionStableId != 0U && transition.interruptionPolicy == EvaluationTransitionInterruptionPolicy::Reject) {
				if (!transition.resetTriggerOnFailure) {
					return AnimationStatus::Rejected;
				}
				ApplyTriggerConsumption(staged, transition);
				AppendSyncHistory(program, staged);
				staged.lastTick = input.tick;
				output = std::move(staged);
				return AnimationStatus::Ok;
			}
			std::uint64_t destinationLeaf = 0U;
			if (!ResolveDestinationLeaf(program, transition.destinationStateId, destinationLeaf)) {
				return AnimationStatus::Rejected;
			}
			if (staged.activeTransitionStableId != 0U) {
				const std::uint64_t priorDestination =
					staged.transitionDestinationStateId;
				const AnimationFixedTick priorDestinationEnteredTick =
					staged.transitionDestinationEnteredTick;
				staged.interruptedTransitionStableId = staged.activeTransitionStableId;
				staged.interruptedTransitionProgressNormalized = staged.transitionProgressNormalized;
				// A Next-source interruption promotes that destination to the new
				// logical source before replacing the transition. Keeping the older
				// Current state here would create an invalid snapshot that fails on
				// the following fixed tick and visually freezes an otherwise valid
				// gameplay crossfade.
				staged.activeStateId = selected.sourceLeaf;
				if(selected.sourceLeaf == priorDestination) {
					staged.stateEnteredTick = priorDestinationEnteredTick;
				}
			}
			staged.activeTransitionStableId = transition.stableId;
			staged.transitionSourceStateId = selected.sourceLeaf;
			staged.transitionDestinationStateId = destinationLeaf;
			staged.transitionStartTick = input.tick;
			staged.transitionDurationTicks = transition.durationTicks;
			staged.transitionProgressNormalized = 0.0F;
			// Normalized offset belongs to the destination pose sampler, not the
			// crossfade duration.  State time starts at the decision boundary.
			staged.retainedDestinationOffsetNormalized = transition.destinationOffsetNormalized;
			staged.transitionDestinationEnteredTick = input.tick;
			staged.transitionHistory.push_back({input.tick, transition.stableId, selected.sourceLeaf, destinationLeaf});
			TrimOldest(staged.transitionHistory);
			ApplyTriggerConsumption(staged, transition);
			if (transition.durationTicks == 0) {
				staged.activeStateId = destinationLeaf;
				staged.stateEnteredTick = input.tick;
				staged.activeTransitionStableId = 0U;
			}
		}
		AppendSyncHistory(program, staged);
		staged.lastTick = input.tick;
		output = std::move(staged);
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}
} // namespace

AnimationStatus ValidateStateMachineProgram(const EvaluationProgram &program) {
	try {
		if (!program.controllerAssetId.IsValid() || program.compatibilityFingerprint == 0U ||
			program.stableHash != ComputeEvaluationProgramStableHash(program) ||
			ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, program.states.size()) != AnimationStatus::Ok ||
			ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, program.transitions.size()) != AnimationStatus::Ok) {
			return AnimationStatus::InvalidArgument;
		}
		std::set<std::uint64_t> stateIds;
		std::map<std::uint64_t, std::size_t> entriesByScope;
		std::map<std::uint64_t, std::size_t> childrenByScope;
		for (const EvaluationState &state : program.states) {
			if (state.stableId == 0U || !IsKnownOpcode(state.nodeOpcode) || !stateIds.insert(state.stableId).second ||
				(state.exit && state.parentStateId == 0U)) {
				return AnimationStatus::InvalidArgument;
			}
			if (state.entry) {
				++entriesByScope[state.parentStateId];
			}
			if (state.parentStateId != 0U) {
				++childrenByScope[state.parentStateId];
			}
		}
		for (const EvaluationState &state : program.states) {
			if (state.parentStateId != 0U && !stateIds.contains(state.parentStateId)) {
				return AnimationStatus::InvalidArgument;
			}
			std::set<std::uint64_t> visited;
			const EvaluationState *current = &state;
			for (std::size_t depth = 0U; current->parentStateId != 0U; ++depth) {
				if (depth >= GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum || !visited.insert(current->stableId).second) {
					return AnimationStatus::InvalidArgument;
				}
				current = FindState(program, current->parentStateId);
				if (current == nullptr) {
					return AnimationStatus::InvalidArgument;
				}
			}
		}
		if (entriesByScope[0U] != 1U) {
			return AnimationStatus::InvalidArgument;
		}
		for (const auto &[scope, childCount] : childrenByScope) {
			(void)childCount;
			if (entriesByScope[scope] != 1U) {
				return AnimationStatus::InvalidArgument;
			}
		}
		std::set<std::uint64_t> transitionIds;
		for (const EvaluationTransition &transition : program.transitions) {
			if (transition.stableId == 0U || !transitionIds.insert(transition.stableId).second || !IsKnownPolicy(transition) ||
				FindState(program, transition.sourceStateId) == nullptr || FindState(program, transition.destinationStateId) == nullptr ||
				transition.durationTicks < 0 || transition.exitTimeTicks < 0 || !std::isfinite(transition.destinationOffsetNormalized) ||
				transition.destinationOffsetNormalized < 0.0F || transition.destinationOffsetNormalized > 1.0F) {
				return AnimationStatus::InvalidArgument;
			}
			const EvaluationState *source = FindState(program, transition.sourceStateId);
			if (source->anyState != transition.anyState) {
				return AnimationStatus::InvalidArgument;
			}
		}
		for (const EvaluationSyncMarker &marker : program.syncMarkers) {
			if (marker.groupStableId == 0U || marker.markerStableId == 0U || marker.clipEventId == 0U || FindState(program, marker.stateId) == nullptr) {
				return AnimationStatus::InvalidArgument;
			}
		}
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

AnimationStatus ValidateStateMachineSnapshot(const EvaluationProgram &program, const StateMachineSnapshot &snapshot) {
	return ValidatePriorSnapshot(program, snapshot, true) ? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
}

AnimationStatus StepStateMachine(const EvaluationProgram &program, const StateMachineSnapshot &prior,
	const StateMachineStepInput &input, StateMachineSnapshot &output) {
	return StepInternal(program, prior, input, output, true);
}

AnimationStatus StepStateMachineTrusted(const EvaluationProgram &program, const StateMachineSnapshot &prior,
	const StateMachineStepInput &input, StateMachineSnapshot &output) {
	return StepInternal(program, prior, input, output, false);
}
} // namespace TerraEngine::Animation
