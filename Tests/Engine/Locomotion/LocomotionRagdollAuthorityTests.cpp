// LocomotionRagdollAuthorityTests.cpp - animation ownership red-gate regressions.

#include "../TestHarness.h"
#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Runtime/RootMotion.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		constexpr float kRagdollFixtureStep = 1.0F / 60.0F;

		class BoundRagdollFixture final {
		public:
			explicit BoundRagdollFixture(TestContext& context) : context_(context)
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				auto& objects = ObjectManager::instance();
				objects.clear();
				Scene::SceneMutationQueue::Instance().Clear();
				locomotion_.Shutdown();
				animation_.Shutdown();
				AnimationConsumerRegistry::Instance().Clear();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				assets_ = LocomotionAnimationFixture::Create(context_);
			}

			~BoundRagdollFixture()
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

			bool Start(bool withRagdollProfile = false)
			{
				if(!assets_) return false;
				auto& objects = ObjectManager::instance();
				owner_ = objects.createObject("Bound pending ragdoll controller");
				if(!objects.AddComponent<Transform>(owner_)) return false;
				auto* controller = objects.AddComponent<CharacterControllerComponent>(owner_);
				auto* movement = objects.AddComponent<CharacterLocomotionComponent>(owner_);
				auto* animatorComponent = assets_->AttachAnimator(objects, owner_);
				if(!controller || !movement || !animatorComponent) return false;
				controller->radius = 0.35F;
				controller->height = 1.8F;
				controller->localCenter = {0.0F, 0.9F, 0.0F};
				controller->useGravity = false;
				movement->animatorBinding = CharacterLocomotionComponent::kSelfBinding;
				animatorComponent->controllerAssetId = EMPTY_UUID;
				animatorComponent->defaultClipAssetId = assets_->RootClip().assetId.value;
				animatorComponent->skeletonOverrideAssetId = assets_->Skeleton().assetId.value;
				animatorComponent->rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
				if(withRagdollProfile) {
					animatorComponent->ragdollProfileOverrideAssetId = assets_->RagdollProfileId();
					validProfile_ = true;
				}
				animation_.SetHeadlessExecutionRequired(true);
				animation_.Initialize();
				if(!physics_.StartWorld()) return false;
				for(std::uint32_t index = 0U; index < 4U; ++index) AdvancePhysics();
				locomotion_.Initialize();
				locomotion_.ObserveProtectedBoundary();
				for(std::uint32_t index = 0U; index < 3U; ++index) {
					if(!AdvanceAnimation()) return false;
					locomotion_.ObserveProtectedBoundary();
				}
				locomotion_.Update(kRagdollFixtureStep);
				const auto handle = locomotion_.ResolvePublished(objects.getUUID(owner_));
				if(!handle) return false;
				handle_ = *handle;
				animator_ = animation_.ResolveAnimatorForObject(objects.getUUID(owner_));
				const auto target = Controller();
				return animator_.IsValid() && target.world.IsValid()
					&& target.character.IsValid();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kRagdollFixtureStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "bound-ragdoll-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			bool AdvanceAnimation()
			{
				auto workload = animation_.DeclareFrameWorkloads(kRagdollFixtureStep);
				const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workload), "bound-ragdoll-animation");
				return result.valid && result.completed;
			}

			Physics::PhysicsCharacterCommandTarget Controller() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return {};
				const UUID objectId = ObjectManager::instance().getUUID(owner_);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[objectId](const Physics::PhysicsCharacterState& state) {
						return state.objectId == objectId;
					});
				if(found == packet->states->end()) return {};
				return {found->world, found->handle, found->objectId,
					found->objectGeneration, found->componentGeneration};
			}

			Locomotion::CommandReceipt Acquire()
			{
				Locomotion::LocomotionCommand command;
				command.kind = Locomotion::CommandKind::Acquire;
				command.handle = handle_;
				command.expectedConfigurationRevision = 1U;
				command.expectedAuthorityEpoch = 0U;
				command.producerId = 911U;
				command.sequence = 1U;
				command.correlationId = 91101U;
				command.mode = Locomotion::MotionMode::RootMotion;
				const auto queued = locomotion_.SubmitBatch(std::span(&command, 1U));
				if(!queued.queued) return {};
				locomotion_.Update(kRagdollFixtureStep);
				return locomotion_.ReadReceipt(handle_, 911U, 1U, 91101U);
			}

			NativeAnimationRagdollReceipt QueuePendingActivate(std::uint64_t correlation)
			{
				NativeAnimationRagdollRequest request{};
				request.header = {static_cast<std::uint32_t>(sizeof(request)), kNativeAnimationAbiVersion,
					static_cast<std::uint16_t>(NativeAnimationRecordKind::RagdollRequest)};
				request.animator = {animator_.slotIndex, animator_.generation};
				request.operation = NativeAnimationRagdollOperation::Activate;
				if(validProfile_) {
					const UUID profileId = assets_->RagdollProfileId();
					std::memcpy(&request.profileAssetId.first, profileId.data(),
						sizeof(request.profileAssetId.first));
					std::memcpy(&request.profileAssetId.second,
						profileId.data() + sizeof(request.profileAssetId.first),
						sizeof(request.profileAssetId.second));
					request.profileGeneration = ResourceManager::Instance()
						.GetAnimationAssetRuntimeGeneration(profileId, AssetType::RagdollProfile);
				} else {
					request.profileAssetId = {1U, 2U};
					request.profileGeneration = 1U;
				}
				request.physicsWorldGeneration = Controller().world.worldGeneration;
				request.blendWeight = 1.0F;
				request.correlationId = correlation;
				return animation_.SubmitRagdollRequest(request);
			}

			Locomotion::LocomotionSystem& Locomotion() noexcept { return locomotion_; }
			AnimationSystem& Animation() noexcept { return animation_; }
			Physics::PhysicsSystem& Physics() noexcept { return physics_; }
			Object Owner() const noexcept { return owner_; }
			AnimatorInstanceHandle Animator() const noexcept { return animator_; }

		private:
			TestContext& context_;
			AnimationSystem& animation_ = AnimationSystem::Instance();
			Locomotion::LocomotionSystem& locomotion_ = Locomotion::LocomotionSystem::Instance();
			Physics::PhysicsSystem& physics_ = Physics::PhysicsSystem::Instance();
			std::unique_ptr<LocomotionAnimationFixture> assets_;
			bool validProfile_ = false;
			Object owner_;
			Locomotion::LocomotionHandle handle_;
			AnimatorInstanceHandle animator_;
		};
	}

	void RegisterLocomotionRagdollAuthorityTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Animation.ControllerReloadUpdatesCapturedInstanceGeneration", [](TestContext& context) {
			AnimatorInstanceStore store;
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId = GenerateUUID();
			descriptor.componentGeneration = 1U;
			descriptor.controller.assetId.value = GenerateUUID();
			descriptor.controller.generation = 5U;
			const AnimatorInstanceHandle handle = store.Create(descriptor);
			context.Expect(handle.IsValid(), "A valid controller-backed animator must acquire a live store slot.");
			if(!handle.IsValid()) return;
			std::vector<AnimationFrameInput> captured;
			store.CaptureFrameInputs(1, captured);
			context.Expect(captured.size() == 1U && captured.front().descriptor.controller.generation == 5U,
				"Worker inputs must initially copy the exact controller publication generation.");
			const std::uint64_t beforeRevision = store.MembershipRevision();
			AnimationAssetHandle next = descriptor.controller;
			next.generation = 6U;
			context.Expect(store.UpdateControllerGeneration(handle, next),
				"A successful same-controller hot reload must retain the animator handle.");
			store.CaptureFrameInputs(2, captured);
			context.Expect(captured.size() == 1U && captured.front().handle == handle
				&& captured.front().descriptor.controller == next
				&& store.MembershipRevision() > beforeRevision,
				"The next immutable worker input and membership observation must use the new generation.");
			AnimationAssetHandle other = next;
			other.assetId.value = GenerateUUID();
			context.Expect(!store.UpdateControllerGeneration(handle, other),
				"Changing the durable controller asset must require a new animator handle.");
			AnimationAssetHandle older = next;
			older.generation = 5U;
			context.Expect(!store.UpdateControllerGeneration(handle, older),
				"An older publication of the same controller must not roll the copied generation backward.");
			store.CaptureFrameInputs(3, captured);
			context.Expect(captured.size() == 1U && captured.front().descriptor.controller == next,
				"A rejected different-controller update must leave the copied descriptor intact.");
			context.Expect(store.Invalidate(handle) && !store.UpdateControllerGeneration(handle, next),
				"A retired animator must never accept a late generation update.");
		});

		registry.Add("Engine.Locomotion.Animation.RagdollSchedulerDeclaresPhysicalHandoffs", [](TestContext& context) {
			BoundRagdollFixture fixture(context);
			if(!fixture.Start()) return;
			const auto has = [](const std::vector<std::string>& resources, std::string_view name) {
				return std::any_of(resources.begin(), resources.end(), [name](const std::string& value) {
					return value == name;
				});
			};
			const auto declaration = fixture.Animation().SchedulerDeclaration();
			for(const std::string_view name : {"PhysicsWorld", "PhysicsCharacterMotionLeases",
				"RagdollPhysicsSnapshots", "RagdollPhysicsResults", "RootMotionResults"}) {
				context.Expect(has(declaration.reads, name),
					"Animation must declare each protected physical observation and result input.");
			}
			for(const std::string_view name : {"PhysicsWorld", "PhysicsCharacterMotionLeases",
				"RagdollPhysicsQueue", "RootMotionIntervals", "RootMotionResults"}) {
				context.Expect(has(declaration.writes, name),
					"Animation must declare each protected physical handoff and result acknowledgement.");
			}
			const auto workloads = fixture.Animation().DeclareFrameWorkloads(kRagdollFixtureStep);
			const auto finalizer = std::find_if(workloads.begin(), workloads.end(), [](const auto& item) {
				return item.workloadName == "FinalizePublishedAnimationAuthority";
			});
			context.Expect(finalizer != workloads.end() && finalizer->mainThreadRequired
				&& !finalizer->workerEligible,
				"Physical handoffs and result acknowledgements must remain in Animation's protected finalizer.");
			if(finalizer == workloads.end()) return;
			for(const std::string_view name : {"PhysicsWorld", "PhysicsCharacterMotionLeases",
				"RagdollPhysicsSnapshots", "RagdollPhysicsResults", "RootMotionResults"}) {
				context.Expect(has(finalizer->reads, name),
					"The protected Animation finalizer must declare physical inputs actually read by the handoff.");
			}
			for(const std::string_view name : {"PhysicsWorld", "PhysicsCharacterMotionLeases",
				"RagdollPhysicsQueue", "RootMotionIntervals", "RootMotionResults"}) {
				context.Expect(has(finalizer->writes, name),
					"The protected Animation finalizer must declare physical outputs actually published by the handoff.");
			}
		});

		registry.Add("Engine.Locomotion.Animation.RootBindingsUseImmutableControllerMapping", [](TestContext& context) {
			AnimationSystem& animation = AnimationSystem::Instance();
			auto staged = std::make_shared<AnimationRootControllerBindingSnapshot>();
			staged->publicationGeneration = 7U;
			AnimationRootControllerBinding binding;
			binding.animator = {3U, 2U};
			binding.bindingGeneration = 11U;
			binding.authorityEpoch = 5U;
			binding.motionEpoch = 13U;
			binding.mode = Physics::CharacterMotionMode::RootMotion;
			staged->active.push_back(binding);
			animation.PublishRootControllerBindings(staged);
			const auto published = animation.ReadRootControllerBindings();
			context.Expect(published && published->publicationGeneration == 7U
				&& published->active.size() == 1U
				&& published->active.front().animator == binding.animator,
				"Animation must pin the complete protected animator-to-controller mapping without a live Locomotion lookup.");
			animation.PublishRootControllerBindings({});
		});

		registry.Add("Engine.Locomotion.Animation.RootConsumerPreservesUnrelatedDeclarations", [](TestContext& context) {
			AnimationConsumerRegistry consumers;
			AnimationConsumerDeclaration gameplay;
			gameplay.consumerId = 17U;
			gameplay.kind = AnimationConsumerKind::Gameplay;
			gameplay.animator = {1U, 1U};
			gameplay.declarationGeneration = 1U;
			gameplay.declarationTick = 0;
			context.Expect(consumers.PublishDeclarations(std::span(&gameplay, 1U), 1U, 0)
				== AnimationStatus::Ok,
				"The pre-existing gameplay consumer must publish before a root-only overlay.");
			const std::array<AnimatorInstanceHandle, 1U> rootAnimators{{{2U, 1U}}};
			auto prepared = consumers.PrepareRootMotionConsumers(rootAnimators, 1U, 0);
			context.Expect(prepared.Status() == AnimationStatus::Ok,
				"A bound animator must reserve one authoritative RootMotion consumer before acquisition.");
			if(prepared.Status() == AnimationStatus::Ok)
				consumers.CommitRootMotionConsumers(std::move(prepared));
			const auto view = consumers.CaptureReadView();
			const auto existing = view.Resolve(gameplay.animator, {}, 0);
			const auto root = view.Resolve(rootAnimators.front(), {}, 0);
			context.Expect(consumers.DeclarationCount() == 1U
				&& consumers.RootMotionDeclarationCount() == 1U
				&& existing.attribution.size() == 1U
				&& root.attribution.size() == 1U
				&& (static_cast<std::uint32_t>(root.attribution.front().reasons)
					& static_cast<std::uint32_t>(AnimationConsumerReason::RootMotion)) != 0U,
				"The root-only overlay must not replace unrelated gameplay declarations or other animators' execution modes.");
		});

		registry.Add("Engine.Locomotion.Animation.ExtractionKeepsSourceAndPhysicalDurationSeparate", [](TestContext& context) {
			AnimationRootMotionExtractionRequest request;
			request.animator = {4U, 1U};
			request.sourceBeginTick = 20;
			request.sourceEndTick = 22;
			request.end.translation = {1.0F, 0.0F, 0.0F};
			request.generation = 9U;
			request.sourceGeneration = 33U;
			request.evaluationDurationSeconds = 1.0 / 60.0;
			AnimationRootMotionDelta delta;
			const AnimationStatus status = ExtractRootMotion(request, delta);
			context.Expect(status == AnimationStatus::Ok && delta.generation == 9U
				&& delta.sourceGeneration == 33U
				&& std::abs(delta.evaluationDurationSeconds - 1.0 / 60.0) < 1.0e-9,
				"A two-tick speed-scaled clip advance must retain its one-frame physical evaluation duration and independent source generation.");
		});

		registry.Add("Engine.Locomotion.Animation.PendingRagdollActivateExcludesBeforeEvaluation", [](TestContext& context) {
			AnimationSystem& animation = AnimationSystem::Instance();
			animation.Shutdown();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			animation.Initialize();
			const Object owner = objects.createObject("Pending ragdoll owner");
			AnimatorInstanceDescriptor descriptor;
			descriptor.ownerObjectId = objects.getUUID(owner);
			descriptor.componentGeneration = 1U;
			const AnimatorInstanceHandle animator = animation.CreateAnimatorInstance(descriptor);
			if(!animator.IsValid()) {
				context.Expect(false, "The fixture must create a live animator for pending ragdoll admission.");
				animation.Shutdown();
				objects.clear();
				return;
			}
			NativeAnimationRagdollRequest activate{};
			activate.header = {
				static_cast<std::uint32_t>(sizeof(activate)), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::RagdollRequest)};
			activate.animator = {animator.slotIndex, animator.generation};
			activate.operation = NativeAnimationRagdollOperation::Activate;
			activate.profileAssetId = {1U, 2U};
			activate.profileGeneration = 1U;
			activate.physicsWorldGeneration = 1U;
			activate.blendWeight = 1.0F;
			activate.correlationId = 91U;
			const auto receipt = animation.SubmitRagdollRequest(activate);
			const auto excluded = animation.ReadRagdollExclusions();
			const bool pending = excluded && std::any_of(excluded->entries.begin(), excluded->entries.end(),
				[animator](const AnimationRagdollExclusion& entry) {
					return entry.animator == animator
						&& entry.state == AnimationRagdollExclusionState::PendingActivate
						&& entry.correlationId == 91U;
				});
			context.Expect(receipt.status == NativeAnimationStatus::Queued && pending,
				"Accepted ragdoll Activate must exclude movement before Animation evaluation or root publication.");
			animation.Shutdown();
			objects.clear();
		});

		registry.Add("Engine.Locomotion.Animation.RootTickEvidenceIsBoundedBeyond256Ticks", [](TestContext& context) {
			auto& publisher = AnimationRootMotionResultPublisher::Instance();
			publisher.Clear();
			const Physics::PhysicsWorldHandle world{{123U}, 1U};
			const Physics::RootMotionIntervalId interval{world, 1U};
			AnimationRootMotionApplicationResult pending;
			pending.animator = {91U, 1U};
			pending.producerId = 1U;
			pending.producerSequence = 1U;
			pending.idempotencyKey = 1U;
			pending.correlationId = 1U;
			context.Expect(publisher.RegisterRootMotionInterval(interval, pending),
				"The bounded ledger fixture must reserve one live physical interval.");
			for(std::uint64_t sequence = 1U; sequence <= 300U; ++sequence) {
				Physics::RootMotionTickResult tick;
				tick.tick = sequence;
				tick.completedStepDuration = 0.0001F;
				tick.requestedTranslation.x = 0.01F;
				tick.realizedTranslation.x = 0.01F;
				tick.contributingIntervals[0U] = interval;
				tick.contributingCount = 1U;
				Physics::RootMotionResultPacket packet;
				packet.world = world;
				packet.records.push_back({sequence, tick});
				context.Expect(publisher.ObserveRootMotionResultPacket(packet),
					"Each copied tiny-step Physics result must be accepted by the existing Animation ledger.");
				if(sequence == 150U) {
					context.Expect(publisher.ObserveRootMotionResultPacket(packet),
						"A repeated copied page may be retried after a partial acknowledgement.");
				}
			}
			context.Expect(publisher.RetainedRootMotionTickEvidenceCount() == 256U,
				"A legal 300-tick interval must retain no more than 256 whole-tick samples per scene.");
			std::array<AnimationRootMotionTickEvidence, 256U> evidence{};
			bool omitted = false;
			const std::size_t copied = publisher.CopyRootMotionTickEvidence(world, 0U, evidence, omitted);
			context.Expect(copied == 256U && omitted
				&& evidence.front().resultSequence == 45U
				&& evidence[copied - 1U].resultSequence == 300U,
				"The fixed ring must report omitted early evidence and preserve its latest 256 exact tick records.");
			Physics::RootMotionTerminalResult terminal;
			terminal.intervalId = interval;
			terminal.disposition = Physics::RootMotionTerminalDisposition::Consumed;
			terminal.finalTick = 300U;
			terminal.firstTickResultSequence = 1U;
			terminal.lastTickResultSequence = 300U;
			Physics::RootMotionResultPacket finalPacket;
			finalPacket.world = world;
			finalPacket.records.push_back({301U, terminal});
			context.Expect(publisher.ObserveRootMotionResultPacket(finalPacket),
				"The terminal packet must resolve the retained interval after its full physical duration.");
			std::vector<AnimationRootMotionApplicationResult> results;
			publisher.CopyForAnimator(pending.animator, results);
			context.Expect(results.size() == 1U
				&& results.front().observedPhysicalTickCount == 300U
				&& std::abs(results.front().aggregateRealizedTranslation.x - 3.0F) < 0.001F
				&& results.front().tickEvidenceExpired,
				"Exact aggregate and tick count must survive ring eviction without replay double-counting.");
			publisher.Clear();
		});

		registry.Add("Engine.Locomotion.Animation.PendingRagdollBeforeAcquirePhysicallyExcludes", [](TestContext& context) {
			BoundRagdollFixture fixture(context);
			if(!fixture.Start()) {
				context.Expect(false, "The bound ragdoll fixture must publish a ready controller and animator.");
				return;
			}
			const auto accepted = fixture.QueuePendingActivate(9201U);
			context.Expect(accepted.status == NativeAnimationStatus::Queued,
				"A pending Activate must enter the immutable exclusion snapshot before Locomotion Publish.");
			fixture.Locomotion().Update(kRagdollFixtureStep);
			const auto lease = fixture.Physics().ReadCharacterMotionLease(fixture.Controller());
			context.Expect(lease && lease->state == Physics::CharacterMotionLeaseState::ExternalExclusive
				&& lease->externalOwnerId == RagdollExternalOwnerId(fixture.Animator()),
				"Locomotion must physically hand an idle bound controller to pending ragdoll exclusion before command admission.");
			const auto acquired = fixture.Acquire();
			context.Expect(acquired.state != Locomotion::ReceiptState::Applied,
				"A subsequent locomotion Acquire cannot steal a pending ragdoll's exclusive controller.");
		});

		registry.Add("Engine.Locomotion.Animation.PendingRagdollAfterAcquireRevokesRootLease", [](TestContext& context) {
			BoundRagdollFixture fixture(context);
			if(!fixture.Start()) {
				context.Expect(false, "The bound ragdoll fixture must publish a ready controller and animator.");
				return;
			}
			const auto acquired = fixture.Acquire();
			const auto previous = fixture.Physics().ReadCharacterMotionLease(fixture.Controller());
			context.Expect(acquired.state == Locomotion::ReceiptState::Applied
				&& previous && previous->state == Physics::CharacterMotionLeaseState::LocomotionOwned,
				"The fixture must own a live root-mode physical lease before the late Activate.");
			if(!previous || acquired.state != Locomotion::ReceiptState::Applied) return;
			const auto accepted = fixture.QueuePendingActivate(9202U);
			context.Expect(accepted.status == NativeAnimationStatus::Queued,
				"A late Activate must be accepted after the earlier locomotion Publish.");
			context.Expect(fixture.AdvanceAnimation(),
				"Animation's protected boundary must complete while the pending exclusion is current.");
			const auto exclusive = fixture.Physics().ReadCharacterMotionLease(fixture.Controller());
			context.Expect(exclusive && exclusive->state == Physics::CharacterMotionLeaseState::ExternalExclusive
				&& exclusive->externalOwnerId == RagdollExternalOwnerId(fixture.Animator()),
				"Animation must physically revoke the old root owner before attempting ragdoll Activate.");
			Physics::RootMotionInterval replay;
			replay.lease = *previous;
			replay.frame = {Physics::kPrimaryPhysicsFrameId, previous->controller.world.worldGeneration};
			replay.animator.slotIndex = fixture.Animator().slotIndex;
			replay.animator.animatorGeneration = fixture.Animator().generation;
			replay.animator.poseGeneration = 1U;
			replay.animator.sourceGeneration = 1U;
			replay.animator.sourceStateId = 1U;
			replay.animator.evaluationSequence = 1U;
			replay.animator.bindingGeneration = 1U;
			replay.animator.publicationGeneration = 1U;
			replay.animator.sourceEndTick = 1;
			replay.ledger.producerId = 1U;
			replay.ledger.producerSequence = 1U;
			replay.ledger.idempotencyKey = 1U;
			replay.ledger.correlationId = 1U;
			replay.durationSeconds = kRagdollFixtureStep;
			replay.maximumSpeed = 1.0F;
			replay.maximumTurnRate = 1.0F;
			replay.frameTranslation.x = 0.01F;
			replay.localTranslation.x = 0.01F;
			context.Expect(!fixture.Physics().ReserveRootMotionInterval(replay).accepted,
				"The old root owner must not publish another interval after external handoff.");
			// A real deferred component removal also advances the object's component
			// generation and forces Physics to replace its controller target.
			Scene::SceneMutationQueue removal;
			Scene::SceneMutationRequest request;
			request.requestId = "remove-bound-locomotion";
			request.operation = Scene::SceneMutationOperation::RemoveComponent;
			request.targetObjectId = ObjectManager::instance().getUUID(fixture.Owner());
			request.componentName = std::string(CharacterLocomotionComponent::kComponentName);
			request.componentPayloadKind = Scene::SceneComponentMutationPayloadKind::Remove;
			removal.Enqueue(std::move(request));
			const auto removed = removal.Apply(ObjectManager::instance());
			context.Expect(removed.applied && !ObjectManager::instance().GetComponent<CharacterLocomotionComponent>(fixture.Owner()),
				"The protected transaction must remove Locomotion before controller replacement.");
			if(!removed.applied) return;
			fixture.Physics().OnAfterPhysicsFlush(removed.transactionResults, removed.publicationGroupResults);
			fixture.AdvancePhysics();
			fixture.Locomotion().ObserveProtectedBoundary();
			fixture.Locomotion().Update(kRagdollFixtureStep);
			const auto replacement = fixture.Controller();
			context.Expect(replacement.world.IsValid() && replacement.character.IsValid()
				&& replacement != previous->controller,
				"Component-generation replacement must create a new full controller target identity.");
			context.Expect(!fixture.Physics().ReadCharacterMotionLease(previous->controller),
				"The pre-removal controller target must no longer be live.");
			const auto retained = fixture.Physics().ReadCharacterMotionLease(replacement);
			context.Expect(retained && retained->state == Physics::CharacterMotionLeaseState::ExternalExclusive,
				"Removing Locomotion must carry the independent ragdoll's exclusion to the replacement controller.");
		});
	}
}
