// AnimationPlaybackLifecycleTests.cpp - playback and runtime ownership checks.

#include "TestHarness.h"

#include "AnimationAuthorityTestHelpers.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimatorPlayback.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Scripting/ScriptingSystem.h"

#include <array>
#include <cmath>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationGuid MakeLifecycleGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimatorInstanceDescriptor MakeDescriptor(std::uint8_t ownerSeed)
		{
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId[0] = ownerSeed;
			descriptor.componentGeneration = 7U;
			return descriptor;
		}

		std::shared_ptr<const SkeletonAsset> MakeReferenceSkeleton()
		{
			auto skeleton = std::make_shared<SkeletonAsset>();
			skeleton->assetId = MakeLifecycleGuid(30U);
			skeleton->compatibilityFingerprint = 0x76543210ULL;
			skeleton->rootJointIndex = 0U;
			SkeletonJoint joint;
			joint.stableId = 11U;
			joint.parentIndex = -1;
			joint.normalizedSourcePath = "/root";
			joint.localBindTransform.translation = {3.0F, 4.0F, 5.0F};
			skeleton->joints.push_back(joint);
			return skeleton;
		}

		AnimationCommand MakePlaybackCommand(
			AnimatorInstanceHandle target,
			AnimationCommandKind kind,
			std::uint64_t sequence)
		{
			AnimationCommand command;
			command.target = target;
			command.kind = kind;
			command.producerId = 1U;
			command.producerSequence = sequence;
			command.correlationId = 100U + sequence;
			return command;
		}

		bool ExecuteAnimationFrame(AnimationSystem& system, float deltaTime, const char* frameId)
		{
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				system.DeclareFrameWorkloads(deltaTime);
			const TerraEngine::Scheduling::FrameExecutionPlan plan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), frameId);
			return plan.valid && plan.completed;
		}

		NativeAnimationProceduralGoalCommand MakePausedProceduralGoal(
			AnimatorInstanceHandle handle,
			std::uint64_t expectedPoseGeneration,
			std::uint64_t expectedPublicationGeneration)
		{
			NativeAnimationProceduralGoalCommand goal{};
			goal.header = {static_cast<std::uint32_t>(sizeof(goal)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalCommand)};
			goal.animator = {handle.slotIndex, handle.generation};
			goal.command = NativeAnimationProceduralCommandKind::SetGoal;
			goal.goalKind = NativeAnimationProceduralGoalKind::CopyTransform;
			goal.coordinateSpace = NativeAnimationProceduralCoordinateSpace::World;
			goal.goalId = 43001U;
			goal.targetJointStableId = 11U;
			goal.goalTransform = {{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}, {1.0F, 1.0F, 1.0F}};
			goal.positionWeight = 1.0F;
			goal.rotationWeight = 0.0F;
			goal.maximumStretchScale = 1.0F;
			goal.producerId = 430U;
			goal.producerSequence = 1U;
			goal.correlationId = 43001U;
			goal.idempotencyKey = 43001U;
			goal.cancellationToken = 43001U;
			goal.expectedPoseGeneration = expectedPoseGeneration;
			goal.expectedPublicationGeneration = expectedPublicationGeneration;
			return goal;
		}
	}

	void RegisterAnimationPlaybackLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Animation.Playback.UnconfiguredControlsRejectWithoutChangingCursor", [](TestContext& context) {
			AnimatorPlayback playback;
			AnimatorPlaybackAdvanceResult advance;
			context.Expect(playback.Play() == AnimationStatus::NotReady
				&& playback.Seek(5) == AnimationStatus::NotReady
				&& playback.Advance({0.0, 0.0, 1}, advance) == AnimationStatus::NotReady
				&& playback.AbsoluteTick() == 0,
				"Playback controls must wait for a checked clip-time configuration instead of treating a default cursor as a valid point clip.");
		});

		registry.Add("Animation.Playback.AuthoredStartTickWrapsLoopingClips", [](TestContext& context) {
			context.Expect(
				ResolveAnimatorInitialPlaybackTick(60, 60, true) == 0
				&& ResolveAnimatorInitialPlaybackTick(75, 60, true) == 15,
				"A looping authored start tick must wrap through the resolved duration so large phase offsets remain distributed.");
			context.Expect(
				ResolveAnimatorInitialPlaybackTick(60, 60, false) == 60
				&& ResolveAnimatorInitialPlaybackTick(75, 60, false) == 60,
				"A one-shot authored start tick must retain the exact final endpoint.");
			context.Expect(
				ResolveAnimatorInitialPlaybackTick(-1, 60, true) == 0
				&& ResolveAnimatorInitialPlaybackTick(1, 0, true) == 0,
				"Authored start ticks must saturate safely at zero for negative requests and point clips.");
		});

		registry.Add("Animation.Playback.LifecycleControlsAndTimePolicies", [](TestContext& context) {
			static_assert(!std::is_same_v<AnimatorPlayback, AnimatorInstanceDescriptor>,
				"Transient playback must remain separate from serialized authored data.");
			AnimatorPlayback playback;
			AnimatorPlaybackConfiguration configuration;
			configuration.durationTicks = 10;
			configuration.fixedStepTicks = 1;
			configuration.timePolicy = AnimatorTimePolicy::Fixed;
			context.Expect(playback.Configure(configuration) == AnimationStatus::Ok
				&& playback.Play() == AnimationStatus::Ok,
				"A valid fixed-time animator should enter playing state.");

			AnimatorPlaybackAdvanceResult advance;
			context.Expect(playback.Advance({0.0, 0.0, 4}, advance) == AnimationStatus::Ok
				&& advance.currentAbsoluteTick == 4,
				"Fixed time must advance by exact integer ticks.");
			context.Expect(playback.Pause() == AnimationStatus::Ok
				&& playback.Advance({1.0, 1.0, 4}, advance) == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 4,
				"Paused playback must retain its exact fixed cursor.");
			context.Expect(playback.Resume() == AnimationStatus::Ok
				&& playback.SetSpeed(0.5F) == AnimationStatus::Ok
				&& playback.Advance({0.0, 0.0, 4}, advance) == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 6,
				"Resume and fractional speed must use deterministic fixed-point accumulation.");
			context.Expect(playback.SetReverse(true) == AnimationStatus::Ok
				&& playback.Advance({0.0, 0.0, 2}, advance) == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 5,
				"Reverse playback must traverse the same cursor without a hidden seek.");

			context.Expect(playback.SetTimePolicy(AnimatorTimePolicy::Scaled) == AnimationStatus::Ok
				&& playback.SetReverse(false) == AnimationStatus::Ok
				&& playback.SetSpeed(1.0F) == AnimationStatus::Ok
				&& playback.Advance({1.0 / 60.0, 2.0 / 60.0, 0}, advance) == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 6,
				"Scaled time must read only the scaled frame clock.");
			context.Expect(playback.SetTimePolicy(AnimatorTimePolicy::Unscaled) == AnimationStatus::Ok
				&& playback.Advance({0.0, 2.0 / 60.0, 0}, advance) == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 8,
				"Unscaled time must continue while the scaled clock is zero.");
			context.Expect(playback.Seek(10) == AnimationStatus::Ok
				&& playback.Advance({0.0, 1.0 / 60.0, 0}, advance) == AnimationStatus::Ok
				&& playback.RunState() == AnimatorPlaybackRunState::Completed
				&& playback.AbsoluteTick() == 10,
				"A non-looping exact end must hold its endpoint without wrapping.");
			context.Expect(playback.SetLooping(true) == AnimationStatus::Ok
				&& playback.Play() == AnimationStatus::Ok
				&& playback.Seek(9) == AnimationStatus::Ok
				&& playback.SetTimePolicy(AnimatorTimePolicy::Fixed) == AnimationStatus::Ok
				&& playback.Advance({0.0, 0.0, 25}, advance) == AnimationStatus::Ok
				&& advance.resolved.resolvedTick == 4 && advance.resolved.loopIndex == 3,
				"Looping catch-up must resolve all crossed loops in one bounded operation.");
			context.Expect(playback.Stop() == AnimationStatus::Ok
				&& playback.AbsoluteTick() == 0
				&& playback.RunState() == AnimatorPlaybackRunState::Stopped,
				"Stop must restore the authored start pose and clear transient fractions.");
		});

		registry.Add("Animation.Playback.DependencyFailurePublishesCompleteSafePose", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceDescriptor descriptor = MakeDescriptor(21U);
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(descriptor);
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeReferenceSkeleton();
			binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
			context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::MissingAsset,
				"A missing clip must become an explicit degraded binding, not a partial generation mix.");
			context.Expect(system.SubmitCommand(MakePlaybackCommand(handle, AnimationCommandKind::Play, 1U)).status
				== AnimationStatus::Queued,
				"Playback commands should enter the typed queue before worker capture.");
			context.Expect(ExecuteAnimationFrame(system, 1.0F / 60.0F, "animation-safe-pose"),
				"The scheduler should complete a degraded animator through the normal boundaries.");

			const PinnedAnimationAuthorityRow published =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			context.Expect(published && published->safeFallback
				&& published->status == AnimationStatus::MissingAsset
				&& published->componentGeneration == descriptor.componentGeneration
				&& published->skeleton.generation == binding.skeletonGeneration
				&& published->skeletonCompatibilityFingerprint
					== binding.skeleton->compatibilityFingerprint,
				"The immutable publication must carry every generation and the visible safe-pose diagnostic.");
			context.Expect(published && published->snapshot
				&& published->snapshot->pose->LocalTransforms().size() == 1U
				&& published->snapshot->pose->LocalTransforms()[0].translation
					== binding.skeleton->joints[0].localBindTransform.translation,
				"Missing dependencies must publish the complete compatible reference pose.");
			context.Expect(PinnedAnimationAuthorityRow::ForObject(system, descriptor.ownerObjectId) == published,
				"Owner-object lookup must return the same generation bundle used by handle lookup.");
			(void)system.InvalidateAnimatorInstance(handle);
		});

		registry.Add("Animation.Playback.StaleWorkPlayModeAndSceneReloadAreGenerationSafe", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceDescriptor authored = MakeDescriptor(31U);
			const AnimatorInstanceHandle original = system.CreateAnimatorInstance(authored);
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeReferenceSkeleton();
			(void)system.BindAnimatorPlayback(original, binding);
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> staleFrame =
				system.DeclareFrameWorkloads(1.0F / 60.0F);

			context.Expect(system.InvalidateAnimatorInstance(original),
				"Scene reload must retire the old runtime instance before slot reuse.");
			const AnimatorInstanceHandle replacement = system.CreateAnimatorInstance(authored);
			(void)system.BindAnimatorPlayback(replacement, binding);
			const std::uint64_t generationBeforeStaleWork = system.AuthorityPublicationGeneration();
			const TerraEngine::Scheduling::FrameExecutionPlan stalePlan =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(staleFrame), "animation-stale-lifecycle");
			context.Expect(stalePlan.valid && stalePlan.completed
				&& !PinnedAnimationAuthorityRow::ForAnimator(system, replacement)
				&& system.AuthorityPublicationGeneration() == generationBeforeStaleWork,
				"Descriptors retained across a reload must fail closed instead of targeting the reused slot.");
			context.Expect(replacement.slotIndex == original.slotIndex
				&& replacement.generation > original.generation
				&& !system.IsAnimatorInstanceAlive(original),
				"Reloaded runtime ownership must preserve monotonically increasing slot generations.");

			context.Expect(ExecuteAnimationFrame(system, 1.0F / 60.0F, "animation-reloaded"),
				"A newly declared frame should publish the replacement generation normally.");
			context.Expect(static_cast<bool>(
				PinnedAnimationAuthorityRow::ForAnimator(system, replacement)),
				"Only replacement-session work may become visible after reload.");
			system.Shutdown();
			context.Expect(!system.IsAnimatorInstanceAlive(replacement)
				&& authored.componentGeneration == 7U,
				"Stopping Play Mode must destroy transient runtime state without modifying authored data.");
			system.Initialize();
			const AnimatorInstanceHandle restored = system.CreateAnimatorInstance(authored);
			context.Expect(restored.IsValid() && restored.generation > replacement.generation,
				"Starting Play Mode again must create a new generation from the restored authored scene.");
			(void)system.InvalidateAnimatorInstance(restored);
		});

		registry.Add("Animation.Playback.LifecycleInvalidationRetiresAuthorityBeforeReplacement", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			const AnimatorInstanceDescriptor descriptor = MakeDescriptor(37U);
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(descriptor);
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeReferenceSkeleton();
			(void)system.BindAnimatorPlayback(handle, binding);
			context.Expect(ExecuteAnimationFrame(system, 1.0F / 60.0F,
				"animation-lifecycle-retire-baseline"),
				"The lifecycle fixture needs one complete authority root before retirement.");
			const std::shared_ptr<const AnimationAuthorityFrame> before =
				system.LoadPublishedAuthorityFrame();
			const std::uint64_t generationBefore = system.AuthorityPublicationGeneration();
			context.Expect(before && before->IsValid(),
				"The baseline frame must expose a structurally valid immutable authority root.");

			// Scene replacement is a lifecycle event, not an ordinary evaluation error.
			// A fresh reader must therefore see no old root between the invalidation and
			// the first complete replacement frame.
			context.Expect(system.NotifyLifecycleInvalidation(
				AnimationLifecycleBoundary::Scene) == AnimationStatus::Ok
				&& !system.LoadPublishedAuthorityFrame()
				&& system.AuthorityPublicationGeneration() > generationBefore,
				"Scene invalidation must retire the old authority root and advance its public evidence atomically.");

			context.Expect(ExecuteAnimationFrame(system, 1.0F / 60.0F,
				"animation-lifecycle-retire-replacement"),
				"A live animator should publish a fresh root after the lifecycle boundary.");
			const std::shared_ptr<const AnimationAuthorityFrame> replacement =
				system.LoadPublishedAuthorityFrame();
			context.Expect(replacement && replacement->IsValid() && replacement != before
				&& replacement->authorityGeneration > generationBefore,
				"Only a newly sealed authority generation may become visible after lifecycle retirement.");
			(void)system.InvalidateAnimatorInstance(handle);
		});

		registry.Add("Animation.Playback.LifecycleInvalidationRetiresPublishedPresentationRoot",
			[](TestContext& context) {
				AnimationSystem& system = AnimationSystem::Instance();
				AnimationPresentationSystem& presentation =
					AnimationPresentationSystem::Instance();
				// Reset only the presentation singleton's submitted-source stream. The
				// fixture then publishes through the ordinary worker/protected workloads.
				presentation.Shutdown();
				presentation.Initialize();
				system.Initialize();
				const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(MakeDescriptor(47U));
				AnimatorPlaybackBinding binding;
				binding.skeleton = MakeReferenceSkeleton();
				(void)system.BindAnimatorPlayback(handle, binding);
				const bool authorityComplete = ExecuteAnimationFrame(system, 1.0F / 60.0F,
					"animation-presentation-lifecycle-authority");
				std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
					presentation.DeclareFrameWorkloads(1.0F / 60.0F);
				const TerraEngine::Scheduling::FrameExecutionPlan presentationPlan =
					TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
						std::move(workloads), "animation-presentation-lifecycle-publish");
				const std::shared_ptr<const AnimationPresentationFrame> before =
					presentation.LoadPublishedAnimationPresentationFrame();
				context.Expect(authorityComplete && presentationPlan.valid && presentationPlan.completed
					&& before && before->IsValid(),
					"The fixture must publish one complete post-physics presentation root before lifecycle retirement.");

				const AnimationStatus invalidated = system.NotifyLifecycleInvalidation(
					AnimationLifecycleBoundary::Scene);
				context.Expect(invalidated == AnimationStatus::Ok
					&& !system.LoadPublishedAuthorityFrame()
					&& !presentation.LoadPublishedGpuAnimationPresentationPlan()
					&& !presentation.LoadPublishedAnimationPresentationFrame(),
					"Scene invalidation must atomically retire authority and both presentation roots before replacement work.");
				(void)system.InvalidateAnimatorInstance(handle);
			});

		registry.Add("Animation.Playback.BatchInvalidationRetiresCrowdAtomically", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			system.Initialize();
			std::array<AnimatorInstanceHandle, 3U> handles{
				system.CreateAnimatorInstance(MakeDescriptor(61U)),
				system.CreateAnimatorInstance(MakeDescriptor(62U)),
				system.CreateAnimatorInstance(MakeDescriptor(63U))};
			const std::array<AnimatorInstanceHandle, 4U> request{
				handles[0], handles[1], handles[1], handles[2]};
			context.Expect(system.InvalidateAnimatorInstances(request) == handles.size(),
				"A crowd retirement must invalidate each unique live handle exactly once.");
			context.Expect(std::none_of(handles.begin(), handles.end(),
				[&system](AnimatorInstanceHandle handle) {
					return system.IsAnimatorInstanceAlive(handle);
				}),
				"The batch boundary must not leave a partially live runtime crowd.");
			context.Expect(system.InvalidateAnimatorInstances(request) == 0U,
				"Repeating a stale batch must be an idempotent no-op.");
		});

		registry.Add("Animation.Playback.PausedPlayModeRetainsPublicationUntilExplicitStep", [](TestContext& context) {
			AnimationSystem& system = AnimationSystem::Instance();
			ScriptingSystem& scripting = ScriptingSystem::Instance();
			system.Initialize();
			const AnimatorInstanceHandle handle = system.CreateAnimatorInstance(MakeDescriptor(41U));
			AnimatorPlaybackBinding binding;
			binding.skeleton = MakeReferenceSkeleton();
			context.Expect(system.BindAnimatorPlayback(handle, binding) == AnimationStatus::MissingAsset
				&& ExecuteAnimationFrame(system, 1.0F / 60.0F, "animation-paused-baseline"),
				"A safe-pose binding must first publish a generation that procedural input can target.");
			const PinnedAnimationAuthorityRow baseline =
				PinnedAnimationAuthorityRow::ForAnimator(system, handle);
			if(!baseline || !baseline->snapshot) {
				context.Expect(false, "The paused-playback fixture requires a published baseline pose.");
				(void)system.InvalidateAnimatorInstance(handle);
				return;
			}

			scripting.SetRunning(true);
			scripting.SetPaused(true);
			(void)scripting.BeginFrameTiming(1.0F / 240.0F);
			const std::uint64_t publicationBeforePause = system.AuthorityPublicationGeneration();
			const std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> pausedWorkloads =
				system.DeclareFrameWorkloads(1.0F / 240.0F);
			context.Expect(pausedWorkloads.empty()
				&& system.AuthorityPublicationGeneration() == publicationBeforePause
				&& PinnedAnimationAuthorityRow::ForAnimator(system, handle) == baseline,
				"Paused Play Mode must retain animator membership and its immutable publication without declaring evaluation work.");

			const NativeAnimationProceduralGoalCommand goal = MakePausedProceduralGoal(
				handle, baseline->snapshot->publicationGeneration,
				baseline->authorityPublicationGeneration);
			context.Expect(system.SubmitProceduralGoal(goal).status == NativeAnimationStatus::Queued,
				"A procedural set may queue against the stable paused publication before the explicit step.");
			scripting.StepExplicit(1.0F / 60.0F);
			(void)scripting.BeginFrameTiming(1.0F / 240.0F);
			const ScriptingSystem::RuntimeTimeSnapshot stepTime = scripting.GetRuntimeTimeSnapshot();
			context.Expect(stepTime.playSessionPaused && stepTime.advancesSimulation,
				"The explicit-step request must be latched before the scheduler declares animation work.");
			context.Expect(std::abs(stepTime.simulationDeltaTime - (1.0F / 60.0F)) < 1.0e-6F,
				"Animation planning must receive the fixed explicit-step duration instead of the render-frame duration.");
			context.Expect(ExecuteAnimationFrame(system, 1.0F / 240.0F, "animation-paused-capture"),
				"One explicit paused step must capture and publish the queued procedural goal.");
			std::array<NativeAnimationProceduralGoalCommand, 1U> activeGoals{};
			std::uint32_t activeGoalCount = 0U;
			context.Expect(system.CopyProceduralGoals(handle, activeGoals, activeGoalCount) == AnimationStatus::Ok
				&& activeGoalCount == 1U && activeGoals[0].goalId == goal.goalId,
				"The capture step must make the accepted copied goal visible; a following step performs its evaluation.");

			scripting.SetPaused(false);
			scripting.SetRunning(false);
			(void)system.InvalidateAnimatorInstance(handle);
		});
	}
}
