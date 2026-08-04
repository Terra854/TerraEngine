// AnimationStateMachineTests.cpp - Deterministic graph state-machine runtime contracts.

#include "TestHarness.h"

#include "Animation/Graph/StateMachineEvaluator.h"
#include "Animation/Runtime/AnimationGraphInstance.h"

#include <algorithm>
#include <atomic>
#include <thread>

namespace TerraEngine::Tests {
namespace {
using namespace TerraEngine::Animation;
AnimationGuid Guid(std::uint8_t seed) {
	AnimationGuid value;
	for (std::size_t i = 0; i < value.value.size(); ++i)
		value.value[i] = static_cast<std::uint8_t>(seed + i);
	return value;
}
EvaluationProgram MakeProgram() {
	EvaluationProgram program;
	program.controllerAssetId = Guid(1U);
	program.compatibilityFingerprint = 7U;
	program.scratchPlan.byteCount = 0U;
	program.states = {{10U, 0U, true, false, false}, {20U, 0U, false, false, false}, {30U, 0U, false, false, false}};
	program.transitions = {{10U, 20U, 1U, 0, 0, 0.5F, 5U, 20U, false, EvaluationTransitionInterruptionSource::Current,
							EvaluationTransitionInterruptionPolicy::Reject, EvaluationTriggerConsumption::SingleConsumer},
						   {10U, 30U, 1U, 0, 0, 0.0F, 5U, 30U, false, EvaluationTransitionInterruptionSource::Current,
							EvaluationTransitionInterruptionPolicy::Reject, EvaluationTriggerConsumption::MultiConsumer}};
	program.stableHash = ComputeEvaluationProgramStableHash(program);
	return program;
}

void Rehash(EvaluationProgram &program) {
	program.stableHash = ComputeEvaluationProgramStableHash(program);
}

EvaluationProgram MakeNestedProgram() {
	EvaluationProgram program;
	program.controllerAssetId = Guid(3U);
	program.compatibilityFingerprint = 9U;
	program.states = {
		{10U, 0U, true, false, true, 100U, EvaluationOpcode::CallSubgraph},
		{11U, 10U, true, true, false, 101U, EvaluationOpcode::CallSubgraph},
		{20U, 0U, false, false, false, 102U, EvaluationOpcode::ProceduralPose},
		{30U, 0U, false, false, false, 103U, EvaluationOpcode::ProceduralPose}};
	program.transitions = {
		{10U, 20U, 0U, 0, 1, 0.0F, 2U, 40U, true},
		{11U, 30U, 0U, 0, 1, 0.0F, 2U, 30U, false}};
	Rehash(program);
	return program;
}
} // namespace
void RegisterAnimationStateMachineTests(TestRegistry &registry) {
	registry.Add("Animation.StateMachine.OrdersFixedTickTransitionsAndTriggerEdges", [](TestContext &context) {
		EvaluationProgram program = MakeProgram();
		StateMachineSnapshot prior{};
		prior.activeStateId = 10U;
		StateMachineSnapshot result;
		const StateMachineTriggerEdge edge{1U, 1U, true};
		const AnimationStatus status = StepStateMachine(program, prior, {4, {&edge, 1U}}, result);
		context.Expect(
			status == AnimationStatus::Ok && result.activeStateId == 20U && result.stateEnteredTick == 4 &&
				result.transitionHistory.back().transitionStableId == 20U && result.retainedTriggerEdges.empty(),
			"Higher priority then state-specific and stable transition identity select one fixed-tick trigger edge deterministically.");
	});
	registry.Add("Animation.StateMachine.TrustedInPlaceStepMatchesTransactionalResultAndReusesHistory", [](TestContext &context) {
		EvaluationProgram program = MakeProgram();
		StateMachineSnapshot prior{};
		prior.activeStateId = 10U;
		prior.transitionHistory.reserve(8U);
		StateMachineSnapshot expected;
		const StateMachineTriggerEdge edge{1U, 1U, true};
		const AnimationStatus expectedStatus = StepStateMachineTrusted(
			program, prior, {4, {&edge, 1U}}, expected);
		StateMachineSnapshot candidate = prior;
		candidate.transitionHistory.reserve(8U);
		const std::size_t retainedCapacity = candidate.transitionHistory.capacity();
		const AnimationStatus inPlaceStatus = StepStateMachineTrustedInPlace(
			program, candidate, {4, {&edge, 1U}});
		context.Expect(
			expectedStatus == AnimationStatus::Ok
				&& inPlaceStatus == AnimationStatus::Ok
				&& candidate == expected
				&& candidate.transitionHistory.capacity() == retainedCapacity,
			"The worker-owned in-place path must produce the exact transactional state while retaining preallocated history storage.");
	});
	registry.Add("Animation.StateMachine.PublishesImmutableSnapshotsAtomically", [](TestContext &context) {
		AnimationGraphInstance instance;
		EvaluationProgram program = MakeProgram();
		const AnimationStatus published = instance.Publish(program, 1U);
		const StateMachineSnapshot before = instance.Snapshot();
		EvaluationProgram stale = program;
		stale.stableHash = 0U;
		const AnimationStatus rejected = instance.Publish(stale, 2U);
		const AnimationStatus initialized = instance.Step({0, {}});
		const StateMachineTriggerEdge edge{1U, 1U, true};
		const AnimationStatus stepped = instance.Step({4, {&edge, 1U}});
		const StateMachineSnapshot after = instance.Snapshot();
		context.Expect(published == AnimationStatus::Ok && rejected == AnimationStatus::StaleHandle && instance.Snapshot() == after &&
						   initialized == AnimationStatus::Ok && stepped == AnimationStatus::Ok && after.activeStateId == 20U &&
						   before.activeStateId == 0U,
					   "Generation/hash rejection leaves the last-good copied program and inspection snapshot publishable without "
					   "live-storage mutation.");
	});
	registry.Add("Animation.StateMachine.ResolvesNestedEntryAndSubgraphState", [](TestContext &context) {
		EvaluationProgram program = MakeProgram();
		program.states = {{10U, 0U, true, false, false, 100U, EvaluationOpcode::CallSubgraph},
			{11U, 10U, true, false, false, 101U, EvaluationOpcode::CallSubgraph},
			{20U, 0U, false, false, false, 102U, EvaluationOpcode::ProceduralPose}};
		program.transitions = {{11U, 20U, 0U, 0, 2, 0.0F, 1U, 1U, false,
			EvaluationTransitionInterruptionSource::Current, EvaluationTransitionInterruptionPolicy::Reject,
			EvaluationTriggerConsumption::SingleConsumer}};
		Rehash(program);
		StateMachineSnapshot first;
		StateMachineSnapshot second;
		const AnimationStatus initialized = StepStateMachine(program, {}, {0, {}}, first);
		const AnimationStatus transitioned = StepStateMachine(program, first, {2, {}}, second);
		context.Expect(initialized == AnimationStatus::Ok && first.activeStateId == 11U && transitioned == AnimationStatus::Ok && second.activeStateId == 20U,
			"Nested default entry resolves to the active leaf and CallSubgraph states use the same deterministic transition tables.");
	});
	registry.Add("Animation.StateMachine.NestedExitAndScopedAnyOrderingIgnoreInputOrder", [](TestContext &context) {
		EvaluationProgram forward = MakeNestedProgram();
		EvaluationProgram reverse = forward;
		std::reverse(reverse.transitions.begin(), reverse.transitions.end());
		Rehash(reverse);
		StateMachineSnapshot forwardInitial;
		StateMachineSnapshot reverseInitial;
		StateMachineSnapshot first;
		StateMachineSnapshot second;
		const AnimationStatus initializedForward = StepStateMachine(forward, {}, {0, {}}, forwardInitial);
		const AnimationStatus initializedReverse = StepStateMachine(reverse, {}, {0, {}}, reverseInitial);
		const AnimationStatus forwardStatus = StepStateMachine(forward, forwardInitial, {1, {}}, first);
		const AnimationStatus reverseStatus = StepStateMachine(reverse, reverseInitial, {1, {}}, second);
		context.Expect(initializedForward == AnimationStatus::Ok && initializedReverse == AnimationStatus::Ok &&
				forwardStatus == AnimationStatus::Ok && reverseStatus == AnimationStatus::Ok && first.activeStateId == 30U &&
				second.activeStateId == 30U && first.transitionHistory.back().transitionStableId == 30U &&
				second.transitionHistory.back().transitionStableId == 30U,
			"Nested entry resolves the CallSubgraph leaf, an ancestor exit bubbles to its scope, and state-specific ties beat scoped any-state regardless of vector order.");
	});
	registry.Add("Animation.StateMachine.ScopedAnyStateUsesItsParentScopeOnly", [](TestContext &context) {
		EvaluationProgram program;
		program.controllerAssetId = Guid(4U);
		program.compatibilityFingerprint = 10U;
		program.states = {{10U, 0U, true, false, false}, {11U, 10U, true, false, false},
			{12U, 10U, false, false, true}, {13U, 10U, false, false, false},
			{20U, 0U, false, false, false}, {30U, 0U, false, false, false}};
		program.transitions = {{12U, 30U, 0U, 0, 0, 0.0F, 1U, 50U, true}};
		Rehash(program);
		StateMachineSnapshot inside;
		inside.activeStateId = 13U;
		StateMachineSnapshot outside;
		outside.activeStateId = 20U;
		StateMachineSnapshot insideResult;
		StateMachineSnapshot outsideResult;
		const AnimationStatus insideStatus = StepStateMachine(program, inside, {1, {}}, insideResult);
		const AnimationStatus outsideStatus = StepStateMachine(program, outside, {1, {}}, outsideResult);
		context.Expect(insideStatus == AnimationStatus::Ok && insideResult.activeStateId == 30U && outsideStatus == AnimationStatus::Ok &&
				outsideResult.activeStateId == 20U,
			"An any-state pseudo-state applies to siblings inside its parent state-machine scope, never to an unrelated root scope.");
	});
	registry.Add("Animation.StateMachine.ExitDestinationBubblesToAncestorContinuation", [](TestContext &context) {
		EvaluationProgram program;
		program.controllerAssetId = Guid(5U);
		program.compatibilityFingerprint = 11U;
		program.states = {{10U, 0U, true, false, false}, {11U, 10U, true, false, false},
			{12U, 10U, false, true, false}, {20U, 0U, false, false, false}};
		program.transitions = {{11U, 12U, 0U, 0, 1, 0.0F, 1U, 60U, false}};
		Rehash(program);
		StateMachineSnapshot initialized;
		StateMachineSnapshot exited;
		const AnimationStatus initializedStatus = StepStateMachine(program, {}, {0, {}}, initialized);
		const AnimationStatus exitStatus = StepStateMachine(program, initialized, {1, {}}, exited);
		context.Expect(initializedStatus == AnimationStatus::Ok && initialized.activeStateId == 11U && exitStatus == AnimationStatus::Ok &&
				exited.activeStateId == 10U && exited.transitionHistory.back().destinationStateId == 10U,
			"Targeting a nested exit node leaves that node's parent scope and resumes at the deterministic ancestor continuation without re-entering its child default.");
	});
	registry.Add("Animation.StateMachine.CrossfadesInterruptAtExactFixedTickBoundaries", [](TestContext &context) {
		EvaluationProgram program = MakeProgram();
		program.transitions = {
			{10U, 20U, 1U, 4, 0, 0.5F, 1U, 100U, false, EvaluationTransitionInterruptionSource::Current,
				EvaluationTransitionInterruptionPolicy::Replace, EvaluationTriggerConsumption::SingleConsumer},
			{10U, 30U, 2U, 3, 0, 0.0F, 2U, 101U, false, EvaluationTransitionInterruptionSource::Current,
				EvaluationTransitionInterruptionPolicy::Replace, EvaluationTriggerConsumption::SingleConsumer},
			{20U, 30U, 3U, 2, 0, 0.0F, 3U, 102U, false, EvaluationTransitionInterruptionSource::Next,
				EvaluationTransitionInterruptionPolicy::Replace, EvaluationTriggerConsumption::SingleConsumer},
			{10U, 30U, 4U, 1, 0, 0.0F, 4U, 103U, false, EvaluationTransitionInterruptionSource::Current,
				EvaluationTransitionInterruptionPolicy::Reject, EvaluationTriggerConsumption::SingleConsumer}};
		Rehash(program);
		StateMachineSnapshot initial;
		const StateMachineTriggerEdge start{1U, 1U, true};
		const AnimationStatus started = StepStateMachine(program, {}, {0, {&start, 1U}}, initial);
		StateMachineSnapshot currentInterrupted;
		const StateMachineTriggerEdge current{2U, 2U, true};
		const AnimationStatus currentStatus = StepStateMachine(program, initial, {1, {&current, 1U}}, currentInterrupted);
		StateMachineSnapshot nextBase;
		const AnimationStatus restarted = StepStateMachine(program, {}, {0, {&start, 1U}}, nextBase);
		StateMachineSnapshot nextInterrupted;
		const StateMachineTriggerEdge next{3U, 3U, true};
		const AnimationStatus nextStatus = StepStateMachine(program, nextBase, {1, {&next, 1U}}, nextInterrupted);
		StateMachineSnapshot nextContinued;
		const AnimationStatus nextContinuedStatus =
			StepStateMachine(program, nextInterrupted, {2, {}}, nextContinued);
		StateMachineSnapshot unchanged = initial;
		StateMachineSnapshot rejectedOutput = unchanged;
		const StateMachineTriggerEdge rejected{4U, 4U, true};
		const AnimationStatus rejectedStatus = StepStateMachine(program, initial, {1, {&rejected, 1U}}, rejectedOutput);
		EvaluationProgram resetOnFailure = program;
		resetOnFailure.transitions.back().resetTriggerOnFailure = true;
		Rehash(resetOnFailure);
		StateMachineSnapshot resetFailureOutput;
		const AnimationStatus resetFailureStatus = StepStateMachine(resetOnFailure, initial, {1, {&rejected, 1U}}, resetFailureOutput);
		StateMachineSnapshot completed;
		const AnimationStatus completedStatus = StepStateMachine(program, initial, {4, {}}, completed);
		context.Expect(started == AnimationStatus::Ok && currentStatus == AnimationStatus::Ok &&
				currentInterrupted.activeTransitionStableId == 101U && currentInterrupted.interruptedTransitionStableId == 100U &&
				restarted == AnimationStatus::Ok && nextStatus == AnimationStatus::Ok && nextInterrupted.activeTransitionStableId == 102U &&
				nextContinuedStatus == AnimationStatus::Ok && nextContinued.activeTransitionStableId == 102U &&
				nextContinued.activeStateId == 20U && nextContinued.transitionSourceStateId == 20U &&
				rejectedStatus == AnimationStatus::Rejected && rejectedOutput == unchanged && resetFailureStatus == AnimationStatus::Ok &&
				resetFailureOutput.activeTransitionStableId == 100U && resetFailureOutput.retainedTriggerEdges.empty() && completedStatus == AnimationStatus::Ok &&
				completed.activeTransitionStableId == 0U && completed.activeStateId == 20U && completed.stateEnteredTick == 0,
			"Crossfades persist through their duration, complete exactly on the boundary, and apply Current/Next Replace or Reject interruption atomically.");
	});
	registry.Add("Animation.StateMachine.TriggerValidationResetAndRetentionStayBounded", [](TestContext &context) {
		EvaluationProgram program = MakeProgram();
		program.transitions = {{10U, 10U, 1U, 0, 0, 0.0F, 1U, 200U, false,
			EvaluationTransitionInterruptionSource::Current, EvaluationTransitionInterruptionPolicy::Replace,
			EvaluationTriggerConsumption::MultiConsumer, false, false}};
		Rehash(program);
		StateMachineSnapshot first;
		const StateMachineTriggerEdge edge{1U, 1U, true};
		const AnimationStatus retained = StepStateMachine(program, {}, {0, {&edge, 1U}}, first);
		EvaluationProgram singleConsumer = program;
		singleConsumer.transitions.front().triggerConsumption = EvaluationTriggerConsumption::SingleConsumer;
		Rehash(singleConsumer);
		const StateMachineTriggerEdge singleEdges[] = {{1U, 100U, true}, {1U, 101U, true}};
		StateMachineSnapshot singleResult;
		const AnimationStatus singleStatus = StepStateMachine(singleConsumer, {}, {0, {singleEdges, 2U}}, singleResult);
		const std::uint64_t reset = 1U;
		StateMachineSnapshot resetResult;
		const AnimationStatus resetStatus = StepStateMachine(program, first, {1, {}, {&reset, 1U}}, resetResult);
		StateMachineSnapshot untouched = first;
		const StateMachineTriggerEdge duplicate[]{edge, edge};
		const AnimationStatus duplicateStatus = StepStateMachine(program, first, {2, {duplicate, 2U}}, untouched);
		const StateMachineTriggerEdge zero{1U, 0U, true};
		const AnimationStatus zeroStatus = StepStateMachine(program, first, {2, {&zero, 1U}}, untouched);
		const AnimationStatus monotonicStatus = StepStateMachine(program, first, {-1, {}}, untouched);
		EvaluationProgram duplicateState = program;
		duplicateState.states.push_back(duplicateState.states.front());
		Rehash(duplicateState);
		const AnimationStatus duplicateStateStatus = StepStateMachine(duplicateState, first, {2, {}}, untouched);
		AnimationGraphInstance boundedInstance;
		const AnimationStatus boundedPublished = boundedInstance.Publish(program, 1U);
		AnimationStatus boundedStatus = AnimationStatus::Ok;
		for (std::uint64_t tick = 0U; tick < GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum; ++tick) {
			const StateMachineTriggerEdge many{1U, tick + 10U, true};
			boundedStatus = boundedInstance.Step({static_cast<AnimationFixedTick>(tick), {&many, 1U}});
			if (boundedStatus != AnimationStatus::Ok) {
				break;
			}
		}
		const StateMachineSnapshot bounded = boundedInstance.Snapshot();
		const StateMachineTriggerEdge overflow{1U, GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum + 10U, true};
		const AnimationStatus overflowStatus = boundedInstance.Step(
			{static_cast<AnimationFixedTick>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum), {&overflow, 1U}});
		context.Expect(retained == AnimationStatus::Ok && first.retainedTriggerEdges.size() == 1U && singleStatus == AnimationStatus::Ok &&
				singleResult.retainedTriggerEdges.size() == 1U && singleResult.retainedTriggerEdges.front().sequenceId == 101U && resetStatus == AnimationStatus::Ok &&
				resetResult.retainedTriggerEdges.empty() && duplicateStatus == AnimationStatus::InvalidArgument && zeroStatus == AnimationStatus::InvalidArgument &&
				monotonicStatus == AnimationStatus::InvalidArgument && duplicateStateStatus == AnimationStatus::InvalidArgument &&
				boundedPublished == AnimationStatus::Ok && overflowStatus == AnimationStatus::HardLimitExceeded && untouched == first &&
				boundedStatus == AnimationStatus::Ok &&
				bounded.transitionHistory.size() == GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum &&
				bounded.retainedTriggerEdges.size() == GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum,
			"Stable trigger identities reject duplicate or zero input, manual reset is explicit, invalid ticks preserve output, and histories trim oldest entries deterministically.");
	});
	registry.Add("Animation.GraphInstance.AtomicInspectionGenerationAndRollback", [](TestContext &context) {
		AnimationGraphInstance instance;
		EvaluationProgram program = MakeProgram();
		const AnimationStatus published = instance.Publish(program, 2U);
		const AnimationStatus initialized = instance.Step({0, {}});
		const std::shared_ptr<const AnimationGraphInspectionSnapshot> rollback = instance.InspectionSnapshot();
		const AnimationStatus advanced = instance.Step({1, {}});
		const AnimationStatus restored = rollback != nullptr ? instance.Restore(*rollback) : AnimationStatus::InternalError;
		const StateMachineSnapshot beforeInvalidRestore = instance.Snapshot();
		AnimationGraphInspectionSnapshot incompatible = *rollback;
		incompatible.programStableHash ^= 1U;
		const AnimationStatus incompatibleRestore = instance.Restore(incompatible);
		const bool incompatibleRestorePreserved = instance.Snapshot() == beforeInvalidRestore;
		std::atomic_bool readersOk{true};
		std::atomic_bool writerDone{false};
		std::thread reader([&instance, &readersOk, &writerDone]() {
			while (!writerDone.load(std::memory_order_acquire)) {
				const auto inspection = instance.InspectionSnapshot();
				if (inspection == nullptr || inspection->programGeneration != 2U) {
					readersOk.store(false);
				}
			}
			for (std::size_t index = 0U; index < 1000U; ++index) {
				const auto inspection = instance.InspectionSnapshot();
				if (inspection == nullptr || inspection->programGeneration != 2U) {
					readersOk.store(false);
				}
			}
		});
		AnimationStatus writerStatus = AnimationStatus::Ok;
		for (AnimationFixedTick tick = 2; tick < 200; ++tick) {
			writerStatus = instance.Step({tick, {}});
			if (writerStatus != AnimationStatus::Ok) {
				break;
			}
		}
		writerDone.store(true, std::memory_order_release);
		reader.join();
		const AnimationStatus staleGeneration = instance.Publish(program, 2U);
		context.Expect(published == AnimationStatus::Ok && initialized == AnimationStatus::Ok && advanced == AnimationStatus::Ok &&
				restored == AnimationStatus::Ok && incompatibleRestore == AnimationStatus::Rejected && incompatibleRestorePreserved &&
				writerStatus == AnimationStatus::Ok && readersOk.load() && staleGeneration == AnimationStatus::StaleHandle,
			"Worker-owned mutable state publishes immutable inspection pointers, strictly increases generations, and rejects incompatible rollback without changing live state.");
	});
}
} // namespace TerraEngine::Tests
