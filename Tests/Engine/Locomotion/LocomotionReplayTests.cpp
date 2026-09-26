// LocomotionReplayTests.cpp - copied parameter evidence and typed ingress replay.

#include "../TestHarness.h"

#include "../Animation/AnimationAuthorityTestHelpers.h"
#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Components/AnimatorComponent.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/CharacterMotionTrace.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include "Tests/Tools/Locomotion/LocomotionReplaySystem.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;

		constexpr float kFixedStep = 1.0F / 60.0F;

		bool ExecuteAnimationFrame(AnimationSystem& animation)
		{
			auto workloads = animation.DeclareFrameWorkloads(kFixedStep);
			const auto result = TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
				std::move(workloads), "locomotion-replay-trace-test");
			return result.valid && result.completed;
		}

		class ParameterTraceRuntime final {
		public:
			explicit ParameterTraceRuntime(TestContext& context) : context_(context) {}
			~ParameterTraceRuntime()
			{
				(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
				LocomotionSystem::Instance().Shutdown();
				AnimationSystem::Instance().Shutdown();
				auto& physics = PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				assets_.reset();
				ThreadPool::instance().shutdown();
			}

			bool Start()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				LocomotionSystem::Instance().Shutdown();
				AnimationSystem::Instance().Shutdown();
				auto& physics = PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
				assets_ = LocomotionAnimationFixture::Create(context_);
				if(!assets_) return false;

				auto& objects = ObjectManager::instance();
				owner_ = objects.createObject("Parameter trace owner");
				ownerId_ = objects.getUUID(owner_);
				context_.Expect(objects.AddComponent<Transform>(owner_) != nullptr,
					"The parameter trace fixture requires a canonical owner transform.");
				auto* controller = objects.AddComponent<CharacterControllerComponent>(owner_);
				auto* movement = objects.AddComponent<CharacterLocomotionComponent>(owner_);
				context_.Expect(controller != nullptr && movement != nullptr,
					"The parameter trace owner needs live Physics and locomotion attachments.");
				if(controller != nullptr) {
					controller->radius = 0.35F;
					controller->height = 1.8F;
					controller->localCenter = {0.0F, 0.9F, 0.0F};
					controller->useGravity = false;
				}
				if(movement != nullptr) {
					movement->maximumSpeed = 4.0;
					movement->animatorBinding = CharacterLocomotionComponent::kSelfBinding;
					movement->speedParameter = "Speed";
					movement->localDirectionXParameter = "DirectionX";
					movement->localDirectionZParameter = "DirectionZ";
					movement->groundedParameter = "Grounded";
					movement->movingParameter = "Moving";
				}
				context_.Expect(assets_->AttachAnimator(objects, owner_) != nullptr,
					"The fixture must attach its resource-backed animator to the owner.");

				auto& animation = AnimationSystem::Instance();
				animation.SetHeadlessExecutionRequired(true);
				animation.Initialize();
				if(!physics.StartWorld()) return false;
				for(std::uint32_t frame = 0U; frame < 4U; ++frame) AdvancePhysics();

				auto& locomotion = LocomotionSystem::Instance();
				locomotion.Initialize();
				locomotion.ObserveProtectedBoundary();
				if(!locomotion.ResolvePublished(ownerId_)) return false;
				context_.Expect(ExecuteAnimationFrame(animation) && ExecuteAnimationFrame(animation),
					"The production Animation coordinator must publish the authored controller before parameter capture.");
				return true;
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				auto& physics = PhysicsSystem::Instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(kFixedStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects, "locomotion-parameter-trace-flush");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			UUID OwnerId() const noexcept { return ownerId_; }

		private:
			TestContext& context_;
			Object owner_{};
			UUID ownerId_ = EMPTY_UUID;
			std::unique_ptr<LocomotionAnimationFixture> assets_;
		};

		class ReplayPhysicsRuntime final {
		public:
			ReplayPhysicsRuntime()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				auto& physics = PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
				auto& objects = ObjectManager::instance();
				for(std::size_t index = 0U; index < objects_.size(); ++index) {
					objects_[index] = objects.createObject("Replay trace character");
					ids_[index] = objects.getUUID(objects_[index]);
					(void)objects.AddComponent<Transform>(objects_[index]);
					(void)objects.AddComponent<CharacterControllerComponent>(objects_[index]);
					(void)objects.AddComponent<CharacterLocomotionComponent>(objects_[index]);
				}
				started_ = physics.StartWorld();
				LocomotionSystem::Instance().Initialize();
				for(std::uint32_t frame = 0U; started_ && frame < 4U; ++frame) AdvancePhysics();
				LocomotionSystem::Instance().ObserveProtectedBoundary();
				started_ = started_ && LocomotionSystem::Instance().ResolvePublished(ids_[0]).has_value();
			}
			~ReplayPhysicsRuntime()
			{
				(void)LocomotionSystem::Instance().ConfigureParameterTrace(false);
				LocomotionSystem::Instance().Shutdown();
				(void)PhysicsSystem::Instance().ConfigureCharacterMotionTrace(false);
				(void)PhysicsSystem::Instance().StopWorld();
				PhysicsSystem::Instance().Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}
			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				auto& physics = PhysicsSystem::Instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(kFixedStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects, "locomotion-replay-physics-flush");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}
			bool Started() const noexcept { return started_; }
			std::array<UUID, 2U> ids_{};
		private:
			std::array<Object, 2U> objects_{};
			bool started_ = false;
		};

		PhysicsCharacterCommandTarget FindController(PhysicsSystem& physics, UUID objectId)
		{
			PhysicsCharacterCommandTarget target{};
			const auto packet = physics.LatestCharacterStatePacket();
			if(!packet || !packet->states) return target;
			for(const auto& state : *packet->states) if(state.objectId == objectId) {
				target.world = state.world;
				target.character = state.handle;
				target.objectId = state.objectId;
				target.objectGeneration = state.objectGeneration;
				target.componentGeneration = state.componentGeneration;
				break;
			}
			return target;
		}
	}

	void RegisterLocomotionReplayTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.Stress.Trace.ParameterBatchIsCopiedBoundedAndCursorAware",
			[](TestContext& context) {
				ParameterTraceRuntime runtime(context);
				if(!runtime.Start()) {
					context.Expect(false, "The real authored animator fixture must be ready before parameter trace checks.");
					return;
				}
				auto& locomotion = LocomotionSystem::Instance();
				context.Expect(!locomotion.CopyParameterTrace(0U).enabled,
					"Locomotion parameter tracing must be disabled by default.");
				context.Expect(!locomotion.ConfigureParameterTrace(true, 0U)
					&& !locomotion.ConfigureParameterTrace(true, 16385U),
					"Parameter trace capacities outside the documented range must be rejected.");
				context.Expect(!locomotion.CopyParameterTrace(0U).enabled,
					"Rejected capacities must preserve disabled capture state.");
				context.Expect(locomotion.ConfigureParameterTrace(true, 1U),
					"A one-record parameter trace ring must be configurable.");
				const auto cursor = locomotion.CopyParameterTrace(0U).latestSequence;
				locomotion.Update(kFixedStep);
				const auto captured = locomotion.CopyParameterTrace(cursor);
				context.Expect(captured.enabled && captured.records.size() == 1U
					&& captured.latestSequence == cursor + 1U
					&& captured.reservedBytes == sizeof(LocomotionParameterTraceRecord),
					"A real Locomotion-to-Animation admission must be captured as one bounded copied record.");
				if(captured.records.size() == 1U) {
					const auto& record = captured.records.front();
					context.Expect(record.sequence == cursor + 1U && record.updateEpoch != 0U
						&& record.animatorObjectId == runtime.OwnerId() && record.count == 5U,
						"The copied parameter record must identify its animator, update, sequence, and full authored batch.");
					bool commandsCopied = true;
					for(std::size_t index = 0U; index < record.count; ++index)
						commandsCopied = commandsCopied && record.commands[index].kind == AnimationCommandKind::SetParameter
							&& record.commands[index].target.IsValid();
					context.Expect(commandsCopied,
						"Trace commands must be copied typed parameter commands with a valid animator identity.");
				}
				context.Expect(!captured.lostRecords,
					"A cursor immediately before the sole retained record must not report loss.");
				context.Expect(locomotion.ConfigureParameterTrace(true, 2U),
					"A valid reconfiguration must replace the old retained parameter ring.");
				const auto reset = locomotion.CopyParameterTrace(cursor);
				context.Expect(reset.enabled && reset.records.empty()
					&& reset.latestSequence == captured.latestSequence
					&& reset.lostRecords,
					"Reconfiguration drops retained records, reports the stale cursor, and never rewinds sequence numbers.");
				context.Expect(locomotion.ConfigureParameterTrace(false),
					"Parameter evidence capture must be disableable after copying the record.");
				const auto disabled = locomotion.CopyParameterTrace(reset.latestSequence);
				context.Expect(!disabled.enabled && disabled.records.empty() && disabled.reservedBytes == 0U,
					"Disabling capture must release the ring and return no live payload references.");
			});

		registry.Add("Engine.Locomotion.Stress.Trace.ReplayRejectsMalformedBatchAndPublishesCurrentTypedTarget",
			[](TestContext& context) {
				using namespace TerraEngine::Locomotion::Validation;
				auto& replay = LocomotionReplaySystem::Instance();
				replay.Shutdown();
				replay.Initialize();
				std::array<CharacterMotionTraceRecord, 2U> malformed{};
				malformed[0U].batch = malformed[1U].batch = 7U;
				malformed[0U].batchIndex = 0U;
				malformed[1U].batchIndex = 1U;
				malformed[0U].batchCount = malformed[1U].batchCount = 2U;
				malformed[0U].payload = CharacterMotionLeaseChange{};
				malformed[1U].payload = CharacterMotionTarget{};
				context.Expect(!replay.QueueFrame(1U, malformed),
					"Replay must reject a multi-record batch whose members do not share one payload kind.");
				const std::span<const CharacterMotionTraceRecord> empty{};
				context.Expect(replay.QueueFrame(1U, empty),
					"A malformed rejected candidate must leave the replay slot available for a complete frame.");
				context.Expect(!replay.QueueFrame(2U, empty),
					"At most one complete replay frame may be pending at the boundary.");
				replay.Shutdown();

				ReplayPhysicsRuntime runtime;
				context.Expect(runtime.Started(),
					"A current real Physics character packet and Locomotion binding are required for replay.");
				if(!runtime.Started()) return;
				auto& physics = PhysicsSystem::Instance();
				auto& locomotion = LocomotionSystem::Instance();
				context.Expect(physics.ConfigureCharacterMotionTrace(true, 8U),
					"The replay test must capture accepted typed ingress from real Physics.");
				const auto resolved = locomotion.ResolvePublishedSnapshot(runtime.ids_[0U]);
				const auto controller = FindController(physics, runtime.ids_[0U]);
				const auto lease = physics.ReadCharacterMotionLease(controller);
				context.Expect(resolved.has_value() && lease.has_value()
					&& lease->state == CharacterMotionLeaseState::Unleased,
					"Replay admission must be based on a freshly resolved binding and unleased current controller.");
				if(!resolved || !lease) return;
				CharacterMotionLeaseChange acquire{};
				acquire.expected = *lease;
				acquire.next = *lease;
				acquire.next.owner = {resolved->handle.sceneLifetime, resolved->handle.objectId,
					resolved->handle.objectGeneration, resolved->handle.attachmentGeneration,
					resolved->handle.slotIndex, resolved->handle.slotGeneration};
				acquire.next.state = CharacterMotionLeaseState::LocomotionOwned;
				acquire.next.mode = CharacterMotionMode::Velocity;
				++acquire.next.authorityEpoch;
				++acquire.next.motionEpoch;
				++acquire.next.publicationEpoch;
				const auto leaseResult = physics.PublishCharacterMotionLeases(std::span(&acquire, 1U));
				context.Expect(leaseResult.accepted,
					"The current character must accept a real copied exclusive locomotion lease.");
				if(!leaseResult.accepted) return;
				auto activeLease = physics.ReadCharacterMotionLease(controller);
				if(!activeLease) return;
				CharacterMotionTarget target{};
				target.controller = controller;
				target.owner = activeLease->owner;
				target.authorityEpoch = activeLease->authorityEpoch;
				target.motionEpoch = activeLease->motionEpoch;
				target.locomotionUpdateEpoch = 1U;
				target.mode = activeLease->mode;
				target.frame = {kPrimaryPhysicsFrameId, controller.world.worldGeneration};
				target.planarVelocity = {0.5F, 0.0F, 0.0F};
				target.acceleration = 20.0F;
				target.braking = 20.0F;
				context.Expect(physics.PublishCharacterMotionTarget(target).Succeeded(),
					"The target chosen for replay must first pass the real protected Physics ingress.");
				const auto captured = physics.CopyCharacterMotionTrace(0U);
				const auto* source = captured.records.empty() ? nullptr : std::get_if<CharacterMotionTarget>(
					&captured.records.back().payload);
				context.Expect(source != nullptr,
					"The typed replay input must come from an actually accepted trace record.");
				if(source == nullptr) return;
				CharacterMotionTraceRecord replayRecord = captured.records.back();
				auto& replayTarget = std::get<CharacterMotionTarget>(replayRecord.payload);
				++replayTarget.locomotionUpdateEpoch;
				const std::uint64_t priorSequence = captured.latestSequence;
				replay.Initialize();
				context.Expect(replay.QueueFrame(1U, std::span(&replayRecord, 1U)),
					"Replay must admit the complete current-world typed target as one bounded frame.");
				replay.Update(0.0F);
				const auto result = replay.ReadResult();
				context.Expect(result.frame == 1U && result.accepted == 1U && result.rejected == 0U
					&& result.failure.empty(),
					"The replay boundary must publish the remapped current target through Physics ingress.");
				const auto afterReplay = physics.CopyCharacterMotionTrace(priorSequence);
				const auto* published = afterReplay.records.empty() ? nullptr
					: std::get_if<CharacterMotionTarget>(&afterReplay.records.back().payload);
				context.Expect(afterReplay.records.size() == 1U && published != nullptr
					&& published->controller == controller && published->owner == activeLease->owner
					&& published->locomotionUpdateEpoch == target.locomotionUpdateEpoch + 1U,
					"Successful replay must create a new accepted Physics trace record with the current identities and epoch.");
				replay.Shutdown();
			});
	}
}
