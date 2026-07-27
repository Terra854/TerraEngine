// SyncGroupEvaluator.cpp - Fixed-tick adjacent-marker phase matching.

#include "Animation/Graph/SyncGroupEvaluator.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace TerraEngine::Animation {
namespace {
struct MarkerInterval {
	std::uint64_t previousHash = 0U;
	std::uint64_t nextHash = 0U;
	AnimationFixedTick startTick = 0;
	AnimationFixedTick lengthTicks = 0;
	AnimationFixedTick phaseTicks = 0;
};

// Validation produces canonical pointer views without copying marker payloads.
// Their sizes are checked against the controller hard limit before allocation.
struct ValidatedSyncInputs {
	std::vector<const EvaluationSyncMarker *> declarationsByStateAndName;
	std::vector<const GraphSyncRuntimeState *> statesById;
	const GraphSyncRuntimeState *leader = nullptr;
};

bool NormalizeTick(const GraphSyncRuntimeState &state, AnimationFixedTick &tick) noexcept {
	if (state.durationTicks <= 0 || state.tick < 0)
		return false;
	if (!state.looping) {
		if (state.tick > state.durationTicks)
			return false;
		tick = state.tick;
		return true;
	}
	tick = state.tick % state.durationTicks;
	return true;
}

AnimationStatus ValidateState(const GraphSyncRuntimeState &state) noexcept {
	const AnimationStatus markerLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, state.markers.size());
	if (markerLimit != AnimationStatus::Ok)
		return markerLimit;
	AnimationFixedTick normalized = 0;
	if (state.stateId == 0U || !NormalizeTick(state, normalized))
		return AnimationStatus::InvalidArgument;
	for (std::size_t index = 0U; index < state.markers.size(); ++index) {
		const GraphSyncRuntimeMarker &marker = state.markers[index];
		if (marker.nameStableHash == 0U || marker.tick < 0 || marker.tick >= state.durationTicks ||
			(index > 0U && state.markers[index - 1U].tick >= marker.tick))
			return AnimationStatus::InvalidArgument;
	}
	return AnimationStatus::Ok;
}

bool DeclarationLessByStateAndName(const EvaluationSyncMarker *left, const EvaluationSyncMarker *right) noexcept {
	if (left->stateId != right->stateId)
		return left->stateId < right->stateId;
	if (left->nameStableHash != right->nameStableHash)
		return left->nameStableHash < right->nameStableHash;
	return left->markerStableId < right->markerStableId;
}

bool IsRuntimeMarkerDeclared(const ValidatedSyncInputs &validated, std::uint64_t stateId, std::uint64_t nameHash) noexcept {
	const auto iterator = std::lower_bound(validated.declarationsByStateAndName.begin(),
		validated.declarationsByStateAndName.end(), std::pair{stateId, nameHash}, [](const EvaluationSyncMarker *marker, const auto &key) {
			return marker->stateId != key.first ? marker->stateId < key.first : marker->nameStableHash < key.second;
		});
	return iterator != validated.declarationsByStateAndName.end() && (*iterator)->stateId == stateId &&
		(*iterator)->nameStableHash == nameHash;
}

