// LocomotionRootLifecycleTests.cpp - root timeline samples through real animation frames.

#include "../TestHarness.h"

#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Runtime/RootMotion.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;
		constexpr float kStep = 1.0F / 60.0F;

		bool RunAnimationFrame(AnimationSystem& animation)
		{
			auto workloads = animation.DeclareFrameWorkloads(kStep);
			const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(
				std::move(workloads), "locomotion-root-lifecycle");
			return result.valid && result.completed;
		}

		// This fixture deliberately drives the same cooked asset and protected
		// Animation/Locomotion/Physics boundaries used by the installed runtime.
		class RootTimelineRuntime final {
		public:
			explicit RootTimelineRuntime(TestContext& context) : context_(context)
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				locomotion_.Shutdown();
				animation_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				assets_ = LocomotionAnimationFixture::Create(context_);
			}

			~RootTimelineRuntime()
			{
				locomotion_.Shutdown();
				animation_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				assets_.reset();
				ThreadPool::instance().shutdown();
			}

			bool Start()
			{
				if(!assets_) return false;
				auto& objects = ObjectManager::instance();
				owner_ = objects.createObject("Root timeline owner");
				(void)objects.AddComponent<Transform>(owner_);
				auto* controller = objects.AddComponent<CharacterControllerComponent>(owner_);
				auto* locomotion = objects.AddComponent<CharacterLocomotionComponent>(owner_);
				auto* animator = assets_->AttachAnimator(objects, owner_);
				if(!controller || !locomotion || !animator) return false;
				controller->radius = 0.35F;
				controller->height = 1.8F;
				controller->localCenter = {0.0F, 0.9F, 0.0F};
				controller->useGravity = false;
				// The 2x loop-boundary sample is 0.25 m per fixed step (15 m/s).
				// Keep this test above that authoring rate so it measures Animation's
				// source-tick integration rather than the separate Physics speed cap.
				locomotion->maximumSpeed = 16.0;
				locomotion->acceleration = 30.0;
				locomotion->braking = 30.0;
				locomotion->animatorBinding = CharacterLocomotionComponent::kSelfBinding;
				// This fixture exercises the direct RootClip timeline, not the cooked
				// controller's idle state machine; avoid graph binding overriding it.
				animator->controllerAssetId = EMPTY_UUID;
				animator->defaultClipAssetId = assets_->RootClip().assetId.value;
				animator->skeletonOverrideAssetId = assets_->Skeleton().assetId.value;
				animator->rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
				objects.notifyObjectStructureChanged(owner_);

				animation_.SetHeadlessExecutionRequired(true);
				animation_.Initialize();
				if(!physics_.StartWorld()) return false;
				for(unsigned i = 0; i < 4U; ++i) AdvancePhysics();
				locomotion_.Initialize();
				locomotion_.ObserveProtectedBoundary();
				for(unsigned i = 0; i < 3U; ++i) {
					if(!RunAnimationFrame(animation_)) return false;
					locomotion_.ObserveProtectedBoundary();
				}
				locomotion_.Update(kStep);
				const auto handle = locomotion_.ResolvePublished(objects.getUUID(owner_));
				if(!handle) return false;
				LocomotionCommand acquire;
				acquire.kind = CommandKind::Acquire;
				acquire.handle = *handle;
				acquire.expectedConfigurationRevision = 1U;
				acquire.mode = MotionMode::RootMotion;
				acquire.producerId = 0x7401U;
				acquire.sequence = 1U;
				acquire.correlationId = 1U;
				const auto admission = locomotion_.SubmitBatch(std::span(&acquire, 1U));
				if(!admission.queued) return false;
				locomotion_.Update(kStep);
				return locomotion_.ReadReceipt(*handle, acquire.producerId, acquire.sequence,
					acquire.correlationId).state == ReceiptState::Applied;
			}

			bool Playback(AnimationCommandKind kind, bool flag, float speed, std::uint64_t sequence,
				AnimationFixedTick tick = 0)
			{
				AnimationCommand command;
				command.target = animation_.ResolveAnimatorForObject(ObjectManager::instance().getUUID(owner_));
				command.kind = kind;
				command.playbackFlag = flag;
				command.playbackSpeed = speed;
				command.playbackTick = tick;
				command.producerId = 0x7402U;
				command.producerSequence = sequence;
				command.correlationId = sequence;
				return command.target.IsValid()
					&& animation_.SubmitCommand(std::move(command)).status == AnimationStatus::Queued;
			}

			bool AnimationStep()
			{
				return RunAnimationFrame(animation_);
			}

			bool RestartPhysicsWorld()
			{
				return physics_.StopWorld() && physics_.StartWorld();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects,
					"locomotion-root-lifecycle-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			std::optional<PhysicsCharacterState> State() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::nullopt;
				const UUID id = ObjectManager::instance().getUUID(owner_);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[id](const auto& state) { return state.objectId == id; });
				return found == packet->states->end() ? std::nullopt
					: std::optional<PhysicsCharacterState>(*found);
			}

			std::vector<AnimationRootMotionApplicationResult> Results() const
			{
				std::vector<AnimationRootMotionApplicationResult> result;
				const auto animator = animation_.ResolveAnimatorForObject(
					ObjectManager::instance().getUUID(owner_));
				if(animator.IsValid())
					AnimationRootMotionResultPublisher::Instance().CopyForAnimator(animator, result);
				return result;
			}

			Object Owner() const noexcept { return owner_; }
			AnimationSystem& Animation() noexcept { return animation_; }
			LocomotionSystem& Locomotion() noexcept { return locomotion_; }

		private:
			TestContext& context_;
			PhysicsSystem& physics_ = PhysicsSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			AnimationSystem& animation_ = AnimationSystem::Instance();
			std::unique_ptr<LocomotionAnimationFixture> assets_;
			Object owner_{};
		};

		const AnimationRootMotionApplicationResult* FindAcceptedRootResult(
			const std::vector<AnimationRootMotionApplicationResult>& results,
			AnimationFixedTick begin, AnimationFixedTick end)
		{
			const auto found = std::find_if(results.begin(), results.end(), [begin, end](const auto& result) {
				return result.sourceBeginTick == begin && result.sourceEndTick == end
					&& result.disposition != AnimationRootMotionDisposition::Rejected;
			});
			return found == results.end() ? nullptr : &*found;
		}
	}

	void RegisterLocomotionRootLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.RootBinding.Lifecycle.PlaybackSpeedScalesCookedRootSamples",
			[](TestContext& context) {
				RootTimelineRuntime runtime(context);
				if(!runtime.Start()) {
					context.Expect(false, "The cooked root timeline fixture must start before playback checks.");
					return;
				}
				const auto before = runtime.State();
				context.Expect(before.has_value(), "The controller must publish a baseline Physics pose.");
				if(!before) return;
				context.Expect(runtime.Playback(AnimationCommandKind::Seek, false, 1.0F, 1U, 7)
					&& runtime.Playback(AnimationCommandKind::SetSpeed, false, 2.0F, 2U),
					"Typed seek and speed commands must enter the real Animation command queue before evaluation.");
				context.Expect(runtime.AnimationStep(), "The doubled-speed Animation frame must complete.");
				runtime.AdvancePhysics();
				const auto after = runtime.State();
				const auto results = runtime.Results();
				const auto* rootResult = FindAcceptedRootResult(results, 7, 9);
				context.Expect(after.has_value() && rootResult != nullptr && rootResult->accumulatedCycles >= 1,
					"The actual cooked clip should publish a two-source-tick root interval crossing its loop boundary at 2x speed.");
				if(after) {
					const double dx = after->pose.position.x - before->pose.position.x;
					context.Expect(dx > 0.20 && dx < 0.31,
						"The wrapped root translation applied by Physics must retain the two-tick 2x playback displacement.");
				}
			});

		registry.Add("Engine.Locomotion.RootBinding.Lifecycle.ReversePlaybackProducesReverseCookedSample",
			[](TestContext& context) {
				RootTimelineRuntime runtime(context);
				if(!runtime.Start()) {
					context.Expect(false, "The cooked root timeline fixture must start before reverse playback checks.");
					return;
				}
				context.Expect(runtime.Playback(AnimationCommandKind::Seek, false, 1.0F, 3U, 2)
					&& runtime.Playback(AnimationCommandKind::SetReverse, true, 1.0F, 4U),
					"Typed seek and reverse commands must enter the real Animation command queue.");
				context.Expect(runtime.AnimationStep(), "The reverse Animation frame must complete.");
				runtime.AdvancePhysics();
				const auto results = runtime.Results();
				const bool hasReverseSample = std::any_of(results.begin(), results.end(), [](const auto& result) {
					return result.sourceEndTick < result.sourceBeginTick
						&& result.disposition != AnimationRootMotionDisposition::Rejected;
				});
				context.Expect(hasReverseSample,
					"Reverse playback must publish the actual cooked root clip interval in decreasing source-tick order.");
				const auto state = runtime.State();
				context.Expect(state.has_value() && state->pose.position.x < 0.0,
					"Physics must realize a reverse root sample as backward translation rather than replaying forward movement.");
			});

		registry.Add("Engine.Locomotion.RootBinding.Lifecycle.WorldRestartCancelsQueuedRootSample",
			[](TestContext& context) {
				RootTimelineRuntime runtime(context);
				if(!runtime.Start()) {
					context.Expect(false, "The cooked root timeline fixture must start before world-restart checks.");
					return;
				}
				const auto beforeRestart = runtime.State();
				context.Expect(runtime.AnimationStep(),
					"Animation must queue one real root interval before the Physics world is restarted.");
				context.Expect(runtime.RestartPhysicsWorld(),
					"Physics must stop and create a new world generation for the cancellation boundary.");
				runtime.AdvancePhysics();
				const auto afterRestart = runtime.State();
				context.Expect(beforeRestart.has_value() && afterRestart.has_value()
					&& std::abs(afterRestart->pose.position.x - beforeRestart->pose.position.x) < 1.0e-5,
					"A queued root interval captured for the retired Physics world must not move the character in its replacement world.");
			});
	}
}
