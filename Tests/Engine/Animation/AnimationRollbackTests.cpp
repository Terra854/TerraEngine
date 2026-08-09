// AnimationRollbackTests.cpp - Deterministic correction, event, and mutation contracts.

#include "TestHarness.h"

#include "Animation/Determinism/AnimationRollback.h"
#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <algorithm>
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

		AnimationRollbackSnapshot Start(AnimationFixedTick tick = 5)
		{
			AnimationRollbackSnapshot snapshot;
			snapshot.animator = {1U, 1U};
			snapshot.tick = tick;
			snapshot.publicationGeneration = 2U;
			snapshot.fingerprint = {11U, 12U, 13U, 14U, 15U, 16U, 17U};
			snapshot.generations = {21U, 22U, 23U, 24U, 25U, 26U, 27U};
			snapshot.state = {{
				AnimationDeterministicStateKind::Controller,
				0U,
				0U,
				1U,
				{static_cast<std::uint64_t>(tick), 0U, 0U, 0U}}};
		AnimationEventCursorRollbackState delivered;
		delivered.streamId = 30U;
		delivered.stateInstanceId = 31U;
		delivered.clipAssetId = Guid(1U);
		delivered.eventId = 32U;
		delivered.sourceTick = tick;
		delivered.deliveredSequence = 5U;
		delivered.deliveredThroughTick = tick;
		delivered.delivered = true;
		snapshot.events.push_back(delivered);
		snapshot.eventWatermark = {30U, 5U, tick};
		if(AnimationRollbackSnapshotCodec{}.Capture(snapshot, snapshot) != AnimationStatus::Ok) {
			return {};
		}
		return snapshot;
	}

	AnimationRollbackStep DeterministicStep()
	{
		return [](AnimationFixedTick tick,
			std::span<const AnimationDeterministicStateRecord> inputs,
			AnimationRollbackSnapshot& output) {
			output = Start(tick);
			output.state.insert(output.state.end(), inputs.begin(), inputs.end());
			output.eventWatermark = {30U, static_cast<std::uint64_t>(tick), tick};
			return AnimationStatus::Ok;
		};
	}

	AnimationRollbackExternalInput Input(
		AnimationFixedTick tick,
		std::uint32_t producer,
		std::uint64_t sequence,
		std::uint64_t stableId,
		std::uint64_t value)
	{
		AnimationRollbackExternalInput input;
		input.tick = tick;
		input.producerId = producer;
		input.producerSequence = sequence;
		input.value = {
			AnimationDeterministicStateKind::Parameter,
			0U,
			0U,
			stableId,
			{value, 0U, 0U, 0U}};
		return input;
	}

	static_assert(AnimationTransportValueRecord<AnimationRollbackDeferredMutation>);
	}

	void RegisterAnimationRollbackTests(TestRegistry& registry)
	{
		registry.Add("Animation.Rollback.CleanRunEquivalenceNormalizesInputs", [](TestContext& context) {
			const AnimationRollbackSnapshot start = Start();
			const std::array unsorted{
				Input(7, 9U, 2U, 72U, 720U),
				Input(6, 4U, 1U, 61U, 610U),
				Input(7, 2U, 1U, 71U, 710U),
				Input(6, 4U, 1U, 61U, 610U)};
			const std::array canonical{
				Input(6, 4U, 1U, 61U, 610U),
				Input(7, 2U, 1U, 71U, 710U),
				Input(7, 9U, 2U, 72U, 720U)};

			AnimationRollback rollback;
			AnimationRollbackResult corrected;
			AnimationRollbackResult clean;
			const AnimationStatus correctedStatus = rollback.Resimulate(
				start, 7, unsorted, DeterministicStep(), corrected);
			const AnimationStatus cleanStatus = rollback.Resimulate(
				start, 7, canonical, DeterministicStep(), clean);

			context.Expect(correctedStatus == AnimationStatus::Ok
				&& cleanStatus == AnimationStatus::Ok
				&& corrected.snapshot == clean.snapshot
				&& corrected.appliedInputs == clean.appliedInputs,
				"Arrival order and identical retransmission must normalize to the same clean-run result.");
			context.Expect(corrected.snapshot.tick == 7
				&& corrected.snapshot.stateChecksum == clean.snapshot.stateChecksum
				&& corrected.snapshot.poseChecksum == clean.snapshot.poseChecksum,
				"A correction inside the retained window must reproduce exact final state and pose hashes.");
		});

		registry.Add("Animation.Rollback.ConflictingOrOutOfWindowInputRejectsAtomically", [](TestContext& context) {
			const AnimationRollbackSnapshot start = Start();
			const std::array conflicting{
				Input(6, 4U, 1U, 61U, 610U),
				Input(6, 4U, 1U, 61U, 999U)};
			AnimationRollbackResult output;
			output.status = AnimationStatus::Ok;
			output.snapshot = Start(99);
			const AnimationRollbackResult lastGood = output;
			AnimationRollback rollback;

			context.Expect(rollback.Resimulate(start, 7, conflicting,
				DeterministicStep(), output) == AnimationStatus::DeterminismViolation
				&& output == lastGood,
				"Conflicting retransmissions with one producer identity must not replace last-good output.");
			const AnimationRollbackExternalInput tooEarly = Input(5, 4U, 2U, 62U, 620U);
			context.Expect(rollback.Resimulate(start, 7, {&tooEarly, 1U},
				DeterministicStep(), output) == AnimationStatus::InvalidArgument
				&& output == lastGood,
				"Inputs at or before the restore point must be rejected without partial resimulation.");
		});

		registry.Add("Animation.Rollback.SuppressesHistoricalEventsAndBuildsTypedMutations", [](TestContext& context) {
			const AnimationRollbackSnapshot start = Start();
			AnimationRollbackExternalInput attachment = Input(6, 8U, 1U, 80U, 1U);
			attachment.value.kind = AnimationDeterministicStateKind::Attachment;
			AnimationRollbackResult result;
			AnimationRollback rollback;
			const AnimationStatus status = rollback.Resimulate(start, 6, {&attachment, 1U},
				[](AnimationFixedTick tick,
					std::span<const AnimationDeterministicStateRecord> inputs,
					AnimationRollbackSnapshot& output) {
					output = Start(tick);
					output.state.insert(output.state.end(), inputs.begin(), inputs.end());

					AnimationEventCursorRollbackState historical = output.events.front();
					historical.sourceTick = 5;
					historical.deliveredThroughTick = 5;
					AnimationEventCursorRollbackState fresh = historical;
					fresh.eventId = 33U;
					fresh.sourceTick = tick;
					fresh.deliveredSequence = 6U;
					fresh.deliveredThroughTick = tick;
					output.events = {historical, fresh};
					output.eventWatermark = {30U, 6U, tick};
					output.rootMotion = {
						tick,
						{EncodeAnimationNetworkFloat(1.0F), 0U, 0U, 0U},
						44U,
						true,
						true};
					return AnimationStatus::Ok;
				}, result);

			context.Expect(status == AnimationStatus::Ok
				&& result.eventsToPublish.size() == 1U
				&& result.eventsToPublish.front().deliveredSequence == 6U
				&& result.suppressedHistoricalEventCount == 1U,
				"Rollback must publish only unseen one-shot event identities after the restore watermark.");
			context.Expect(result.rootMotionCorrections.size() == 1U
				&& result.deferredMutations.commands.size() == 3U
				&& result.deferredMutations.requiresProtectedSceneFlush
				&& result.deferredMutations.requiresPhysicsBoundary
				&& result.deferredMutations.checksum
					== ComputeAnimationRollbackMutationChecksum(result.deferredMutations),
				"Root, physics, and attachment correction must leave workers as one typed atomic command batch.");
			context.Expect(result.deferredMutations.commands[0].animator == start.animator
				&& result.deferredMutations.generations == start.generations,
				"Every deferred mutation must retain animator and scene/physics generation identity.");
		});

		registry.Add("Animation.Rollback.BadStepPublicationPreservesLastGood", [](TestContext& context) {
			const AnimationRollbackSnapshot start = Start();
			AnimationRollbackResult output;
			output.status = AnimationStatus::Ok;
			output.snapshot = Start(88);
			output.snapshot.publicationGeneration = 91U;
			output.snapshot.generations = {101U, 102U, 103U, 104U, 105U, 106U, 107U};
			const AnimationRollbackResult lastGood = output;
			AnimationRollback rollback;
			const AnimationStatus status = rollback.Resimulate(start, 6, {},
				[](AnimationFixedTick tick,
					std::span<const AnimationDeterministicStateRecord>,
					AnimationRollbackSnapshot& produced) {
					produced = Start(tick);
					produced.fingerprint.programAsset += 1U;
					return AnimationStatus::Ok;
				}, output);
			context.Expect(status == AnimationStatus::DeterminismViolation && output == lastGood
				&& output.snapshot.publicationGeneration == 91U
				&& output.snapshot.generations == lastGood.snapshot.generations,
				"A mismatched asset/program publication must reject atomically and retain every last-good generation.");
		});
	}
}
