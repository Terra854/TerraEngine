// AnimationReplicationTests.cpp - Typed replication, prediction, and replay contracts.

#include "TestHarness.h"

#include "Animation/Determinism/AnimationReplay.h"
#include "Animation/Determinism/AnimationReplication.h"
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

		AnimationRollbackSnapshot Snapshot(AnimationFixedTick tick)
		{
			AnimationRollbackSnapshot snapshot;
			snapshot.animator = {3U, 1U};
			snapshot.tick = tick;
			snapshot.publicationGeneration = 2U;
			snapshot.fingerprint = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
			snapshot.generations = {11U, 12U, 13U, 14U, 15U, 16U, 17U};
			snapshot.state = {{
				AnimationDeterministicStateKind::Event,
				0U,
				0U,
				1U,
				{static_cast<std::uint64_t>(tick), 0U, 0U, 0U}}};
		AnimationEventCursorRollbackState event;
		event.streamId = 2U;
		event.stateInstanceId = 3U;
		event.clipAssetId = Guid(1U);
		event.eventId = static_cast<std::uint64_t>(tick + 100);
		event.sourceTick = tick;
		event.deliveredSequence = static_cast<std::uint64_t>(tick);
		event.deliveredThroughTick = tick;
		event.delivered = true;
		snapshot.events.push_back(event);
		snapshot.eventWatermark = {2U, static_cast<std::uint64_t>(tick), tick};
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot) != AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}

		AnimationReplicationPacket CompletePacket(AnimationNetworkPacketKind kind, bool predicted)
		{
			AnimationReplicationPacket packet;
			packet.header.kind = kind;
			packet.header.sequence = 40U;
			packet.header.animator = {3U, 1U};
			packet.header.tick = 12;
			packet.header.publicationGeneration = 2U;
			packet.header.fingerprint = {1U, 2U, 3U, 4U, 5U, 6U, 7U};

			AnimationReplicatedCommandRecord command;
			command.commandId = 101U;
			command.targetStableId = 102U;
			command.correlationId = 103U;
			command.effectiveTick = 12;
			command.producerId = 4U;
			command.producerSequence = 5U;
			command.operation = AnimationReplicatedCommandKind::CrossFade;
			command.payloadWordCount = 2U;
			command.payloadWords[0] = 201U;
			command.payloadWords[1] = EncodeAnimationNetworkFloat(0.25F);
			command.predicted = predicted;
			packet.commands.push_back(command);

			AnimationReplicatedParameterRecord parameter;
			parameter.parameter.parameterId = 301U;
			parameter.parameter.type = AnimationRollbackValueType::Float;
			parameter.parameter.valueWords[0] = EncodeAnimationNetworkFloat(0.75F);
			parameter.effectiveTick = 12;
			parameter.producerId = 4U;
			parameter.producerSequence = 6U;
			parameter.predicted = predicted;
			packet.parameters.push_back(parameter);

			packet.records.push_back({{
				AnimationDeterministicStateKind::Controller,
				0U,
				0U,
				401U,
				{1U, 2U, 3U, 4U}}, predicted});

			AnimationReplicatedRootMotionRecord root;
			root.state.accumulationId = 501U;
			root.state.sourceBeginTick = 11;
			root.state.sourceEndTick = 12;
			root.state.localTranslation.words[0] = EncodeAnimationNetworkFloat(0.5F);
			root.state.generation = 2U;
			root.state.correlationId = 502U;
			root.state.appliedThroughTick = 11;
			root.acknowledged = false;
			root.predicted = predicted;
			packet.rootMotion.push_back(root);

			AnimationReplicatedMotionMatchRecord motion;
			motion.state.stableNodeId = 601U;
			motion.state.databaseFingerprint = 602U;
			motion.state.databaseGeneration = 2U;
			motion.state.querySequence = 603U;
			motion.state.candidateId = 604U;
			motion.state.sourceClipFingerprint = 605U;
			motion.state.sampleTick = 10;
			motion.state.totalScoreBits = EncodeAnimationNetworkFloat(1.0F);
			motion.state.selectionHash = 606U;
			motion.predicted = predicted;
			packet.motionMatches.push_back(motion);

			AnimationReplicatedRagdollRecord ragdoll;
			ragdoll.state.ragdollInstanceId = 701U;
			ragdoll.state.bodyId = 702U;
			ragdoll.state.stableJointId = 703U;
			ragdoll.state.lifecycle = AnimationRollbackRagdollState::Active;
			ragdoll.state.worldGeneration = 2U;
			ragdoll.state.profileGeneration = 3U;
			ragdoll.state.skeletonGeneration = 4U;
			ragdoll.state.physicsPublicationGeneration = 5U;
			ragdoll.state.bodyGeneration = 6U;
			ragdoll.state.fixedStep = 12;
			ragdoll.state.modelTransform.rotation.words[3] = EncodeAnimationNetworkFloat(1.0F);
			ragdoll.state.modelTransform.scale.words = {
				EncodeAnimationNetworkFloat(1.0F),
				EncodeAnimationNetworkFloat(1.0F),
				EncodeAnimationNetworkFloat(1.0F)};
			ragdoll.state.blendWeightBits = EncodeAnimationNetworkFloat(0.5F);
			ragdoll.state.eventSequence = 704U;
			ragdoll.predicted = predicted;
			packet.ragdolls.push_back(ragdoll);
			return packet;
		}

		AnimationReplicationAcceptance Acceptance(const AnimationReplicationPacket& packet)
		{
			AnimationReplicationAcceptance acceptance;
			acceptance.animator = packet.header.animator;
			acceptance.fingerprint = packet.header.fingerprint;
			acceptance.publicationGeneration = packet.header.publicationGeneration;
			acceptance.minimumSequence = packet.header.sequence;
			acceptance.allowPredictedInput = true;
			acceptance.allowLateJoinSnapshot = true;
			return acceptance;
		}
	}

	void RegisterAnimationReplicationTests(TestRegistry& registry)
	{
		registry.Add("Animation.Replication.TypedAuthoritativeRoundTrip", [](TestContext& context) {
			const AnimationReplicationPacket packet = CompletePacket(
				AnimationNetworkPacketKind::AuthoritativeState, false);
			AnimationReplicationCodec codec;
			std::vector<std::uint8_t> bytes;
			AnimationReplicationPacket decoded;
			const AnimationStatus encode = codec.Encode(packet, bytes);
			const AnimationStatus decode = codec.Decode(bytes, Acceptance(packet), decoded);
			context.Expect(encode == AnimationStatus::Ok && decode == AnimationStatus::Ok,
				"A valid typed authoritative packet must encode and decode successfully.");
			context.Expect(decoded.commands == packet.commands
				&& decoded.parameters == packet.parameters
				&& decoded.records == packet.records
				&& decoded.rootMotion == packet.rootMotion
				&& decoded.motionMatches == packet.motionMatches
				&& decoded.ragdolls == packet.ragdolls,
				"Command, parameter, state, root, motion-match, and ragdoll values must round-trip exactly.");
			context.Expect(decoded.header.payloadChecksum != 0U
				&& decoded.header.byteCount == bytes.size()
				&& decoded.header.fingerprint == packet.header.fingerprint,
				"The wire envelope must retain complete asset/program identity and a bounded content checksum.");
		});

		registry.Add("Animation.Replication.PredictionPolicyAndAtomicCorruptionRejection", [](TestContext& context) {
			const AnimationReplicationPacket predicted = CompletePacket(
				AnimationNetworkPacketKind::PredictedInput, true);
			AnimationReplicationCodec codec;
			std::vector<std::uint8_t> bytes;
			context.Expect(codec.Encode(predicted, bytes) == AnimationStatus::Ok,
				"A consistently predicted command/state packet must encode.");

			AnimationReplicationPacket output = CompletePacket(
				AnimationNetworkPacketKind::AuthoritativeState, false);
			const AnimationReplicationPacket lastGood = output;
			AnimationReplicationAcceptance denied = Acceptance(predicted);
			denied.allowPredictedInput = false;
			context.Expect(codec.Decode(bytes, denied, output) == AnimationStatus::Rejected
				&& output == lastGood,
				"Prediction policy rejection must preserve the caller's last-good packet.");

			std::vector<std::uint8_t> corrupted = bytes;
			corrupted.back() ^= 1U;
			context.Expect(codec.Decode(corrupted, Acceptance(predicted), output)
				== AnimationStatus::ContentHashMismatch && output == lastGood,
				"Corrupt payload bytes must be rejected by the envelope checksum atomically.");
			const std::span<const std::uint8_t> truncated(bytes.data(), bytes.size() - 1U);
			context.Expect(codec.Decode(truncated, Acceptance(predicted), output)
				== AnimationStatus::TruncatedInput && output == lastGood,
				"Truncated packets must have an explicit status and must not partially decode.");

			AnimationReplicationAcceptance wrongAsset = Acceptance(predicted);
			wrongAsset.fingerprint.programAsset += 1U;
			context.Expect(codec.Decode(bytes, wrongAsset, output)
				== AnimationStatus::IncompatibleAsset && output == lastGood,
				"Remote data with the wrong program fingerprint must not replace accepted state.");
		});

		registry.Add("Animation.Replication.MixedPredictionAndBoundsRejectLastGoodBytes", [](TestContext& context) {
			AnimationReplicationPacket mixed = CompletePacket(
				AnimationNetworkPacketKind::PredictedInput, true);
			mixed.ragdolls.front().predicted = false;
			std::vector<std::uint8_t> bytes{9U, 8U, 7U};
			const std::vector<std::uint8_t> lastGood = bytes;
			AnimationReplicationCodec codec;
			context.Expect(codec.Encode(mixed, bytes) == AnimationStatus::InvalidArgument
				&& bytes == lastGood,
				"A packet that mixes predicted and authoritative values must reject before replacing bytes.");
			mixed = CompletePacket(AnimationNetworkPacketKind::PredictedInput, true);
			mixed.records.resize(kAnimationDeterminismMaximumPacketRecords + 1U,
				mixed.records.front());
			context.Expect(codec.Encode(mixed, bytes) == AnimationStatus::HardLimitExceeded
				&& bytes == lastGood,
				"Record pressure beyond the frozen packet bound must preserve the previous encoded envelope.");
		});

		registry.Add("Animation.Replay.LateJoinAndSeekUseExplicitHistoricalEventPolicy", [](TestContext& context) {
			AnimationHistory history;
			const AnimationRollbackSnapshot tickFour = Snapshot(4);
			const AnimationRollbackSnapshot tickEight = Snapshot(8);
			context.Expect(history.Store(tickFour) == AnimationStatus::Ok
				&& history.Store(tickEight) == AnimationStatus::Ok,
				"Replay history must retain valid generation-stamped snapshots.");

			AnimationReplayRequest lateJoin;
			lateJoin.mode = AnimationReplayMode::LateJoin;
			lateJoin.animator = tickFour.animator;
			lateJoin.seekTick = 7;
			lateJoin.requirements = {
				tickFour.animator,
				tickFour.fingerprint,
				tickFour.publicationGeneration,
				tickFour.generations};
			lateJoin.eventPolicy = AnimationReplayEventPolicy::SuppressHistorical;
			const AnimationReplayResult joined = ResolveAnimationReplay(history, lateJoin);
			context.Expect(joined.status == AnimationStatus::Ok
				&& joined.resumeSnapshot.tick == 4
				&& joined.requiredCatchUpTicks == 3U
				&& joined.nextEventSequence == 5U
				&& joined.suppressHistoricalOneShotEvents
				&& joined.historicalEvents.empty(),
				"Late join must select the newest prior snapshot and continue after its event watermark.");

			AnimationReplayRequest replay = lateJoin;
			replay.mode = AnimationReplayMode::ReplaySeek;
			replay.eventPolicy = AnimationReplayEventPolicy::ReplayHistorical;
			const AnimationReplayResult replayed = ResolveAnimationReplay(history, replay);
			context.Expect(replayed.status == AnimationStatus::Ok
				&& !replayed.suppressHistoricalOneShotEvents
				&& replayed.historicalEvents.size() == 1U,
				"Historical one-shot delivery must occur only when replay explicitly requests it.");

			replay.requireExactTick = true;
			context.Expect(ResolveAnimationReplay(history, replay).status == AnimationStatus::NotReady,
				"An exact seek must not silently substitute an earlier retained tick.");
			lateJoin.seekTick = 2;
			context.Expect(ResolveAnimationReplay(history, lateJoin).status
				== AnimationStatus::RangeOutOfBounds,
				"A seek before retained history must report the recoverable too-old status.");
		});
	}
}