AnimationStatus ValidateInputs(const EvaluationSyncGroup &group, std::span<const EvaluationSyncMarker> groupMarkers,
	std::span<const GraphSyncRuntimeState> states, ValidatedSyncInputs &validated) {
	if (group.stableId == 0U || group.leaderStateId == 0U || groupMarkers.empty() || states.empty())
		return AnimationStatus::InvalidArgument;
	const AnimationStatus declarationLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, groupMarkers.size());
	const AnimationStatus stateLimit = ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, states.size());
	if (declarationLimit != AnimationStatus::Ok)
		return declarationLimit;
	if (stateLimit != AnimationStatus::Ok)
		return stateLimit;

	std::vector<const EvaluationSyncMarker *> declarationsByStableId;
	std::vector<const EvaluationSyncMarker *> declarationsByStateAndEvent;
	validated.declarationsByStateAndName.reserve(groupMarkers.size());
	declarationsByStableId.reserve(groupMarkers.size());
	declarationsByStateAndEvent.reserve(groupMarkers.size());
	for (const EvaluationSyncMarker &marker : groupMarkers) {
		if (marker.groupStableId != group.stableId || marker.markerStableId == 0U || marker.clipEventId == 0U ||
			marker.stateId == 0U || marker.nameStableHash == 0U)
			return AnimationStatus::InvalidArgument;
		validated.declarationsByStateAndName.push_back(&marker);
		declarationsByStableId.push_back(&marker);
		declarationsByStateAndEvent.push_back(&marker);
	}

	std::sort(validated.declarationsByStateAndName.begin(), validated.declarationsByStateAndName.end(), DeclarationLessByStateAndName);
	std::sort(declarationsByStableId.begin(), declarationsByStableId.end(), [](const auto *left, const auto *right) {
		return left->markerStableId < right->markerStableId;
	});
	std::sort(declarationsByStateAndEvent.begin(), declarationsByStateAndEvent.end(), [](const auto *left, const auto *right) {
		return left->stateId != right->stateId ? left->stateId < right->stateId : left->clipEventId < right->clipEventId;
	});
	for (std::size_t index = 1U; index < groupMarkers.size(); ++index) {
		const EvaluationSyncMarker &priorName = *validated.declarationsByStateAndName[index - 1U];
		const EvaluationSyncMarker &name = *validated.declarationsByStateAndName[index];
		const EvaluationSyncMarker &priorEvent = *declarationsByStateAndEvent[index - 1U];
		const EvaluationSyncMarker &event = *declarationsByStateAndEvent[index];
		if (declarationsByStableId[index - 1U]->markerStableId == declarationsByStableId[index]->markerStableId ||
			(priorName.stateId == name.stateId && priorName.nameStableHash == name.nameStableHash) ||
			(priorEvent.stateId == event.stateId && priorEvent.clipEventId == event.clipEventId))
			return AnimationStatus::InvalidArgument;
	}

	// One runtime state must exist for every distinct compiled member state. This
	// rejects missing and unrelated inputs before any follower correction runs.
	std::size_t participantCount = 0U;
	std::uint64_t previousParticipant = 0U;
	bool leaderDeclared = false;
	for (const EvaluationSyncMarker *marker : validated.declarationsByStateAndName) {
		if (participantCount == 0U || marker->stateId != previousParticipant) {
			++participantCount;
			previousParticipant = marker->stateId;
		}
		leaderDeclared = leaderDeclared || marker->stateId == group.leaderStateId;
	}
	if (!leaderDeclared || participantCount != states.size())
		return AnimationStatus::InvalidArgument;

	validated.statesById.reserve(states.size());
	std::size_t runtimeMarkerTotal = 0U;
	for (const GraphSyncRuntimeState &state : states) {
		const AnimationStatus stateStatus = ValidateState(state);
		if (stateStatus != AnimationStatus::Ok)
			return stateStatus;
		const std::size_t maximum = static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum);
		if (state.markers.size() > maximum - runtimeMarkerTotal)
			return AnimationStatus::HardLimitExceeded;
		runtimeMarkerTotal += state.markers.size();
		validated.statesById.push_back(&state);
	}
	std::sort(validated.statesById.begin(), validated.statesById.end(), [](const auto *left, const auto *right) {
		return left->stateId < right->stateId;
	});

	// A separately sorted key list makes duplicate-name validation O(n log n)
	// even at the authored hard limit. It also keeps every allocation bounded by
	// the already checked aggregate marker count.
	std::vector<std::pair<std::uint64_t, std::uint64_t>> runtimeNames;
	runtimeNames.reserve(runtimeMarkerTotal);
	for (const GraphSyncRuntimeState *state : validated.statesById)
		for (const GraphSyncRuntimeMarker &marker : state->markers)
			runtimeNames.emplace_back(state->stateId, marker.nameStableHash);
	std::sort(runtimeNames.begin(), runtimeNames.end());
	if (std::adjacent_find(runtimeNames.begin(), runtimeNames.end()) != runtimeNames.end())
		return AnimationStatus::InvalidArgument;

	std::size_t declarationIndex = 0U;
	for (std::size_t stateIndex = 0U; stateIndex < validated.statesById.size(); ++stateIndex) {
		if (stateIndex > 0U && validated.statesById[stateIndex - 1U]->stateId == validated.statesById[stateIndex]->stateId)
			return AnimationStatus::InvalidArgument;
		while (declarationIndex < validated.declarationsByStateAndName.size() &&
			validated.declarationsByStateAndName[declarationIndex]->stateId < validated.statesById[stateIndex]->stateId)
			++declarationIndex;
		if (declarationIndex == validated.declarationsByStateAndName.size() ||
			validated.declarationsByStateAndName[declarationIndex]->stateId != validated.statesById[stateIndex]->stateId)
			return AnimationStatus::InvalidArgument;
		for (const GraphSyncRuntimeMarker &marker : validated.statesById[stateIndex]->markers)
			if (!IsRuntimeMarkerDeclared(validated, validated.statesById[stateIndex]->stateId, marker.nameStableHash))
				return AnimationStatus::InvalidArgument;
	}
	const auto leader = std::lower_bound(validated.statesById.begin(), validated.statesById.end(), group.leaderStateId,
		[](const GraphSyncRuntimeState *state, std::uint64_t stateId) { return state->stateId < stateId; });
	if (leader == validated.statesById.end() || (*leader)->stateId != group.leaderStateId)
		return AnimationStatus::InvalidArgument;
	validated.leader = *leader;
	return AnimationStatus::Ok;
}

