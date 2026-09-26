// LocomotionRootBindingTests.cpp - live root-animator ownership boundaries.

#include "../TestHarness.h"

#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Runtime/RootMotion.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionAnimationBindings.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string_view>
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
			const auto result = Scheduling::ExecuteFrameWorkloadsCompatibility(
				std::move(workloads), "locomotion-root-binding");
			return result.valid && result.completed;
		}

		class RootBindingRuntime final {
		public:
			explicit RootBindingRuntime(TestContext& context) : context_(context)
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

			~RootBindingRuntime()
			{
				locomotion_.Shutdown();
				animation_.Shutdown();
				auto& consumers = AnimationConsumerRegistry::Instance();
				consumers.Clear();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				assets_.reset();
				ThreadPool::instance().shutdown();
			}

			Object AddCharacter(std::string_view name, bool attachAnimator,
				bool validRootClip = true, bool lockedRotation = false)
			{
				auto& objects = ObjectManager::instance();
				const Object object = objects.createObject(std::string(name));
				auto* transform = objects.AddComponent<Transform>(object);
				auto* controller = objects.AddComponent<CharacterControllerComponent>(object);
				auto* movement = objects.AddComponent<CharacterLocomotionComponent>(object);
				context_.Expect(transform != nullptr && controller != nullptr && movement != nullptr,
					"The root binding fixture needs a transform, physical controller, and locomotion component.");
				if(controller != nullptr) {
					controller->radius = 0.35F;
					controller->height = 1.8F;
					controller->localCenter = {0.0F, 0.9F, 0.0F};
					controller->useGravity = false;
					if(lockedRotation) controller->rotationPolicy = PhysicsCharacterRotationPolicy::Locked;
				}
				if(movement != nullptr) {
					movement->maximumSpeed = 4.0;
					movement->acceleration = 30.0;
					movement->braking = 30.0;
					movement->animatorBinding = attachAnimator
						? CharacterLocomotionComponent::kSelfBinding
						: CharacterLocomotionComponent::kNoneBinding;
				}
				if(attachAnimator) AttachRootAnimator(object, validRootClip);
				return object;
			}

			Object AddDescendantAnimator(Object owner, bool validRootClip = true)
			{
				auto& objects = ObjectManager::instance();
				const Object child = objects.createObject("Root animator descendant");
				(void)objects.AddComponent<Transform>(child);
				context_.Expect(objects.setParent(child, owner), "The descendant animator must be parented under its locomotion owner.");
				AttachRootAnimator(child, validRootClip);
				if(auto* movement = objects.GetComponent<CharacterLocomotionComponent>(owner)) {
					movement->animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
					movement->animatorObjectId = objects.getUUID(child);
				}
				return child;
			}

			void AttachRootAnimator(Object object, bool validRootClip)
			{
				auto* animator = assets_->AttachAnimator(ObjectManager::instance(), object);
				context_.Expect(animator != nullptr, "The cooked root-motion fixture must attach an AnimatorComponent.");
				if(animator != nullptr) {
					animator->controllerAssetId = EMPTY_UUID;
					animator->defaultClipAssetId = validRootClip ? assets_->RootClip().assetId.value : EMPTY_UUID;
					animator->skeletonOverrideAssetId = assets_->Skeleton().assetId.value;
					animator->rootMotionPolicy = AnimatorRootMotionPolicy::ExtractOnly;
				}
			}

			bool Start()
			{
				if(!assets_) return false;
				animation_.SetHeadlessExecutionRequired(true);
				animation_.Initialize();
				if(!physics_.StartWorld()) {
					context_.Expect(false, "The root binding test needs a running Physics world.");
					return false;
				}
				for(std::uint32_t i = 0U; i < 4U; ++i) AdvancePhysics();
				locomotion_.Initialize();
				locomotion_.ObserveProtectedBoundary();
				for(std::uint32_t i = 0U; i < 3U; ++i) {
					if(!ExecuteAnimationFrame(animation_)) {
						context_.Expect(false, "Animation binding frames must complete for the cooked root fixture.");
						return false;
					}
					locomotion_.ObserveProtectedBoundary();
				}
				locomotion_.Update(kFixedStep);
				return true;
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kFixedStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects,
					"locomotion-root-binding-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			bool RefreshBindings()
			{
				if(!ExecuteAnimationFrame(animation_)) return false;
				locomotion_.ObserveProtectedBoundary();
				locomotion_.Update(kFixedStep);
				return true;
			}

			std::optional<LocomotionHandle> Handle(Object object) const
			{
				return locomotion_.ResolvePublished(ObjectManager::instance().getUUID(object));
			}

			CapturedLocomotionAnimator CaptureAnimator(Object owner) const
			{
				const auto handle = Handle(owner);
				const auto* movement = ObjectManager::instance().GetComponent<CharacterLocomotionComponent>(owner);
				return handle && movement ? CaptureLocomotionAnimator(*handle, *movement) : CapturedLocomotionAnimator{};
			}

			PhysicsCharacterCommandTarget Controller(Object object) const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return {};
				const UUID objectId = ObjectManager::instance().getUUID(object);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[objectId](const auto& state) { return state.objectId == objectId; });
				if(found == packet->states->end()) return {};
				return {found->world, found->handle, found->objectId,
					found->objectGeneration, found->componentGeneration};
			}

			std::optional<PhysicsCharacterState> State(Object object) const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::nullopt;
				const UUID objectId = ObjectManager::instance().getUUID(object);
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[objectId](const auto& state) { return state.objectId == objectId; });
				return found == packet->states->end() ? std::nullopt : std::optional<PhysicsCharacterState>(*found);
			}

			CommandReceipt Submit(Object object, CommandKind kind, std::uint64_t producer,
				std::uint64_t sequence, std::uint64_t epoch, std::uint64_t token = 0U,
				MotionMode mode = MotionMode::Velocity)
			{
				const auto handle = Handle(object);
				if(!handle) return {};
				LocomotionCommand command;
				command.kind = kind;
				command.handle = *handle;
				command.expectedAuthorityEpoch = epoch;
				command.expectedConfigurationRevision = 1U;
				command.ownerToken = token;
				command.producerId = producer;
				command.sequence = sequence;
				command.correlationId = producer * 100U + sequence;
				command.mode = mode;
				const auto admission = locomotion_.SubmitBatch(std::span(&command, 1U));
				if(!admission.queued || admission.receipts.empty()) return {};
				locomotion_.Update(kFixedStep);
				return locomotion_.ReadReceipt(*handle, producer, sequence, command.correlationId);
			}

			const AnimationRootControllerBinding* RootBinding(
				AnimatorInstanceHandle animator, std::shared_ptr<const AnimationRootControllerBindingSnapshot>& pin) const
			{
				pin = animation_.ReadRootControllerBindings();
				if(!pin) return nullptr;
				const auto found = std::find_if(pin->active.begin(), pin->active.end(),
					[animator](const auto& row) { return row.animator == animator; });
				return found == pin->active.end() ? nullptr : &*found;
			}

			LocomotionSystem& Locomotion() noexcept { return locomotion_; }
			AnimationSystem& Animation() noexcept { return animation_; }
			LocomotionAnimationFixture& Assets() noexcept { return *assets_; }

		private:
			TestContext& context_;
			PhysicsSystem& physics_ = PhysicsSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			AnimationSystem& animation_ = AnimationSystem::Instance();
			std::unique_ptr<LocomotionAnimationFixture> assets_;
		};

		bool HasOwnedLease(RootBindingRuntime& runtime, Object object, CharacterMotionMode mode)
		{
			const auto lease = PhysicsSystem::Instance().ReadCharacterMotionLease(runtime.Controller(object));
			return lease && lease->state == CharacterMotionLeaseState::LocomotionOwned
				&& lease->mode == mode && lease->owner.IsValid();
		}
	}

	void RegisterLocomotionRootBindingTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.RootBinding.SelfAnimatorAcquiresAndStopsWithoutRelease", [](TestContext& context) {
			RootBindingRuntime runtime(context);
			const Object owner = runtime.AddCharacter("Self root owner", true);
			if(!runtime.Start()) return;
			const auto handle = runtime.Handle(owner);
			const UUID ownerId = ObjectManager::instance().getUUID(owner);
			const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(ownerId);
			const auto captured = runtime.CaptureAnimator(owner);
			context.Expect(handle.has_value() && animator.IsValid(),
				"A live Self-bound AnimatorComponent must publish both locomotion and Animation identities.");
			context.Expect(captured.animator == animator && captured.rootReady
				&& captured.reason == LocomotionReason::None,
				"The Self-bound animator must have a ready root-motion clip before acquisition is attempted.");
			if(!handle || !animator.IsValid()) return;

			const auto acquired = runtime.Submit(owner, CommandKind::Acquire, 901U, 1U, 0U,
				0U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"A root-ready Self animator and unlocked character must admit root-mode acquisition.");
			if(acquired.state != ReceiptState::Applied) return;
			std::shared_ptr<const AnimationRootControllerBindingSnapshot> bindingPin;
			const auto* binding = runtime.RootBinding(animator, bindingPin);
			context.Expect(binding != nullptr && binding->locomotionOwned && binding->rootMotionEnabled
				&& binding->mode == CharacterMotionMode::RootMotion
				&& AnimationConsumerRegistry::Instance().RootMotionDeclarationCount() == 1U
				&& HasOwnedLease(runtime, owner, CharacterMotionMode::RootMotion),
				"Root acquisition must bind exactly this animator as the root consumer and publish its Physics lease.");

			const auto stopped = runtime.Submit(owner, CommandKind::Stop, 901U, 2U,
				acquired.authorityEpoch, acquired.ownerToken, MotionMode::RootMotion);
			context.Expect(stopped.state == ReceiptState::Applied && stopped.ownerToken == acquired.ownerToken,
				"Stop must retain the original motion owner.");
			binding = runtime.RootBinding(animator, bindingPin);
			context.Expect(binding != nullptr && binding->locomotionOwned && !binding->rootMotionEnabled
				&& AnimationConsumerRegistry::Instance().RootMotionDeclarationCount() == 0U
				&& HasOwnedLease(runtime, owner, CharacterMotionMode::RootMotion),
				"Stop must disable root consumption while retaining the owned root-mode lease.");

			const auto velocity = runtime.Submit(owner, CommandKind::SetMode, 901U, 3U,
				stopped.authorityEpoch, stopped.ownerToken, MotionMode::Velocity);
			binding = runtime.RootBinding(animator, bindingPin);
			context.Expect(velocity.state == ReceiptState::Applied && velocity.ownerToken == acquired.ownerToken
				&& binding != nullptr && binding->locomotionOwned && !binding->rootMotionEnabled
				&& binding->mode == CharacterMotionMode::Velocity
				&& AnimationConsumerRegistry::Instance().RootMotionDeclarationCount() == 0U
				&& HasOwnedLease(runtime, owner, CharacterMotionMode::Velocity),
				"Switching to velocity must retain manual ownership and remove only root-motion consumption.");
		});

		registry.Add("Engine.Locomotion.RootBinding.DescendantAnimatorCanOwnRootMotion", [](TestContext& context) {
			RootBindingRuntime runtime(context);
			auto& objects = ObjectManager::instance();
			const Object owner = runtime.AddCharacter("Descendant root owner", false);
			constexpr float ownerYaw = 0.7F;
			if(auto* ownerTransform = objects.GetComponent<Transform>(owner)) {
				ownerTransform->worldPose.rotation = glm::quat(std::cos(ownerYaw * 0.5F), 0.0F,
					std::sin(ownerYaw * 0.5F), 0.0F);
				objects.MarkObjectTransformDirty(owner);
			}
			const Object child = runtime.AddDescendantAnimator(owner);
			constexpr glm::dvec3 expectedLocalOffset{0.25, 1.0, -0.5};
			if(auto* childTransform = objects.GetComponent<Transform>(child)) {
				childTransform->parentRelativePose.translationMetres = expectedLocalOffset;
				if(const auto* ownerTransform = objects.GetComponent<Transform>(owner)) {
					const glm::vec3 boundedLocal(expectedLocalOffset);
					const glm::dvec3 worldOffset(ownerTransform->worldPose.rotation * boundedLocal);
					childTransform->worldPose.position = ownerTransform->worldPose.position;
					childTransform->worldPose.position.offsetMetres += worldOffset;
					childTransform->worldPose.rotation = ownerTransform->worldPose.rotation
						* childTransform->parentRelativePose.rotation;
					objects.MarkObjectTransformDirty(child);
				}
			}
			if(!runtime.Start()) return;
			const auto handle = runtime.Handle(owner);
			const UUID childId = ObjectManager::instance().getUUID(child);
			const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(childId);
			const auto captured = runtime.CaptureAnimator(owner);
			context.Expect(handle.has_value() && animator.IsValid(),
				"A same-scene descendant animator must publish a live generation-qualified identity.");
			context.Expect(captured.animator == animator && captured.objectId == childId && captured.rootReady,
				"The descendant animator's compatible root clip must be ready before root ownership is requested.");
			context.Expect(std::abs(captured.relativeTranslation.x - expectedLocalOffset.x) < 0.001F
				&& std::abs(captured.relativeTranslation.y - expectedLocalOffset.y) < 0.001F
				&& std::abs(captured.relativeTranslation.z - expectedLocalOffset.z) < 0.001F,
				"A rotated controller must capture the descendant's nonzero translation in controller-local coordinates.");
			if(!handle || !animator.IsValid()) return;
			const auto acquired = runtime.Submit(owner, CommandKind::Acquire, 902U, 1U, 0U,
				0U, MotionMode::RootMotion);
			std::shared_ptr<const AnimationRootControllerBindingSnapshot> pin;
			const auto* binding = runtime.RootBinding(animator, pin);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U
				&& binding != nullptr && binding->controller.character == runtime.Controller(owner).character
				&& binding->animatorObjectId == childId && binding->rootMotionEnabled
				&& std::abs(binding->capturedRelativeTranslation.x - expectedLocalOffset.x) < 0.001F
				&& std::abs(binding->capturedRelativeTranslation.y - expectedLocalOffset.y) < 0.001F
				&& std::abs(binding->capturedRelativeTranslation.z - expectedLocalOffset.z) < 0.001F
				&& AnimationConsumerRegistry::Instance().RootMotionDeclarationCount() == 1U,
				"The descendant root consumer must resolve to its owning controller, retain its copied local offset, and be the only active root consumer.");
			if(binding == nullptr || !binding->bindingValidityGuard) return;
			const auto validityGuard = binding->bindingValidityGuard;
			context.Expect(validityGuard->Validate() == CharacterMotionValidityReason::Valid,
				"An unchanged descendant binding must pass its copied validity guard.");

			if(auto* childTransform = objects.GetComponent<Transform>(child)) {
				childTransform->parentRelativePose.translationMetres.x += 0.125;
				if(const auto* ownerTransform = objects.GetComponent<Transform>(owner)) {
					const glm::vec3 boundedLocal(childTransform->parentRelativePose.translationMetres);
					const glm::dvec3 worldOffset(ownerTransform->worldPose.rotation * boundedLocal);
					childTransform->worldPose.position = ownerTransform->worldPose.position;
					childTransform->worldPose.position.offsetMetres += worldOffset;
					objects.MarkObjectTransformDirty(child);
				}
			}
			context.Expect(validityGuard->Validate() != CharacterMotionValidityReason::Valid,
				"Changing the descendant's local offset must invalidate the captured root binding.");

			const Object outside = objects.createObject("Reparented descendant target");
			(void)objects.AddComponent<Transform>(outside);
			context.Expect(objects.setParent(child, outside),
				"The descendant must be reparented outside the controller hierarchy for the stale-binding check.");
			context.Expect(validityGuard->Validate() != CharacterMotionValidityReason::Valid,
				"Reparenting the animator outside its controller hierarchy must invalidate the captured root binding.");
		});

		registry.Add("Engine.Locomotion.RootBinding.RootClipMovesOwnerOnceWithoutMutatingDescendant", [](TestContext& context) {
			RootBindingRuntime runtime(context);
			const Object owner = runtime.AddCharacter("Automatic root-motion owner", false);
			const Object child = runtime.AddDescendantAnimator(owner);
			if(auto* movement = ObjectManager::instance().GetComponent<CharacterLocomotionComponent>(owner))
				movement->maximumSpeed = 8.0;
			if(!runtime.Start()) return;

			const UUID childId = ObjectManager::instance().getUUID(child);
			const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(childId);
			const auto captured = runtime.CaptureAnimator(owner);
			context.Expect(animator.IsValid() && captured.rootReady && captured.animator == animator,
				"The cooked descendant RootClip must be ready before automatic root motion is acquired.");
			if(!animator.IsValid() || !captured.rootReady) return;

			const auto acquired = runtime.Submit(owner, CommandKind::Acquire, 908U, 1U, 0U,
				0U, MotionMode::RootMotion);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The root-ready descendant must acquire root-motion ownership before an Animation frame runs.");
			if(acquired.state != ReceiptState::Applied) return;
			const auto before = runtime.State(owner);
			const auto* childTransform = ObjectManager::instance().GetComponent<Transform>(child);
			context.Expect(before.has_value() && childTransform != nullptr,
				"The test needs an initial Physics state and the descendant's local transform.");
			if(!before || childTransform == nullptr) return;
			const auto childLocalBefore = childTransform->parentRelativePose;

			context.Expect(ExecuteAnimationFrame(runtime.Animation()),
				"One fixed Animation frame must publish the cooked root sample.");
			runtime.AdvancePhysics();
			const auto applied = runtime.State(owner);
			const auto* childAfter = ObjectManager::instance().GetComponent<Transform>(child);
			context.Expect(applied.has_value() && childAfter != nullptr,
				"The first Physics step must publish the owner state and preserve the descendant component.");
			if(!applied || childAfter == nullptr) return;

			const double movedX = applied->pose.position.x - before->pose.position.x;
			const float movedYaw = applied->realizedYawRadians;
			if(!(movedX > 0.07 && movedX < 0.19 && movedYaw > 0.02F && movedYaw < 0.07F)) {
				std::vector<AnimationRootMotionApplicationResult> rootResults;
				AnimationRootMotionResultPublisher::Instance().CopyForAnimator(animator, rootResults);
				std::ostringstream diagnostic;
				diagnostic << "Expected one approximately 0.125 m / 0.04375 rad RootClip step; before={x:"
					<< before->pose.position.x << ",tick:" << before->tick << "}, after={x:"
					<< applied->pose.position.x << ",tick:" << applied->tick << ",dx:" << movedX
					<< ",requestedYaw:" << applied->requestedYawRadians << ",realizedYaw:" << movedYaw
					<< "}, rootResults=" << rootResults.size();
				const std::size_t first = rootResults.size() > 3U ? rootResults.size() - 3U : 0U;
				for(std::size_t index = first; index < rootResults.size(); ++index) {
					const auto& result = rootResults[index];
					diagnostic << " [mode:" << static_cast<unsigned>(result.applicationMode)
						<< ",status:" << static_cast<unsigned>(result.status)
						<< ",disposition:" << static_cast<unsigned>(result.disposition)
						<< ",source:" << result.sourceBeginTick << "->" << result.sourceEndTick
						<< ",request:{" << result.requestedTranslation.x << ','
						<< result.requestedTranslation.y << ',' << result.requestedTranslation.z
						<< ",yaw:" << result.requestedYawRadians << "},intervalSeq:"
						<< result.rootIntervalId.sequence << ']';
				}
				context.Expect(false, diagnostic.str());
			}
			const auto& childLocalAfter = childAfter->parentRelativePose;
			const auto& rotationAfter = childLocalAfter.rotation;
			const auto& rotationBefore = childLocalBefore.rotation;
			const float localRotationDot = rotationAfter.w * rotationBefore.w
				+ rotationAfter.x * rotationBefore.x + rotationAfter.y * rotationBefore.y
				+ rotationAfter.z * rotationBefore.z;
			context.Expect(std::abs(childLocalAfter.translationMetres.x - childLocalBefore.translationMetres.x) < 1.0e-6
				&& std::abs(childLocalAfter.translationMetres.y - childLocalBefore.translationMetres.y) < 1.0e-6
				&& std::abs(childLocalAfter.translationMetres.z - childLocalBefore.translationMetres.z) < 1.0e-6
				&& std::abs(localRotationDot) > 0.99999F,
				"Root extraction must move and turn the owning character without independently mutating its descendant Animator transform.");

			// No new Animation evaluation occurs here, so this uncovered Physics tick
			// must not replay the already-consumed root interval.
			runtime.AdvancePhysics();
			const auto quiet = runtime.State(owner);
			context.Expect(quiet.has_value()
				&& std::abs(quiet->pose.position.x - applied->pose.position.x) < 0.002
				&& std::abs(quiet->realizedYawRadians) < 0.001F,
				"A later Physics-only step must not replay the consumed root translation or yaw.");
		});

		registry.Add("Engine.Locomotion.RootBinding.MissingLockedAndReparentedAnimatorsFailClosed", [](TestContext& context) {
			{
				RootBindingRuntime runtime(context);
				const Object owner = runtime.AddCharacter("Missing root source", true, false);
				if(!runtime.Start()) return;
				const auto result = runtime.Submit(owner, CommandKind::Acquire, 903U, 1U, 0U,
					0U, MotionMode::RootMotion);
				context.Expect(result.state == ReceiptState::Rejected
					&& result.reason == LocomotionReason::AnimationUnavailable
					&& result.ownerToken == 0U && !HasOwnedLease(runtime, owner, CharacterMotionMode::RootMotion),
					"A missing direct root clip must reject acquisition without publishing owner or Physics lease.");
			}
			{
				RootBindingRuntime runtime(context);
				const Object owner = runtime.AddCharacter("Locked root controller", true, true, true);
				if(!runtime.Start()) return;
				const auto result = runtime.Submit(owner, CommandKind::Acquire, 904U, 1U, 0U,
					0U, MotionMode::RootMotion);
				context.Expect(result.state == ReceiptState::Rejected
					&& result.reason == LocomotionReason::UnsupportedController && result.ownerToken == 0U
					&& !HasOwnedLease(runtime, owner, CharacterMotionMode::RootMotion),
					"A rotation-locked controller cannot acquire root yaw authority.");
			}
			{
				RootBindingRuntime runtime(context);
				const Object owner = runtime.AddCharacter("Reparented root owner", false);
				const Object child = runtime.AddDescendantAnimator(owner);
				if(!runtime.Start()) return;
				const Object outside = ObjectManager::instance().createObject("External animator parent");
				(void)ObjectManager::instance().AddComponent<Transform>(outside);
				context.Expect(ObjectManager::instance().setParent(child, outside),
					"The test must move the named animator outside the owning hierarchy.");
				context.Expect(runtime.RefreshBindings(), "The protected binding refresh must complete after reparenting.");
				const auto result = runtime.Submit(owner, CommandKind::Acquire, 905U, 1U, 0U,
					0U, MotionMode::RootMotion);
				context.Expect(result.state == ReceiptState::Rejected
					&& result.ownerToken == 0U && !HasOwnedLease(runtime, owner, CharacterMotionMode::RootMotion),
					"A reparented-out descendant animator must reject root acquisition without ownership.");
			}
		});

		registry.Add("Engine.Locomotion.RootBinding.SharedAncestorAnimatorRejectsSecondOwner", [](TestContext& context) {
			RootBindingRuntime runtime(context);
			auto& objects = ObjectManager::instance();
			const Object outer = runtime.AddCharacter("Outer locomotion owner", false);
			const Object inner = runtime.AddCharacter("Inner locomotion owner and animator", true);
			context.Expect(objects.setParent(inner, outer), "The second locomotion controller must be nested below the first.");
			if(auto* outerMovement = objects.GetComponent<CharacterLocomotionComponent>(outer)) {
				outerMovement->animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
				outerMovement->animatorObjectId = objects.getUUID(inner);
			}
			if(auto* transform = objects.GetComponent<Transform>(inner))
				transform->worldPose.position.offsetMetres.x = 3.0;
			objects.MarkObjectTransformDirty(inner);
			if(!runtime.Start()) return;
			const auto firstHandle = runtime.Handle(outer);
			const auto secondHandle = runtime.Handle(inner);
			const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(objects.getUUID(inner));
			context.Expect(firstHandle.has_value() && secondHandle.has_value() && animator.IsValid(),
				"Both ancestor controllers and their single shared animator must be live before acquisition.");
			if(!firstHandle || !secondHandle || !animator.IsValid()) return;
			const auto first = runtime.Submit(outer, CommandKind::Acquire, 906U, 1U, 0U,
				0U, MotionMode::RootMotion);
			context.Expect(first.state == ReceiptState::Applied && first.ownerToken != 0U,
				"The first controller may exclusively acquire the shared root animator.");
			if(first.state != ReceiptState::Applied) return;
			const auto second = runtime.Submit(inner, CommandKind::Acquire, 907U, 1U, 0U,
				0U, MotionMode::RootMotion);
			const auto firstLease = PhysicsSystem::Instance().ReadCharacterMotionLease(runtime.Controller(outer));
			std::shared_ptr<const AnimationRootControllerBindingSnapshot> pin;
			const auto* binding = runtime.RootBinding(animator, pin);
			context.Expect(second.state == ReceiptState::Rejected
				&& second.reason == LocomotionReason::OwnershipConflict && second.ownerToken == 0U
				&& firstLease && firstLease->state == CharacterMotionLeaseState::LocomotionOwned
				&& firstLease->owner.objectId == objects.getUUID(outer)
				&& binding != nullptr && binding->controller.objectId == objects.getUUID(outer)
				&& binding->rootMotionEnabled
				&& AnimationConsumerRegistry::Instance().RootMotionDeclarationCount() == 1U,
				"A second ancestor mapping the same animator must fail without disturbing the first root owner.");
		});
	}
}
