// AnimationEventCurveTests.cpp - event, snapshot, and root-motion semantics.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationEventStream.h"
#include "Animation/Runtime/AnimationSnapshots.h"
#include "Animation/Runtime/RootMotion.h"

#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid MakeRuntimeGuid(std::uint8_t seed) {
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationAssetHandle MakeClipHandle() {
			return {MakeRuntimeGuid(20U), 3U};
		}

		AnimatorInstanceDescriptor MakeRuntimeDescriptor() {
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = 7U;
			descriptor.componentGeneration = 1U;
			return descriptor;
		}

		AnimationClipEvent MakeEvent(std::uint64_t id, AnimationFixedTick tick) {
			AnimationClipEvent event;
			event.stableEventId = id;
			event.tick = tick;
			event.payloadType = AnimationEventPayloadType::Integer;
			event.payload = std::to_string(id);
			return event;
		}

		AnimationEventTraversalRequest MakeTraversal(
			AnimatorInstanceHandle animator,
			AnimationFixedTick previous,
			AnimationFixedTick current) {
			AnimationEventTraversalRequest request;
			request.animator = animator;
			request.stateInstanceId = 10U;
			request.sourceStateId = 50U;
			request.previousAbsoluteTick = previous;
			request.currentAbsoluteTick = current;
			request.correlationId = 99U;
			request.producerOrderBase = 100U;
			return request;
		}

		bool NearlyEqual(float expected, float actual, float tolerance = 0.0001F) {
			return std::abs(expected - actual) <= tolerance;
		}
	}

	void RegisterAnimationEventCurveTests(TestRegistry& registry) {
		registry.Add("Animation.Event.ForwardTraversalIsOrderedAndExactlyOnce", [](TestContext& context) {
			AnimationEventStream stream;
			const std::vector<AnimationClipEvent> authored{
				MakeEvent(30U, 20), MakeEvent(10U, 5), MakeEvent(20U, 20)};
			context.Expect(stream.Configure(MakeClipHandle(), 100, authored) == AnimationStatus::Ok,
				"A valid authored event stream should configure atomically.");

			AnimationEventTraversalResult result;
			const AnimatorInstanceHandle animator{2U, 4U};
			context.Expect(stream.Traverse(MakeTraversal(animator, 0, 30), result) == AnimationStatus::Ok,
				"Forward traversal should publish every crossed event.");
			context.Expect(result.events.size() == 3U
				&& result.events[0].identity.eventId == 10U
				&& result.events[1].identity.eventId == 20U
				&& result.events[2].identity.eventId == 30U,
				"Events must sort by time, stable ID, then authored order.");
			context.Expect(result.events[0].producerOrder == 100U
				&& result.events[2].producerOrder == 102U,
				"Published producer order should be stable and contiguous.");

			context.Expect(stream.Traverse(MakeTraversal(animator, 0, 30), result) == AnimationStatus::Ok
				&& result.events.empty(),
				"Repeating the same interval must not expose one-shot events twice.");
		});

		registry.Add("Animation.Event.CopiedStreamsShareAuthoredDataButKeepIndependentWatermarks", [](TestContext& context) {
			AnimationEventStream original;
			const std::vector<AnimationClipEvent> authored{
				MakeEvent(1U, 10), MakeEvent(2U, 20), MakeEvent(3U, 30)};
			context.Expect(original.Configure(MakeClipHandle(), 100, authored)
				== AnimationStatus::Ok,
				"The source event stream should configure before it enters a worker snapshot.");
			AnimationEventStream copied = original;
			const AnimatorInstanceHandle animator{12U, 3U};
			AnimationEventTraversalResult originalResult;
			AnimationEventTraversalResult copiedResult;
			context.Expect(original.Traverse(MakeTraversal(animator, 0, 40), originalResult)
					== AnimationStatus::Ok
				&& copied.Traverse(MakeTraversal(animator, 0, 40), copiedResult)
					== AnimationStatus::Ok
				&& originalResult.events.size() == authored.size()
				&& copiedResult.events.size() == authored.size(),
				"Worker copies must retain immutable authored ordering while advancing independent delivery history.");
			context.Expect(original.Traverse(MakeTraversal(animator, 0, 40), originalResult)
					== AnimationStatus::Ok && originalResult.events.empty(),
				"Advancing a copied stream must not reset or alias the source delivery watermark.");
		});

		registry.Add("Animation.Event.LoopCatchUpAndReverseUseDeterministicOccurrenceOrder", [](TestContext& context) {
			AnimationEventStream stream;
			const std::vector<AnimationClipEvent> authored{MakeEvent(1U, 25), MakeEvent(2U, 75)};
			stream.Configure(MakeClipHandle(), 100, authored);
			const AnimatorInstanceHandle animator{3U, 8U};
			AnimationEventTraversalResult result;
			stream.Traverse(MakeTraversal(animator, 0, 250), result);
			context.Expect(result.events.size() == 5U
				&& result.events[0].identity.loopCount == 0
				&& result.events[2].identity.loopCount == 1
				&& result.events[4].identity.loopCount == 2,
				"A catch-up update must include each eligible occurrence across every crossed loop.");

			stream.ResetDeliveryHistory();
			stream.Traverse(MakeTraversal(animator, 250, 0), result);
			context.Expect(result.events.size() == 5U
				&& result.events[0].identity.eventId == 1U
				&& result.events[0].identity.loopCount == 2
				&& result.events[1].identity.eventId == 2U
				&& result.events[1].identity.loopCount == 1
				&& result.events.back().direction == AnimationPlaybackDirection::Reverse,
				"Reverse traversal must deliver occurrences in descending time with stable ties.");
		});

		registry.Add("Animation.Event.SeekPoliciesSuppressReplayOrExplicitlyReemit", [](TestContext& context) {
			AnimationEventStream stream;
			stream.Configure(MakeClipHandle(), 100, std::vector<AnimationClipEvent>{MakeEvent(4U, 25)});
			const AnimatorInstanceHandle animator{4U, 9U};
			AnimationEventTraversalResult result;
			AnimationEventTraversalRequest seek = MakeTraversal(animator, 0, 80);
			seek.historicalPolicy = AnimationHistoricalEventPolicy::Suppress;
			stream.Traverse(seek, result);
			context.Expect(result.events.empty(), "A suppressing seek must never replay historical one-shots.");

			stream.Traverse(MakeTraversal(animator, 80, 130), result);
			context.Expect(result.events.size() == 1U && result.events.front().identity.loopCount == 1,
				"Playback after a seek should deliver only a newly crossed occurrence.");
			AnimationEventTraversalRequest reemit = MakeTraversal(animator, 80, 130);
			reemit.historicalPolicy = AnimationHistoricalEventPolicy::Reemit;
			stream.Traverse(reemit, result);
			context.Expect(result.events.size() == 1U && result.events.front().historical,
				"Only an explicit re-emission policy may expose a previously delivered occurrence.");
		});

		registry.Add("Animation.Event.OverflowAndRetentionRemainBoundedAndVisible", [](TestContext& context) {
			AnimationEventStream stream;
			AnimationEventStreamConfig config;
			config.maxEventsPerUpdate = 2U;
			config.retainedEventCapacity = 2U;
			const std::vector<AnimationClipEvent> authored{
				MakeEvent(1U, 10), MakeEvent(2U, 20), MakeEvent(3U, 30)};
			stream.Configure(MakeClipHandle(), 100, authored, config);
			const AnimatorInstanceHandle animator{5U, 2U};
			AnimationEventTraversalResult result;
			context.Expect(stream.Traverse(MakeTraversal(animator, 0, 40), result) == AnimationStatus::BufferTooSmall
				&& result.events.size() == 2U && result.overflowed
				&& result.droppedEventLowerBound == 1U,
				"Overflow must retain a deterministic prefix and report an explicit bounded diagnostic.");

			stream.Traverse(MakeTraversal(animator, 100, 115), result);
			std::vector<AnimationEventIdentity> retained;
			stream.CopyRetainedEvents(retained);
			context.Expect(retained.size() == 2U && result.retainedEventEvictions == 1U
				&& stream.Diagnostics().retainedEventEvictions == 1U,
				"Retention must evict the oldest identity deterministically and expose the eviction.");
		});

		registry.Add("Animation.Event.ZeroDurationAndInvalidPoliciesFailClosed", [](TestContext& context) {
			AnimationEventStream stream;
			const std::vector<AnimationClipEvent> pointEvents{MakeEvent(1U, 0)};
			context.Expect(stream.Configure(MakeClipHandle(), 0, pointEvents) == AnimationStatus::Ok,
				"A zero-duration clip is a valid stable point and must not fail binding because it has no traversal interval.");

			AnimationEventTraversalResult result;
			const AnimatorInstanceHandle animator{6U, 2U};
			context.Expect(stream.Traverse(MakeTraversal(animator, 0, 1), result) == AnimationStatus::Ok
				&& result.events.empty(),
				"A point clip must never divide by zero or invent a loop event while time advances externally.");

			AnimationEventTraversalRequest invalid = MakeTraversal(animator, 0, 1);
			invalid.eventPolicy = static_cast<AnimatorEventPolicy>(255U);
			context.Expect(stream.Traverse(invalid, result) == AnimationStatus::InvalidArgument
				&& result.events.empty(),
				"Unknown event-policy values must be rejected before traversal can publish a partial batch.");
		});

		registry.Add("Animation.Snapshot.PublishesImmutablePoseMorphCurveEventAndGeneration", [](TestContext& context) {
			AnimatorInstanceStore store;
			const AnimatorInstanceHandle animator = store.Create(MakeRuntimeDescriptor());
			auto pose = std::make_shared<PoseBuffer>();
			PoseBufferLayout layout{1U, 2U, 2U, 1U, 16U};
			AnimationPoseGeneration generation{animator, 1U, {}, 3U, 25};
			context.Expect(pose->BeginWrite(layout, generation) == AnimationStatus::Ok,
				"A bounded worker pose should begin in writable scratch state.");
			auto morphs = pose->MutableMorphValues();
			morphs[0] = {40U, 0.25F};
			morphs[1] = {20U, 0.75F};
			auto curves = pose->MutableCurveValues();
			curves[0] = {8U, -1.0F};
			curves[1] = {3U, 2.0F};
			AnimationEventRecord event;
			event.identity = {animator, 1U, MakeClipHandle().assetId, 9U, 25, 0};
			event.direction = AnimationPlaybackDirection::Forward;
			pose->AppendEvent(event);
			context.Expect(pose->Seal() == AnimationStatus::Ok && pose->MutableScratch().empty(),
				"Seal must validate all output and permanently remove mutable access.");

			AnimationSnapshot snapshot;
			snapshot.animator = animator;
			snapshot.publicationGeneration = 1U;
			snapshot.tick = 25;
			snapshot.playback.normalizedTime = 0.25F;
			snapshot.playback.transitionProgress = 0.5F;
			snapshot.playback.effectiveSpeed = 1.0F;
			snapshot.playback.latestStatus = AnimationStatus::Ok;
			snapshot.pose = pose;
			AnimationSnapshotPublisher publisher;
			context.Expect(publisher.Publish(snapshot, store) == AnimationStatus::Ok,
				"A complete matching generation should publish with one immutable pointer swap.");
			const auto published = publisher.Load();
			context.Expect(published && published->pose->MorphValues()[0].stableId == 20U
				&& published->pose->CurveValues()[0].stableId == 3U
				&& published->pose->Events().size() == 1U,
				"Published morphs, curves, and events must be copied in deterministic lookup order.");
			context.Expect(publisher.Publish(snapshot, store) == AnimationStatus::StaleHandle,
				"An equal or older worker generation must never replace the current snapshot.");
		});

		registry.Add("Animation.Snapshot.RejectsNonFiniteChannelsAtomically", [](TestContext& context) {
			PoseBuffer pose;
			const AnimatorInstanceHandle animator{8U, 2U};
			pose.BeginWrite({0U, 1U, 0U, 0U, 0U}, {animator, 1U, {}, 1U, 0});
			pose.MutableMorphValues()[0] = {1U, std::numeric_limits<float>::quiet_NaN()};
			context.Expect(pose.Seal() == AnimationStatus::InvalidArgument && !pose.IsSealed(),
				"A non-finite morph/curve value must reject the entire candidate without publication.");
		});

		registry.Add("Animation.Snapshot.SharesExactVisualPayloadWithDistinctGeneration", [](TestContext& context) {
			const AnimatorInstanceHandle leader{81U, 2U};
			const AnimatorInstanceHandle follower{82U, 2U};
			PoseBuffer leaderPose;
			context.Expect(leaderPose.BeginWrite({1U, 0U, 0U, 0U, 8U},
				{leader, 5U, {}, 3U, 42}) == AnimationStatus::Ok
				&& leaderPose.Seal() == AnimationStatus::Ok,
				"A sealed visual leader should provide a checked immutable payload.");
			PoseBuffer followerPose;
			context.Expect(followerPose.ShareSealedVisualPayloadFrom(
				leaderPose, {follower, 9U, {}, 3U, 42}) == AnimationStatus::Ok
				&& followerPose.Generation().animator == follower
				&& followerPose.Generation().poseGeneration == 9U
				&& followerPose.LocalTransforms().data() == leaderPose.LocalTransforms().data()
				&& followerPose.ModelTransforms().data() == leaderPose.ModelTransforms().data()
				&& followerPose.MutableLocalTransforms().empty(),
				"Exact visual followers must alias sealed transform/palette input while retaining their own generation.");
			PoseBuffer rejectedFollower;
			context.Expect(rejectedFollower.ShareSealedVisualPayloadFrom(
				leaderPose, {{}, 1U, {}, 3U, 42}) == AnimationStatus::InvalidArgument,
				"A shared payload must reject an invalid follower identity before publication.");
		});

		registry.Add("Animation.RootMotion.ExtractsForwardReverseAndMultiLoopIntervals", [](TestContext& context) {
			AnimationRootMotionExtractionRequest request;
			request.animator = {9U, 1U};
			request.sourceBeginTick = 0;
			request.sourceEndTick = 250;
			request.begin = {{0.25F, 0.0F, 0.0F}, 0.0F, 0};
			request.end = {{0.5F, 0.0F, 0.0F}, 0.0F, 2};
			request.accumulatedCycleTranslation = {1.0F, 0.0F, 0.0F};
			request.generation = 4U;
			AnimationRootMotionDelta delta;
			context.Expect(ExtractRootMotion(request, delta) == AnimationStatus::Ok
				&& NearlyEqual(2.25F, delta.localTranslation.x)
				&& delta.accumulatedCycles == 2,
				"Forward extraction must include every crossed cycle in one bounded operation.");

			std::swap(request.begin, request.end);
			std::swap(request.sourceBeginTick, request.sourceEndTick);
			context.Expect(ExtractRootMotion(request, delta) == AnimationStatus::Ok
				&& NearlyEqual(-2.25F, delta.localTranslation.x)
				&& delta.accumulatedCycles == -2,
				"Reverse extraction must be the exact inverse interval with signed cycles.");
		});

		registry.Add("Animation.RootMotion.AccumulatesContiguousTicksWithoutLiveMutation", [](TestContext& context) {
			const AnimatorInstanceHandle animator{10U, 3U};
			AnimationRootMotionDelta first;
			first.animator = animator;
			first.sourceBeginTick = 10;
			first.sourceEndTick = 20;
			first.localTranslation = first.worldTranslation = {1.0F, 0.0F, 0.0F};
			first.generation = 5U;
			AnimationRootMotionDelta second = first;
			second.sourceBeginTick = 20;
			second.sourceEndTick = 30;
			second.localTranslation = second.worldTranslation = {2.0F, 0.0F, 0.0F};
			AnimationRootMotionAccumulator accumulator;
			context.Expect(accumulator.Accumulate(first) == AnimationStatus::Ok
				&& accumulator.Accumulate(second) == AnimationStatus::Ok
				&& NearlyEqual(3.0F, accumulator.Peek()->localTranslation.x),
				"Only contiguous copied intervals should compose in source-tick order.");
			AnimationRootMotionDelta gap = second;
			gap.sourceBeginTick = 40;
			gap.sourceEndTick = 50;
			context.Expect(accumulator.Accumulate(gap) == AnimationStatus::InvalidArgument
				&& accumulator.Peek()->sourceEndTick == 30,
				"A gap must reject atomically without partially changing the accumulated request.");
			const auto consumed = accumulator.Consume();
			context.Expect(consumed.has_value() && !accumulator.Peek().has_value(),
				"Consumption transfers one immutable request without applying scene or physics mutation.");
		});
	}
}