bool FindContainingInterval(const GraphSyncRuntimeState &state, MarkerInterval &output) noexcept {
	if (state.markers.size() < 2U)
		return false;
	AnimationFixedTick tick = 0;
	if (!NormalizeTick(state, tick))
		return false;
	const auto upper = std::upper_bound(state.markers.begin(), state.markers.end(), tick,
		[](AnimationFixedTick value, const GraphSyncRuntimeMarker &marker) { return value < marker.tick; });
	std::size_t previous = 0U;
	if (upper == state.markers.begin()) {
		if (!state.looping)
			return false;
		previous = state.markers.size() - 1U;
	} else {
		previous = static_cast<std::size_t>(std::distance(state.markers.begin(), upper) - 1);
	}
	const std::size_t next = previous + 1U == state.markers.size() ? 0U : previous + 1U;
	if (next == 0U && !state.looping)
		return false;
	const AnimationFixedTick start = state.markers[previous].tick;
	const AnimationFixedTick length = next == 0U
		? (state.durationTicks - start) + state.markers[next].tick
		: state.markers[next].tick - start;
	const AnimationFixedTick phase = tick >= start ? tick - start : (state.durationTicks - start) + tick;
	if (length <= 0 || phase < 0 || phase >= length)
		return false;
	output = {state.markers[previous].nameStableHash, state.markers[next].nameStableHash, start, length, phase};
	return true;
}

bool FindMatchingInterval(const GraphSyncRuntimeState &state, std::uint64_t previousHash, std::uint64_t nextHash,
	MarkerInterval &output) noexcept {
	if (state.markers.size() < 2U)
		return false;
	for (std::size_t index = 0U; index < state.markers.size(); ++index) {
		const std::size_t next = index + 1U == state.markers.size() ? 0U : index + 1U;
		if (next == 0U && !state.looping)
			continue;
		if (state.markers[index].nameStableHash != previousHash || state.markers[next].nameStableHash != nextHash)
			continue;
		const AnimationFixedTick length = next == 0U
			? (state.durationTicks - state.markers[index].tick) + state.markers[next].tick
			: state.markers[next].tick - state.markers[index].tick;
		if (length <= 0)
			continue;
		output = {previousHash, nextHash, state.markers[index].tick, length, 0};
		return true;
	}
	return false;
}

bool MultiplyDivide(AnimationFixedTick value, AnimationFixedTick numerator, AnimationFixedTick denominator,
	AnimationFixedTick &output) noexcept {
	if (value < 0 || numerator < 0 || denominator <= 0)
		return false;
	if (numerator == 0) {
		output = 0;
		return true;
	}
	if (value > std::numeric_limits<AnimationFixedTick>::max() / numerator)
		return false;
	output = (value * numerator) / denominator;
	return true;
}

