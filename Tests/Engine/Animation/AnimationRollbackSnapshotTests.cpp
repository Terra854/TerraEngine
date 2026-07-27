// AnimationRollbackSnapshotTests.cpp - Complete snapshot, history, and identity contracts.

#include "TestHarness.h"

#include "Animation/Determinism/AnimationHistory.h"
#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <type_traits>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid Guid(std::uint8_t seed)
		{
			AnimationGuid value;
			value.value[0] = seed;
			return value;
		}

		AnimationNetworkTransformBits TransformBits(float offset)
		{
			AnimationNetworkTransformBits value;
			value.translation.words = {
				EncodeAnimationNetworkFloat(offset),
				EncodeAnimationNetworkFloat(offset + 1.0F),
				EncodeAnimationNetworkFloat(offset + 2.0F)};
			value.rotation.words = {
				EncodeAnimationNetworkFloat(0.0F),
				EncodeAnimationNetworkFloat(0.0F),
				EncodeAnimationNetworkFloat(0.0F),
				EncodeAnimationNetworkFloat(1.0F)};
			value.scale.words = {
				EncodeAnimationNetworkFloat(1.0F),
				EncodeAnimationNetworkFloat(1.0F),
				EncodeAnimationNetworkFloat(1.0F)};
			return value;
		}

		AnimationRollbackSnapshot CompleteSnapshot(AnimationFixedTick tick = 10)
		{
			AnimationRollbackSnapshot value;
			value.animator = {7U, 2U};
			value.tick = tick;
			value.publicationGeneration = 3U;
			value.frozenWorkerCount = 4U;
			value.fingerprint = {11U, 12U, 13U, 14U, 15U, 16U, 17U};
			value.generations = {21U, 22U, 23U, 24U, 25U, 26U, 27U};

			AnimationControllerRollbackState controller;
			controller.controllerInstanceId = 101U;
			controller.activeStateId = 102U;
			controller.previousStateId = 103U;
			controller.stateEnteredTick = tick - 3;
			controller.lastEvaluatedTick = tick;
			controller.syncGroupId = 104U;
			controller.syncMarkerId = 105U;
			controller.deterministicRandomState = 106U;
			value.controllers.push_back(controller);

			AnimationClipRollbackState clip;
			clip.clipInstanceId = 201U;
			clip.clipAssetId = Guid(1U);
			clip.clipGeneration = 2U;
			clip.clipContentFingerprint = 202U;
			clip.absoluteTick = tick + 5;
			clip.resolvedTick = 5;
			clip.durationTicks = 60;
			clip.loopCount = 2;
			clip.speedBits = EncodeAnimationNetworkFloat(1.25F);
			clip.runState = AnimationRollbackPlaybackState::Playing;
			clip.direction = AnimationRollbackPlaybackDirection::Forward;
			clip.looping = true;
			value.clips.push_back(clip);

			AnimationTransitionRollbackState transition;
			transition.transitionStableId = 301U;
			transition.sourceStateId = 302U;
			transition.destinationStateId = 303U;
			transition.startTick = tick - 2;
			transition.durationTicks = 8;
			transition.progressBits = EncodeAnimationNetworkFloat(0.25F);
			transition.destinationOffsetBits = EncodeAnimationNetworkFloat(0.5F);
			transition.interruptedTransitionStableId = 304U;
			transition.interruptedProgressBits = EncodeAnimationNetworkFloat(0.75F);
			value.transitions.push_back(transition);

			AnimationLayerRollbackState layer;
			layer.layerStableId = 401U;
			layer.activeStateId = 402U;
			layer.stateTick = tick - 1;
			layer.weightBits = EncodeAnimationNetworkFloat(0.8F);
			layer.additive = true;
			value.layers.push_back(layer);

			// Insert parameters out of order. Capture must canonicalize them before
			// checksumming so producer scheduling cannot affect serialized identity.
			AnimationParameterRollbackState highParameter;
			highParameter.parameterId = 502U;
			highParameter.type = AnimationRollbackValueType::Float;
			highParameter.valueWords[0] = EncodeAnimationNetworkFloat(3.5F);
			value.parameters.push_back(highParameter);
			AnimationParameterRollbackState lowParameter;
			lowParameter.parameterId = 501U;
			lowParameter.type = AnimationRollbackValueType::Transform;
			const AnimationNetworkTransformBits parameterTransform = TransformBits(2.0F);
			lowParameter.valueWords = parameterTransform.words();
			value.parameters.push_back(lowParameter);

			AnimationTriggerRollbackState trigger;
			trigger.parameterId = 601U;
			trigger.sequenceId = 602U;
			trigger.raised = true;
			trigger.consumed = false;
			trigger.resetPending = true;
			value.triggers.push_back(trigger);

			AnimationEventCursorRollbackState event;
			event.streamId = 701U;
			event.stateInstanceId = 702U;
			event.clipAssetId = Guid(2U);
			event.eventId = 703U;
			event.sourceTick = tick - 1;
			event.loopCount = 4;
			event.deliveredSequence = 704U;
			event.deliveredThroughTick = tick;
			event.delivered = true;
			value.events.push_back(event);

			AnimationRootMotionRollbackState root;
			root.accumulationId = 801U;
			root.sourceBeginTick = tick - 1;
			root.sourceEndTick = tick;
			root.localTranslation.words[0] = EncodeAnimationNetworkFloat(0.25F);
			root.worldTranslation.words[2] = EncodeAnimationNetworkFloat(0.5F);
			root.localYawBits = EncodeAnimationNetworkFloat(0.1F);
			root.worldYawBits = EncodeAnimationNetworkFloat(0.2F);
			root.accumulatedCycles = 3;
			root.sourceStateId = 802U;
			root.layerIndex = 1U;
			root.generation = 2U;
			root.correlationId = 803U;
			root.appliedThroughTick = tick - 1;
			value.rootMotionStates.push_back(root);

			AnimationIkRollbackState ik;
			ik.stableNodeId = 901U;
			ik.goalId = 902U;
			ik.targetStableId = 903U;
			ik.evaluatedTick = tick;
			ik.solverState = AnimationRollbackIkState::Converged;
			ik.pelvisOffsetBits = EncodeAnimationNetworkFloat(0.03F);
			ik.weightBits = EncodeAnimationNetworkFloat(0.9F);
			ik.errorBits = EncodeAnimationNetworkFloat(0.001F);
			ik.plantedContactId = 904U;
			value.ikStates.push_back(ik);

			AnimationRetargetRollbackState retarget;
			retarget.stableNodeId = 1001U;
			retarget.profileFingerprint = 1002U;
			retarget.profileGeneration = 2U;
			retarget.sourceSkeletonGeneration = 3U;
			retarget.targetSkeletonGeneration = 4U;
			retarget.sourcePoseGeneration = 5U;
			retarget.targetPoseGeneration = 6U;
			retarget.sourceTick = tick;
			retarget.normalizedTimeBits = EncodeAnimationNetworkFloat(0.4F);
			retarget.deterministicHash = 1003U;
			retarget.reducedFidelity = true;
			value.retargetStates.push_back(retarget);

			AnimationMotionMatchRollbackState motionMatch;
			motionMatch.stableNodeId = 1101U;
			motionMatch.databaseFingerprint = 1102U;
			motionMatch.databaseGeneration = 2U;
			motionMatch.querySequence = 1103U;
			motionMatch.candidateId = 1104U;
			motionMatch.sourceClipFingerprint = 1105U;
			motionMatch.sampleTick = tick - 5;
			motionMatch.totalScoreBits = EncodeAnimationNetworkFloat(1.5F);
			motionMatch.selectionHash = 1106U;
			motionMatch.usedFallback = false;
			value.motionMatchStates.push_back(motionMatch);

			AnimationMorphRollbackState morph;
			morph.targetStableId = 1201U;
			morph.sourceStableId = 1202U;
			morph.weightBits = EncodeAnimationNetworkFloat(0.6F);
			morph.generation = 2U;
			value.morphStates.push_back(morph);

			AnimationRagdollRollbackState ragdoll;
			ragdoll.ragdollInstanceId = 1301U;
			ragdoll.bodyId = 1302U;
			ragdoll.stableJointId = 1303U;
			ragdoll.lifecycle = AnimationRollbackRagdollState::Active;
			ragdoll.worldGeneration = 2U;
			ragdoll.profileGeneration = 3U;
			ragdoll.skeletonGeneration = 4U;
			ragdoll.physicsPublicationGeneration = 5U;
			ragdoll.bodyGeneration = 6U;
			ragdoll.fixedStep = tick;
			ragdoll.modelTransform = TransformBits(4.0F);
			ragdoll.linearVelocity.words[0] = EncodeAnimationNetworkFloat(1.0F);
			ragdoll.angularVelocity.words[1] = EncodeAnimationNetworkFloat(2.0F);
			ragdoll.blendWeightBits = EncodeAnimationNetworkFloat(0.7F);
			ragdoll.eventSequence = 1304U;
			ragdoll.sleeping = false;
			value.ragdollStates.push_back(ragdoll);

			// The generic extension remains available to T262+ for future opcodes,
			// but it is no longer the only storage for authoritative domains.
			value.state = {{AnimationDeterministicStateKind::Controller, 0U, 0U, 1401U, {1U, 2U, 3U, 4U}}};
			value.eventWatermark = {701U, 704U, tick};
			value.rootMotion = {tick, {1U, 2U, 3U, 4U}, 803U, true, true};

			AnimationRollbackSnapshot captured;
			if(AnimationRollbackSnapshotCodec{}.Capture(value, captured) != AnimationStatus::Ok) {
				return {};
			}
			return captured;
		}

		AnimationRollbackRestoreRequirements Requirements(const AnimationRollbackSnapshot& snapshot)
		{
			AnimationRollbackRestoreRequirements requirements;
			requirements.animator = snapshot.animator;
			requirements.fingerprint = snapshot.fingerprint;
			requirements.publicationGeneration = snapshot.publicationGeneration;
			requirements.generations = snapshot.generations;
			return requirements;
		}

		static_assert(AnimationTransportValueRecord<AnimationControllerRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationClipRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationTransitionRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationLayerRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationParameterRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationTriggerRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationEventCursorRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationRootMotionRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationIkRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationRetargetRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationMotionMatchRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationMorphRollbackState>);
		static_assert(AnimationTransportValueRecord<AnimationRagdollRollbackState>);
	}

	void RegisterAnimationRollbackSnapshotTests(TestRegistry& registry)
	{
		registry.Add("Animation.RollbackSnapshot.CompleteModeIndependentRoundTrip", [](TestContext& context) {
			const AnimationRollbackSnapshot captured = CompleteSnapshot();
			context.Expect(ValidateRollbackSnapshot(captured) == AnimationStatus::Ok
				&& captured.stateChecksum != 0U && captured.poseChecksum != 0U,
				"Capture must produce one valid checksummed value snapshot for every authoritative domain.");
			context.Expect(captured.controllers.size() == 1U && captured.clips.size() == 1U
				&& captured.transitions.size() == 1U && captured.layers.size() == 1U
				&& captured.parameters.size() == 2U && captured.triggers.size() == 1U
				&& captured.events.size() == 1U && captured.rootMotionStates.size() == 1U
				&& captured.ikStates.size() == 1U && captured.retargetStates.size() == 1U
				&& captured.motionMatchStates.size() == 1U && captured.morphStates.size() == 1U
				&& captured.ragdollStates.size() == 1U,
				"The rollback model must retain controller, clip, transition, layer, parameter, trigger, event, root, IK, retarget, MM, morph, and ragdoll state.");
			context.Expect(captured.parameters[0].parameterId == 501U
				&& captured.parameters[1].parameterId == 502U,
				"Capture must canonicalize variable records before hashing them.");
			AnimationRollbackSnapshot anotherWorkerLane = captured;
			anotherWorkerLane.frozenWorkerCount = 31U;
			context.Expect(AnimationRollbackSnapshotCodec{}.Capture(
				anotherWorkerLane, anotherWorkerLane) == AnimationStatus::Ok
				&& anotherWorkerLane.stateChecksum == captured.stateChecksum
				&& anotherWorkerLane.poseChecksum == captured.poseChecksum,
				"Captured authority hashes must remain independent of scheduler worker count.");

			AnimationRollbackSnapshot restored;
			const AnimationStatus status = AnimationRollbackSnapshotCodec{}.Restore(
				captured, Requirements(captured), restored);
			context.Expect(status == AnimationStatus::Ok && restored == captured,
				"Restore must copy the complete mode-independent snapshot without losing a domain.");
		});

		registry.Add("Animation.RollbackSnapshot.BoundsRejectWithoutReplacingLastGood", [](TestContext& context) {
			AnimationRollbackSnapshot oversized = CompleteSnapshot();
			oversized.parameters.resize(kAnimationRollbackMaximumParameterStates + 1U);
			AnimationRollbackSnapshot lastGood = CompleteSnapshot(99);
			const AnimationRollbackSnapshot originalLastGood = lastGood;
			const AnimationStatus status = AnimationRollbackSnapshotCodec{}.Capture(oversized, lastGood);
			context.Expect(status == AnimationStatus::HardLimitExceeded && lastGood == originalLastGood,
				"A snapshot beyond a frozen domain bound must be rejected before replacing the last-good value.");
		});

		registry.Add("Animation.RollbackSnapshot.FingerprintAndGenerationRejectionIsAtomic", [](TestContext& context) {
			const AnimationRollbackSnapshot captured = CompleteSnapshot();
			AnimationRollbackSnapshot output = CompleteSnapshot(77);
			const AnimationRollbackSnapshot lastGood = output;
			AnimationRollbackRestoreRequirements fingerprintMismatch = Requirements(captured);
			fingerprintMismatch.fingerprint.aggregateAssetHash += 1U;
			context.Expect(AnimationRollbackSnapshotCodec{}.Restore(captured, fingerprintMismatch, output)
				== AnimationStatus::IncompatibleAsset && output == lastGood,
				"An incompatible asset/program fingerprint must leave the prior output unchanged.");

			AnimationRollbackRestoreRequirements generationMismatch = Requirements(captured);
			generationMismatch.generations.physicsWorldGeneration += 1U;
			context.Expect(AnimationRollbackSnapshotCodec{}.Restore(captured, generationMismatch, output)
				== AnimationStatus::StaleHandle && output == lastGood,
				"An object/component/scene/asset/program/physics/history generation mismatch must reject atomically.");

			AnimationHistory history;
			context.Expect(history.Store(captured) == AnimationStatus::Ok,
				"The valid baseline snapshot must enter history before mismatch checks.");
			const AnimationHistoryLookupResult historyFingerprintMismatch = history.LookupDetailed(
				captured.animator, captured.tick, fingerprintMismatch, output);
			context.Expect(historyFingerprintMismatch.outcome
				== AnimationHistoryLookupOutcome::FingerprintMismatch && output == lastGood,
				"History fingerprint rejection must preserve the caller's last-good output.");
			const AnimationHistoryLookupResult historyGenerationMismatch = history.LookupDetailed(
				captured.animator, captured.tick, generationMismatch, output);
			context.Expect(historyGenerationMismatch.outcome
				== AnimationHistoryLookupOutcome::GenerationMismatch && output == lastGood,
				"History generation rejection must preserve the caller's last-good output.");

			AnimationRollbackSnapshot corrupted = captured;
			corrupted.morphStates[0].weightBits ^= 1U;
			context.Expect(AnimationRollbackSnapshotCodec{}.Restore(
				corrupted, Requirements(captured), output) == AnimationStatus::ContentHashMismatch
				&& output == lastGood,
				"A changed authoritative value with stale checksums must be rejected before replacing output.");
		});

		registry.Add("Animation.RollbackHistory.ExactLookupDistinguishesTooOldAndMissing", [](TestContext& context) {
			AnimationHistory history({2U, AnimationHistoryPressurePolicy::EvictOldest});
			const AnimationRollbackSnapshot first = CompleteSnapshot(1);
			const AnimationRollbackSnapshot second = CompleteSnapshot(2);
			const AnimationRollbackSnapshot third = CompleteSnapshot(3);
			context.Expect(history.Store(first) == AnimationStatus::Ok
				&& history.Store(second) == AnimationStatus::Ok
				&& history.Store(third) == AnimationStatus::Ok,
				"Evict-oldest pressure must retain new fixed ticks without exceeding the ring capacity.");

			AnimationRollbackSnapshot output = CompleteSnapshot(80);
			const AnimationRollbackSnapshot lastGood = output;
			const AnimationHistoryLookupResult tooOld = history.LookupDetailed(
				first.animator, 1, Requirements(first), output);
			context.Expect(tooOld.status == AnimationStatus::RangeOutOfBounds
				&& tooOld.outcome == AnimationHistoryLookupOutcome::TooOld
				&& tooOld.oldestRetainedTick == 2 && tooOld.newestRetainedTick == 3
				&& output == lastGood,
				"A correction before the retained window must return the explicit recoverable TooOld outcome without changing output.");

			const AnimationHistoryLookupResult missing = history.LookupDetailed(
				first.animator, 4, Requirements(first), output);
			context.Expect(missing.status == AnimationStatus::NotReady
				&& missing.outcome == AnimationHistoryLookupOutcome::TickNotRetained
				&& output == lastGood,
				"An absent tick that is not too old must remain distinguishable from an evicted correction.");

			const AnimationHistoryLookupResult found = history.LookupDetailed(
				first.animator, 2, Requirements(first), output);
			context.Expect(found.status == AnimationStatus::Ok
				&& found.outcome == AnimationHistoryLookupOutcome::Found && output.tick == 2,
				"Exact tick lookup must return the requested retained snapshot, not merely the closest value.");
		});

		registry.Add("Animation.RollbackHistory.RejectNewPressurePreservesRing", [](TestContext& context) {
			AnimationHistory history({2U, AnimationHistoryPressurePolicy::RejectNew});
			const AnimationRollbackSnapshot first = CompleteSnapshot(10);
			const AnimationRollbackSnapshot second = CompleteSnapshot(11);
			const AnimationRollbackSnapshot rejected = CompleteSnapshot(12);
			context.Expect(history.Store(first) == AnimationStatus::Ok
				&& history.Store(second) == AnimationStatus::Ok
				&& history.Store(rejected) == AnimationStatus::HardLimitExceeded,
				"Reject-new pressure must report the capacity limit instead of silently discarding a retained tick.");
			AnimationRollbackSnapshot output;
			context.Expect(history.Lookup(first.animator, 10, first.fingerprint,
				first.publicationGeneration, output) == AnimationStatus::Ok && output.tick == 10,
				"Rejecting a new snapshot must preserve the last-good contents of the bounded ring.");
		});

		registry.Add("Animation.RollbackSnapshot.TransportRecordsOwnNoRuntimeOrGpuReferences", [](TestContext& context) {
			AnimationRollbackSnapshot snapshot = CompleteSnapshot();
			const AnimationRollbackSnapshot copy = snapshot;
			snapshot.parameters[0].valueWords[0] ^= 1U;
			context.Expect(copy.parameters[0] != snapshot.parameters[0]
				&& copy.ragdollStates[0].modelTransform == snapshot.ragdollStates[0].modelTransform,
				"Rollback records must be copied fixed-width values rather than aliases to live animator, renderer, or GPU storage.");
		});
	}
}
