// AnimationDivergenceTests.cpp - Canonical earliest-divergence and bound contracts.

#include "TestHarness.h"

#include "Animation/Determinism/AnimationDivergence.h"
#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <array>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid Guid(std::uint8_t seed)
		{
			AnimationGuid value;
			value.value[0] = seed;
			return value;
		}

		AnimationRollbackSnapshot Snapshot(
			AnimatorInstanceHandle animator,
			AnimationFixedTick tick,
			std::uint64_t parameterValue)
		{
			AnimationRollbackSnapshot snapshot;
			snapshot.animator = animator;
			snapshot.tick = tick;
			snapshot.publicationGeneration = 1U;
			snapshot.fingerprint = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
			snapshot.generations = {11U, 12U, 13U, 14U, 15U, 16U, 17U};
		AnimationParameterRollbackState parameter;
		parameter.parameterId = 22U;
		parameter.type = AnimationRollbackValueType::Integer;
		parameter.valueWords[0] = static_cast<std::uint32_t>(parameterValue);
		snapshot.parameters.push_back(parameter);
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot) != AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}

		AnimationRollbackSnapshot EventSnapshot(
			AnimationFixedTick tick,
			std::uint64_t sequence)
		{
			AnimationRollbackSnapshot snapshot = Snapshot({5U, 1U}, tick, 1U);
		AnimationEventCursorRollbackState event;
		event.streamId = 50U;
		event.stateInstanceId = 51U;
		event.clipAssetId = Guid(1U);
		event.eventId = 52U;
		event.sourceTick = tick;
		event.deliveredSequence = sequence;
		event.deliveredThroughTick = tick;
		event.delivered = true;
		snapshot.events = {event};
		snapshot.eventWatermark = {50U, sequence, tick};
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot)
			!= AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}

		AnimationRollbackSnapshot RootSnapshot(
			AnimationFixedTick tick,
			std::uint32_t translationBits)
		{
			AnimationRollbackSnapshot snapshot = Snapshot({5U, 1U}, tick, 1U);
		AnimationRootMotionRollbackState root;
		root.accumulationId = 60U;
		root.sourceBeginTick = tick - 1;
		root.sourceEndTick = tick;
		root.localTranslation.words[0] = translationBits;
		root.generation = 2U;
		root.correlationId = 61U;
		root.appliedThroughTick = tick - 1;
		snapshot.rootMotionStates = {root};
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot)
			!= AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}

		AnimationRollbackSnapshot MotionSnapshot(
			AnimationFixedTick tick,
			std::uint64_t candidate)
		{
			AnimationRollbackSnapshot snapshot = Snapshot({5U, 1U}, tick, 1U);
		AnimationMotionMatchRollbackState motion;
		motion.stableNodeId = 70U;
		motion.databaseFingerprint = 71U;
		motion.databaseGeneration = 2U;
		motion.querySequence = 72U;
		motion.candidateId = candidate;
		motion.sourceClipFingerprint = 73U;
		motion.sampleTick = tick - 1;
		motion.totalScoreBits = EncodeAnimationNetworkFloat(1.0F);
		motion.selectionHash = 74U;
		snapshot.motionMatchStates = {motion};
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot)
			!= AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}
	}

	void RegisterAnimationDivergenceTests(TestRegistry& registry)
	{
		registry.Add("Animation.Divergence.CanonicalizesStreamsAndFindsEarliestGraphElement", [](TestContext& context) {
			const AnimationRollbackSnapshot expectedEarly = Snapshot({2U, 1U}, 3, 10U);
			const AnimationRollbackSnapshot actualEarly = Snapshot({2U, 1U}, 3, 11U);
			const AnimationRollbackSnapshot sameLate = Snapshot({1U, 1U}, 8, 20U);
			const std::array expected{sameLate, expectedEarly};
			const std::array actual{actualEarly, sameLate};
			AnimationDivergenceAnalyzer analyzer;
			AnimationDivergenceResult result;
			const AnimationStatus status = analyzer.Analyze(expected, actual, result);
			context.Expect(status == AnimationStatus::Ok && result.point.found
				&& result.point.animator == expectedEarly.animator
				&& result.point.tick == 3
				&& result.point.field == AnimationDivergenceField::GraphElement
				&& result.point.category == AnimationDeterministicStateKind::Parameter
				&& result.point.stableId == 22U,
				"Input ordering must not affect earliest tick/animator/graph-element localization.");
		});

		registry.Add("Animation.Divergence.ClassifiesAssetGenerationEventRootAndMotion", [](TestContext& context) {
			AnimationDivergenceAnalyzer analyzer;
			AnimationDivergenceResult result;

			const AnimationRollbackSnapshot expectedAsset = Snapshot({5U, 1U}, 4, 1U);
			AnimationRollbackSnapshot actualAsset = expectedAsset;
			actualAsset.fingerprint.programAsset += 1U;
			(void)AnimationRollbackSnapshotCodec{}.Capture(actualAsset, actualAsset);
			context.Expect(analyzer.Analyze({&expectedAsset, 1U}, {&actualAsset, 1U}, result)
				== AnimationStatus::Ok
				&& result.point.field == AnimationDivergenceField::AssetFingerprint,
				"A program/content mismatch must be distinguished from a graph-value mismatch.");

			const AnimationRollbackSnapshot expectedEvent = EventSnapshot(5, 4U);
			const AnimationRollbackSnapshot actualEvent = EventSnapshot(5, 5U);
			(void)analyzer.Analyze({&expectedEvent, 1U}, {&actualEvent, 1U}, result);
			context.Expect(result.point.field == AnimationDivergenceField::Event
				&& result.point.category == AnimationDeterministicStateKind::Event
				&& result.point.stableId == 52U,
				"Event cursor divergence must identify the exact event identity.");

			const AnimationRollbackSnapshot expectedRoot = RootSnapshot(
				6, EncodeAnimationNetworkFloat(1.0F));
			const AnimationRollbackSnapshot actualRoot = RootSnapshot(
				6, EncodeAnimationNetworkFloat(2.0F));
			(void)analyzer.Analyze({&expectedRoot, 1U}, {&actualRoot, 1U}, result);
			context.Expect(result.point.field == AnimationDivergenceField::RootMotion
				&& result.point.stableId == 60U,
				"Root-motion divergence must be localized separately from generic pose output.");

			const AnimationRollbackSnapshot expectedMotion = MotionSnapshot(7, 80U);
			const AnimationRollbackSnapshot actualMotion = MotionSnapshot(7, 81U);
			(void)analyzer.Analyze({&expectedMotion, 1U}, {&actualMotion, 1U}, result);
			context.Expect(result.point.field == AnimationDivergenceField::MotionMatch
				&& result.point.stableId == 70U,
				"Motion selection divergence must report the stable motion graph node.");
		});

		registry.Add("Animation.Divergence.AssetGenerationAndPoseChecksumAreExplicit", [](TestContext& context) {
			const AnimationRollbackSnapshot expected = Snapshot({5U, 1U}, 9, 1U);
			AnimationRollbackSnapshot generation = expected;
			generation.generations.assetGeneration += 1U;
			(void)AnimationRollbackSnapshotCodec{}.Capture(generation, generation);
			AnimationDivergenceAnalyzer analyzer;
			AnimationDivergenceResult result;
			(void)analyzer.Analyze({&expected, 1U}, {&generation, 1U}, result);
			context.Expect(result.point.field == AnimationDivergenceField::AssetGeneration
				&& result.point.expectedGeneration == expected.generations.assetGeneration
				&& result.point.actualGeneration == generation.generations.assetGeneration,
				"Generation mismatch diagnostics must expose both expected and actual identities.");

			AnimationRollbackSnapshot pose = expected;
			pose.poseChecksum ^= 1U;
			(void)analyzer.Analyze({&expected, 1U}, {&pose, 1U}, result);
			context.Expect(result.point.field == AnimationDivergenceField::PoseChecksum,
				"A pose checksum mismatch with equal authoritative state must remain a distinct category.");
		});

		registry.Add("Animation.Divergence.RetentionBoundAndMissingSnapshotAreAtomic", [](TestContext& context) {
			const std::array expected{
				Snapshot({1U, 1U}, 1, 1U),
				Snapshot({1U, 1U}, 2, 2U),
				Snapshot({1U, 1U}, 3, 3U)};
			AnimationDivergenceAnalyzer bounded({2U});
			AnimationDivergenceResult output;
			output.status = AnimationStatus::Ok;
			output.point.found = true;
			output.point.tick = 99;
			const AnimationDivergenceResult lastGood = output;
			context.Expect(bounded.Analyze(expected, expected, output)
				== AnimationStatus::HardLimitExceeded && output == lastGood,
				"Diagnostic input beyond the configured retention bound must preserve last-good output.");

			AnimationDivergenceAnalyzer analyzer;
			const std::span<const AnimationRollbackSnapshot> missing(expected.data(), 2U);
			AnimationDivergenceResult missingResult;
			context.Expect(analyzer.Analyze(expected, missing, missingResult)
				== AnimationStatus::Ok
				&& missingResult.point.field == AnimationDivergenceField::MissingSnapshot
				&& missingResult.point.tick == 3,
				"Unequal streams must identify the earliest missing animator/tick snapshot.");
		});
	}
}