bool ResolveFollowerTick(const GraphSyncRuntimeState &follower, const MarkerInterval &interval,
	AnimationFixedTick mappedPhase, AnimationFixedTick &output) noexcept {
	if (mappedPhase < 0 || mappedPhase >= interval.lengthTicks)
		return false;
	if (follower.looping) {
		const AnimationFixedTick ticksUntilWrap = follower.durationTicks - interval.startTick;
		output = mappedPhase >= ticksUntilWrap ? mappedPhase - ticksUntilWrap : interval.startTick + mappedPhase;
		return output >= 0 && output < follower.durationTicks;
	}
	if (interval.startTick > std::numeric_limits<AnimationFixedTick>::max() - mappedPhase)
		return false;
	output = interval.startTick + mappedPhase;
	return output >= 0 && output <= follower.durationTicks;
}

bool NormalizedFallback(const GraphSyncRuntimeState &leader, const GraphSyncRuntimeState &follower,
	AnimationFixedTick &output) noexcept {
	AnimationFixedTick leaderTick = 0;
	if (!NormalizeTick(leader, leaderTick) || !MultiplyDivide(leaderTick, follower.durationTicks, leader.durationTicks, output))
		return false;
	if (follower.looping)
		output %= follower.durationTicks;
	else
		output = std::clamp(output, AnimationFixedTick{0}, follower.durationTicks);
	return true;
}
} // namespace

AnimationStatus EvaluateSyncGroup(const EvaluationSyncGroup &group, std::span<const EvaluationSyncMarker> groupMarkers,
	std::span<const GraphSyncRuntimeState> states, std::vector<GraphSyncCorrection> &output) {
	try {
		ValidatedSyncInputs validated;
		const AnimationStatus validationStatus = ValidateInputs(group, groupMarkers, states, validated);
		if (validationStatus != AnimationStatus::Ok)
			return validationStatus;
		MarkerInterval leaderInterval{};
		const bool leaderHasInterval = FindContainingInterval(*validated.leader, leaderInterval);
		std::vector<GraphSyncCorrection> staged;
		staged.reserve(validated.statesById.size() - 1U);
		for (const GraphSyncRuntimeState *follower : validated.statesById) {
			if (follower->stateId == group.leaderStateId)
				continue;
			MarkerInterval followerInterval{};
			GraphSyncCorrection correction{};
			correction.followerStateId = follower->stateId;
			if (leaderHasInterval && FindMatchingInterval(*follower, leaderInterval.previousHash, leaderInterval.nextHash, followerInterval)) {
				AnimationFixedTick mapped = 0;
				if (!MultiplyDivide(leaderInterval.phaseTicks, followerInterval.lengthTicks, leaderInterval.lengthTicks, mapped) ||
					!ResolveFollowerTick(*follower, followerInterval, mapped, correction.correctedTick))
					return AnimationStatus::RangeOverflow;
			} else {
				if (!NormalizedFallback(*validated.leader, *follower, correction.correctedTick))
					return AnimationStatus::RangeOverflow;
				correction.reducedFidelity = true;
			}
			staged.push_back(correction);
		}
		output = std::move(staged);
		return AnimationStatus::Ok;
	} catch (const std::bad_alloc &) {
		return AnimationStatus::InternalError;
	}
}

AnimationStatus EvaluateSyncGroup(const EvaluationSyncMarker &leader, std::span<const EvaluationSyncMarker> groupMarkers,
	std::span<const GraphSyncRuntimeState> states, std::vector<GraphSyncCorrection> &output) {
	if (leader.groupStableId == 0U || leader.markerStableId == 0U || leader.clipEventId == 0U || leader.stateId == 0U ||
		leader.nameStableHash == 0U || std::find(groupMarkers.begin(), groupMarkers.end(), leader) == groupMarkers.end())
		return AnimationStatus::InvalidArgument;
	return EvaluateSyncGroup(EvaluationSyncGroup{leader.groupStableId, leader.stateId}, groupMarkers, states, output);
}
} // namespace TerraEngine::Animation
