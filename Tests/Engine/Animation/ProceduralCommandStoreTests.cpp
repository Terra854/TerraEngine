// ProceduralCommandStoreTests.cpp - Focused contract tests for procedural gameplay commands.

#include "TestHarness.h"

#include "Animation/Procedural/ProceduralCommandStore.h"
#include "Animation/Procedural/ProceduralTypes.h"

#include <array>
#include <limits>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimatorInstanceDescriptor Descriptor(std::uint8_t seed) {
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = seed;
			descriptor.componentGeneration = 1U;
			return descriptor;
		}

		NativeAnimationAnimatorHandle NativeHandle(AnimatorInstanceHandle value) {
			return {value.slotIndex, value.generation};
		}

		NativeAnimationProceduralGoalCommand Goal(
			NativeAnimationAnimatorHandle animator,
			std::uint32_t producer,
			std::uint64_t sequence,
			std::uint64_t correlation,
			std::uint64_t idempotency,
			std::uint64_t cancellation) {
			NativeAnimationProceduralGoalCommand command{};
			command.header = {static_cast<std::uint32_t>(sizeof(command)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalCommand)};
			command.animator = animator;
			command.command = NativeAnimationProceduralCommandKind::SetGoal;
			command.goalKind = NativeAnimationProceduralGoalKind::PositionAndRotation;
			command.coordinateSpace = NativeAnimationProceduralCoordinateSpace::World;
			command.layerIndex = 2U;
			command.goalId = 77U;
			command.targetJointStableId = 991U;
			command.goalTransform = {{1.0F, 2.0F, 3.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
			command.poleVector = {0.0F, 1.0F, 0.0F};
			command.priority = 3;
			command.positionWeight = 1.0F;
			command.rotationWeight = 1.0F;
			command.poleWeight = 0.0F;
			command.maximumStretchScale = 1.0F;
			command.producerId = producer;
			command.producerSequence = sequence;
			command.correlationId = correlation;
			command.idempotencyKey = idempotency;
			command.cancellationToken = cancellation;
			command.targetTick = 12;
			command.expectedPublicationGeneration = 5U;
			command.expectedPoseGeneration = 6U;
			return command;
		}

		NativeAnimationProceduralResultSnapshot Result(NativeAnimationAnimatorHandle animator) {
			NativeAnimationProceduralResultSnapshot result{};
			result.header = {static_cast<std::uint32_t>(sizeof(result)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralResultSnapshot)};
			result.status = NativeAnimationStatus::Ok;
			result.animator = animator;
			result.solver = NativeAnimationProceduralSolverKind::TwoBoneIk;
			result.state = NativeAnimationProceduralSolverState::Solved;
			result.goalId = 77U;
			result.targetJointStableId = 991U;
			result.resultTransform = {{3.0F, 2.0F, 1.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
			result.inputWeight = 1.0F;
			result.finalContribution = 1.0F;
			result.iterationLimit = 8U;
			result.poseGeneration = 6U;
			result.componentGeneration = 1U;
			result.skeletonGeneration = 2U;
			result.programGeneration = 3U;
			result.observationTick = 12;
			result.publicationGeneration = 7U;
			result.producerOrder = 4U;
			result.correlationId = 55U;
			return result;
		}
	}

	void RegisterProceduralCommandStoreTests(TestRegistry& registry) {
		registry.Add("Animation.ProceduralCommands.DeferMutationAndOrderAtProtectedBoundary", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const NativeAnimationAnimatorHandle animator = NativeHandle(instances.Create(Descriptor(1U)));
			ProceduralCommandStore store;
			NativeAnimationProceduralGoalCommand later = Goal(animator, 9U, 2U, 90U, 900U, 9000U);
			NativeAnimationProceduralGoalCommand earlier = Goal(animator, 2U, 1U, 20U, 200U, 2000U);
			earlier.goalId = 78U;
			earlier.targetTick = 11;
			context.Expect(store.Submit(later, instances).status == NativeAnimationStatus::Queued
				&& store.Submit(earlier, instances).status == NativeAnimationStatus::Queued,
				"Copied procedural commands must queue before the animation boundary.");
			std::array<NativeAnimationProceduralGoalCommand, 2U> before{};
			std::uint32_t count = 0U;
			context.Expect(store.CopyActiveGoals(animator, before, count) == AnimationStatus::NotReady,
				"No gameplay command may mutate active procedural goals before publication.");
			std::vector<NativeAnimationProceduralGoalCommand> drained;
			store.DrainThrough(12, drained, instances);
			context.Expect(drained.size() == 2U && drained[0].goalId == 78U && drained[1].goalId == 77U,
				"Due commands must use deterministic tick, animator, priority, producer, and stable-id ordering.");
			for(const auto& command : drained) store.PublishCompletion(command, AnimationStatus::Ok, 12, 8U);
			context.Expect(store.CopyActiveGoals(animator, before, count) == AnimationStatus::Ok && count == 2U,
				"Only protected publication should make the complete copied goal batch observable.");
		});

		registry.Add("Animation.ProceduralCommands.CaptureAllActiveGoalsAsOneOrderedSnapshot", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const NativeAnimationAnimatorHandle first = NativeHandle(instances.Create(Descriptor(10U)));
			const NativeAnimationAnimatorHandle second = NativeHandle(instances.Create(Descriptor(11U)));
			ProceduralCommandStore store;
			// Submit in reverse animator order to prove retained storage and bulk output
			// use generation-safe handle order rather than producer arrival order.
			const auto secondGoal = Goal(second, 2U, 1U, 2U, 2U, 20U);
			const auto firstGoal = Goal(first, 1U, 1U, 1U, 1U, 10U);
			(void)store.Submit(secondGoal, instances);
			(void)store.Submit(firstGoal, instances);
			std::vector<NativeAnimationProceduralGoalCommand> drained;
			store.DrainThrough(12, drained, instances);
			for(const auto& command : drained) {
				store.PublishCompletion(command, AnimationStatus::Ok, 12, 5U);
			}
			std::vector<ActiveProceduralGoalRange> ranges;
			std::vector<NativeAnimationProceduralGoalCommand> goals;
			context.Expect(store.CaptureActiveGoals(ranges, goals) == AnimationStatus::Ok
					&& ranges.size() == 2U && goals.size() == 2U,
				"One protected capture should return every retained active goal.");
			context.Expect(ranges.size() == 2U
					&& ranges[0U].animator.slotIndex == first.slotIndex
					&& ranges[1U].animator.slotIndex == second.slotIndex
					&& goals[ranges[0U].firstGoal].animator.slotIndex == first.slotIndex
					&& goals[ranges[1U].firstGoal].animator.slotIndex == second.slotIndex,
				"Flat ranges must merge linearly with AnimatorInstanceStore slot order.");
		});

		registry.Add("Animation.ProceduralCommands.ReconcileIdempotencyAndCancellationWithoutAliasing", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const NativeAnimationAnimatorHandle animator = NativeHandle(instances.Create(Descriptor(2U)));
			ProceduralCommandStore store;
			const NativeAnimationProceduralGoalCommand original = Goal(animator, 3U, 4U, 44U, 444U, 4444U);
			const NativeAnimationProceduralGoalReceipt accepted = store.Submit(original, instances);
			const NativeAnimationProceduralGoalReceipt duplicate = store.Submit(original, instances);
			NativeAnimationProceduralGoalCommand conflict = original;
			conflict.positionWeight = 0.5F;
			const NativeAnimationProceduralGoalReceipt rejected = store.Submit(conflict, instances);
			context.Expect(accepted.status == NativeAnimationStatus::Queued && duplicate.duplicate == 1U
				&& duplicate.status == NativeAnimationStatus::Queued && rejected.duplicate == 1U
				&& rejected.idempotencyConflict == 1U && rejected.status == NativeAnimationStatus::Rejected,
				"Exact retries must receive the retained receipt while key conflicts are rejected atomically.");
			const NativeAnimationProceduralGoalReceipt cancellation = store.Cancel(4444U, 999U);
			const NativeAnimationProceduralGoalReceipt retained = store.ReadReceipt(3U, 4U, 44U);
			context.Expect(cancellation.status == NativeAnimationStatus::Cancelled && cancellation.correlationId == 999U
				&& retained.status == NativeAnimationStatus::Cancelled && retained.correlationId == 44U
				&& store.PendingCount() == 0U,
				"Cancellation must remove only a pending copy and preserve submission correlation for receipt polling.");
		});

		registry.Add("Animation.ProceduralCommands.RejectMalformedGenerationsAndBoundActiveGoals", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const NativeAnimationAnimatorHandle animator = NativeHandle(instances.Create(Descriptor(3U)));
			ProceduralCommandStore store(512U, 512U);
			NativeAnimationProceduralGoalCommand malformed = Goal(animator, 1U, 1U, 1U, 1U, 1U);
			malformed.expectedPoseGeneration = 0U;
			context.Expect(store.Submit(malformed, instances).status == NativeAnimationStatus::InvalidArgument,
				"Set goals without an expected pose generation must fail before entering deferred storage.");
			malformed = Goal(animator, 0U, 2U, 2U, 2U, 2U);
			context.Expect(store.Submit(malformed, instances).status == NativeAnimationStatus::InvalidArgument,
				"Every deferred command must name a nonzero producer for deterministic ordering and replay.");
			NativeAnimationProceduralGoalCommand clear{};
			clear.header = {static_cast<std::uint32_t>(sizeof(clear)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalCommand)};
			clear.animator = animator;
			clear.command = NativeAnimationProceduralCommandKind::ClearGoal;
			clear.goalId = 77U;
			clear.producerId = 1U;
			clear.producerSequence = 3U;
			clear.correlationId = 3U;
			clear.targetTick = 12;
			clear.positionWeight = 0.5F;
			context.Expect(store.Submit(clear, instances).status == NativeAnimationStatus::InvalidArgument,
				"Clear commands must keep every ignored payload byte semantically zero for ABI extensibility.");
			for(std::uint64_t index = 0U; index < kProceduralGoalHardMaximum; ++index) {
				NativeAnimationProceduralGoalCommand command = Goal(animator, 8U, index + 1U, index + 1U,
					index + 1U, index + 1000U);
				command.goalId = index + 1U;
				(void)store.Submit(command, instances);
			}
			std::vector<NativeAnimationProceduralGoalCommand> drained;
			store.DrainThrough(12, drained, instances);
			for(const auto& command : drained) store.PublishCompletion(command, AnimationStatus::Ok, 12, 2U);
			NativeAnimationProceduralGoalCommand overflow = Goal(animator, 8U, 300U, 300U, 300U, 1300U);
			overflow.goalId = 1000U;
			(void)store.Submit(overflow, instances);
			store.DrainThrough(12, drained, instances);
			store.PublishCompletion(drained.front(), AnimationStatus::Ok, 12, 3U);
			context.Expect(store.ReadReceipt(8U, 300U, 300U).status == NativeAnimationStatus::HardLimitExceeded,
				"The 256-goal animator hard cap must reject a new set without changing active goals.");
		});

		registry.Add("Animation.ProceduralCommands.CopyWholeResultPublicationsOrNothing", [](TestContext& context) {
			AnimatorInstanceStore instances;
			const NativeAnimationAnimatorHandle animator = NativeHandle(instances.Create(Descriptor(4U)));
			ProceduralCommandStore store;
			const NativeAnimationProceduralResultSnapshot published = Result(animator);
			context.Expect(store.PublishResults(animator, {&published, 1U}, instances) == AnimationStatus::Ok,
				"A finite generation-stamped result must publish as a copied batch.");
			NativeAnimationProceduralResultSnapshot sentinel{};
			sentinel.goalId = std::numeric_limits<std::uint64_t>::max();
			std::array<NativeAnimationProceduralResultSnapshot, 1U> tooSmall{};
			std::uint32_t required = 0U;
			context.Expect(store.CopyResults(animator, {}, required) == AnimationStatus::BufferTooSmall && required == 1U,
				"The count query must report the full required result count without exposing store memory.");
			context.Expect(store.CopyResults(animator, tooSmall, required) == AnimationStatus::Ok
				&& tooSmall[0].goalId == published.goalId,
				"A sufficient destination must receive one coherent copied publication.");
			NativeAnimationProceduralResultSnapshot invalid = published;
			invalid.targetErrorMeters = std::numeric_limits<float>::quiet_NaN();
			context.Expect(store.PublishResults(animator, {&invalid, 1U}, instances) == AnimationStatus::InvalidArgument,
				"A rejected result batch must preserve the previous publication atomically.");
			context.Expect(store.CopyResults(animator, tooSmall, required) == AnimationStatus::Ok
				&& tooSmall[0].goalId == published.goalId,
				"Invalid result input must not partially replace the retained snapshot.");
		});
	}
}
