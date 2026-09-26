// LocomotionObservedAnimationTests.cpp - live physics feedback and animator binding.

#include "../TestHarness.h"

#include "../Animation/AnimationAuthorityTestHelpers.h"
#include "LocomotionAnimationFixture.h"

#include "Animation/AnimationSystem.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Locomotion/LocomotionAnimationBindings.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> workloads =
				animation.DeclareFrameWorkloads(kFixedStep);
			const TerraEngine::Scheduling::FrameExecutionPlan result =
				TerraEngine::Scheduling::ExecuteFrameWorkloadsCompatibility(
					std::move(workloads), "locomotion-observed-animation");
			return result.valid && result.completed;
		}

		class ObservedAnimationRuntime final {
		public:
			ObservedAnimationRuntime(TestContext& context, bool wall, bool invalidSpeedMapping,
				bool attachAnimator = true)
				: context_(context)
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				locomotion_.Shutdown();
				animation_.Shutdown();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				assets_ = LocomotionAnimationFixture::Create(context_);

				auto& objects = ObjectManager::instance();
				owner_ = objects.createObject("Observed locomotion owner");
				context_.Expect(objects.AddComponent<Transform>(owner_) != nullptr,
					"The locomotion owner must have a canonical transform.");
				CharacterControllerComponent* controller =
					objects.AddComponent<CharacterControllerComponent>(owner_);
				if(controller != nullptr) {
					controller->radius = 0.35F;
					controller->height = 1.8F;
					controller->localCenter = {0.0F, 0.9F, 0.0F};
					controller->useGravity = false;
					controller->rotationPolicy = PhysicsCharacterRotationPolicy::Locked;
				}
				movement_ = objects.AddComponent<CharacterLocomotionComponent>(owner_);
				context_.Expect(controller != nullptr && movement_ != nullptr,
					"The live owner must have both a Physics character controller and locomotion authoring.");
				if(movement_ != nullptr) {
					movement_->maximumSpeed = 4.0;
					movement_->acceleration = 30.0;
					movement_->braking = 30.0;
					movement_->animatorBinding = CharacterLocomotionComponent::kSelfBinding;
					movement_->speedParameter = invalidSpeedMapping ? "StartMove" : "Speed";
					movement_->localDirectionXParameter = "DirectionX";
					movement_->localDirectionZParameter = "DirectionZ";
					movement_->groundedParameter = "Grounded";
					movement_->movingParameter = "Moving";
				}
				if(attachAnimator) context_.Expect(assets_->AttachAnimator(objects, owner_) != nullptr,
					"The fixture must attach a resource-backed controller animator to the owner.");
				if(wall) AddFloorAndWall(objects);

				animation_.SetHeadlessExecutionRequired(true);
				animation_.Initialize();
				context_.Expect(physics_.StartWorld(), "The integration fixture must start a real Physics world.");
				for(std::uint32_t frame = 0U; frame < 4U; ++frame) AdvancePhysics();
				locomotion_.Initialize();
				locomotion_.ObserveProtectedBoundary();
				handle_ = locomotion_.ResolvePublished(objects.getUUID(owner_));
				context_.Expect(handle_.has_value(),
					"The real Physics state packet must bind the authored locomotion owner before integration assertions.");
				// The first protected Animation frame reconciles the authored controller;
				// a second frame publishes its initial immutable parameter snapshot.
				if(attachAnimator) {
					context_.Expect(ExecuteAnimationFrame(animation_),
						"The production Animation coordinator must bind the cooked fixture assets.");
					context_.Expect(ExecuteAnimationFrame(animation_),
						"The bound controller must publish its initial parameter snapshot.");
				}
				locomotion_.Update(kFixedStep);
				// Complete the setup parameter batch before any test submits movement
				// commands, just as the production frame does after protected Publish.
				if(attachAnimator) context_.Expect(ExecuteAnimationFrame(animation_),
					"The initial locomotion parameter batch must reach its Animation boundary before gameplay commands.");
			}

			~ObservedAnimationRuntime()
			{
				locomotion_.Shutdown();
				animation_.Shutdown();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				assets_.reset();
				ThreadPool::instance().shutdown();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(kFixedStep);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "locomotion-observed-animation-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			CommandReceipt AcquireAndSetIntent(std::uint64_t producer)
			{
				if(!handle_) return {};
				LocomotionCommand acquire{};
				acquire.kind = CommandKind::Acquire;
				acquire.handle = *handle_;
				acquire.expectedConfigurationRevision = 1U;
				acquire.producerId = producer;
				acquire.sequence = 1U;
				acquire.correlationId = producer * 100U + 1U;
				(void)locomotion_.SubmitBatch(std::span(&acquire, 1U));
				locomotion_.Update(kFixedStep);
				if(HasAnimator()) (void)ExecuteAnimationFrame(animation_);
				const CommandReceipt acquired = locomotion_.ReadReceipt(
					*handle_, producer, acquire.sequence, acquire.correlationId);
				if(acquired.ownerToken == 0U) return acquired;
				LocomotionCommand intent{};
				intent.kind = CommandKind::SetIntent;
				intent.handle = *handle_;
				intent.expectedAuthorityEpoch = acquired.authorityEpoch;
				intent.expectedConfigurationRevision = 1U;
				intent.ownerToken = acquired.ownerToken;
				intent.intentKind = IntentKind::Script;
				intent.directionSpace = DirectionSpace::World;
				intent.direction = {1.0, 0.0, 0.0};
				intent.requestedSpeed = 4.0;
				intent.lifetime = IntentLifetime::UntilStopped;
				intent.producerId = producer;
				intent.sequence = 2U;
				intent.correlationId = producer * 100U + 2U;
				(void)locomotion_.SubmitBatch(std::span(&intent, 1U));
				locomotion_.Update(kFixedStep);
				if(HasAnimator()) (void)ExecuteAnimationFrame(animation_);
				return acquired;
			}

			double OwnerX() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::numeric_limits<double>::quiet_NaN();
				const UUID id = ObjectManager::instance().getUUID(owner_);
				const auto state = std::find_if(packet->states->begin(), packet->states->end(), [id](const auto& item) {
					return item.objectId == id;
				});
				return state == packet->states->end() ? std::numeric_limits<double>::quiet_NaN()
					: state->pose.position.x;
			}

			const LocomotionObservation* CurrentObservation(std::shared_ptr<const LocomotionObservationRoot>& pin) const
			{
				pin = locomotion_.CopyObservations();
				if(!pin || pin->entries.empty()) return nullptr;
				const auto found = std::find_if(pin->entries.begin(), pin->entries.end(), [this](const auto& value) {
					return handle_ && value.handle == *handle_;
				});
				return found == pin->entries.end() ? nullptr : &*found;
			}

			Object Owner() const { return owner_; }
			Object Child() const { return child_; }
			Object ExternalParent() const { return externalParent_; }
			CharacterLocomotionComponent* Movement() const { return movement_; }
			LocomotionHandle Handle() const { return handle_.value_or(LocomotionHandle{}); }
			LocomotionSystem& Locomotion() { return locomotion_; }
			AnimationSystem& Animation() { return animation_; }
			PhysicsSystem& Physics() { return physics_; }
			LocomotionAnimationFixture& Assets() { return *assets_; }
			bool HasAnimator() const
			{
				return animation_.ResolveAnimatorForObject(ObjectManager::instance().getUUID(owner_)).IsValid();
			}

			Object AddAnimatorDescendant()
			{
				auto& objects = ObjectManager::instance();
				child_ = objects.createObject("Locomotion animator descendant");
				(void)objects.AddComponent<Transform>(child_);
				(void)objects.setParent(child_, owner_);
				(void)assets_->AttachAnimator(objects, child_);
				return child_;
			}

			Object AddExternalParent()
			{
				auto& objects = ObjectManager::instance();
				externalParent_ = objects.createObject("Outside animator parent");
				(void)objects.AddComponent<Transform>(externalParent_);
				return externalParent_;
			}

			bool RefreshAnimationAndLocomotion()
			{
				if(!ExecuteAnimationFrame(animation_)) return false;
				locomotion_.ObserveProtectedBoundary();
				locomotion_.Update(kFixedStep);
				return true;
			}

		private:
			void AddFloorAndWall(ObjectManager& objects)
			{
				const Object floor = objects.createObject("Static locomotion test floor");
				const Object obstacle = objects.createObject("Static locomotion test wall");
				(void)objects.AddComponent<Transform>(floor);
				(void)objects.AddComponent<Transform>(obstacle);
				auto* floorSet = objects.AddComponent<ColliderSetComponent>(floor);
				auto* wallSet = objects.AddComponent<ColliderSetComponent>(obstacle);
				if(floorSet != nullptr) floorSet->Add().shape = BoxColliderShape{{20.0F, 0.5F, 20.0F}};
				if(wallSet != nullptr) wallSet->Add().shape = BoxColliderShape{{0.15F, 1.0F, 2.0F}};
				if(auto* transform = objects.GetComponent<Transform>(floor))
					transform->worldPose.position.offsetMetres = {0.0, -0.5, 0.0};
				if(auto* transform = objects.GetComponent<Transform>(obstacle))
					transform->worldPose.position.offsetMetres = {1.8, 1.0, 0.0};
				objects.MarkObjectTransformDirty(floor);
				objects.MarkObjectTransformDirty(obstacle);
			}

			TestContext& context_;
			PhysicsSystem& physics_ = PhysicsSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			AnimationSystem& animation_ = AnimationSystem::Instance();
			std::unique_ptr<LocomotionAnimationFixture> assets_;
			Object owner_{};
			Object child_{};
			Object externalParent_{};
			CharacterLocomotionComponent* movement_ = nullptr;
			std::optional<LocomotionHandle> handle_;
		};

		const AnimationParameterSnapshot* FindParameter(
			const AnimationPlaybackSnapshot& playback, std::uint32_t id)
		{
			const auto found = std::find_if(playback.parameters.begin(), playback.parameters.end(), [id](const auto& value) {
				return value.parameterId == id;
			});
			return found == playback.parameters.end() ? nullptr : &*found;
		}
	}

	void RegisterLocomotionObservedAnimationTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.AnimationObserved.WallFeedbackReachesNextAnimatorPublication",
			[](TestContext& context) {
				ObservedAnimationRuntime runtime(context, true, false);
				const CommandReceipt acquired = runtime.AcquireAndSetIntent(830U);
				context.Expect(acquired.ownerToken != 0U,
					"The real character must acquire its Physics motion lease before the wall approach.");
				if(acquired.ownerToken == 0U) return;
				// End the setup frame at its Physics boundary. The loop below then
				// follows the real one-Locomotion / one-Animation / one-Physics cadence.
				runtime.AdvancePhysics();
				double previousX = runtime.OwnerX();
				double lastSubmittedSpeed = 0.0;
				std::uint64_t lastSubmittedTick = 0U;
				std::shared_ptr<const AnimationSnapshot> preAnimationSnapshot;
				double firstSubmittedSpeed = 0.0;
				bool firstSampleCaptured = false;
				for(std::uint32_t frame = 0U; frame < 120U; ++frame) {
					runtime.Locomotion().Update(kFixedStep);
					std::shared_ptr<const LocomotionObservationRoot> frameObservationPin;
					const LocomotionObservation* frameObservation = runtime.CurrentObservation(frameObservationPin);
					if(frameObservation != nullptr) {
						lastSubmittedSpeed = frameObservation->observedPlanarSpeed;
						lastSubmittedTick = frameObservation->physicalTick;
					}
					if(frame == 0U) {
						const auto initialAnimator = runtime.Animation().ResolveAnimatorForObject(
							ObjectManager::instance().getUUID(runtime.Owner()));
						const auto before = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), initialAnimator);
						if(before && before->snapshot) preAnimationSnapshot = before->snapshot;
						if(frameObservation != nullptr) {
							firstSubmittedSpeed = frameObservation->observedPlanarSpeed;
							firstSampleCaptured = true;
						}
					}
					context.Expect(ExecuteAnimationFrame(runtime.Animation()),
						"Animation must consume the latest immutable completed-physics observation batch.");
					if(frame == 0U) {
						const auto firstAnimator = runtime.Animation().ResolveAnimatorForObject(
							ObjectManager::instance().getUUID(runtime.Owner()));
						const auto publishedFirst = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), firstAnimator);
						const auto* priorSpeed = preAnimationSnapshot
							? FindParameter(preAnimationSnapshot->playback,
								static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter)) : nullptr;
						const auto* firstSpeed = publishedFirst && publishedFirst->snapshot
							? FindParameter(publishedFirst->snapshot->playback,
								static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter)) : nullptr;
						context.Expect(preAnimationSnapshot && publishedFirst && publishedFirst->snapshot
							&& publishedFirst->snapshot != preAnimationSnapshot
							&& firstSampleCaptured && firstSubmittedSpeed > 0.01 && priorSpeed
							&& std::holds_alternative<float>(priorSpeed->value)
							&& std::abs(static_cast<double>(std::get<float>(priorSpeed->value))
								- firstSubmittedSpeed) > 0.01
							&& firstSpeed && std::holds_alternative<float>(firstSpeed->value)
								&& std::abs(static_cast<double>(std::get<float>(firstSpeed->value)) - firstSubmittedSpeed) < 0.02,
							"A completed Physics sample remains invisible before Animation publication and appears in that next immutable snapshot. firstCaptured="
							+ std::to_string(firstSampleCaptured) + ", firstObserved=" + std::to_string(firstSubmittedSpeed)
							+ ", priorValue=" + (priorSpeed && std::holds_alternative<float>(priorSpeed->value)
								? std::to_string(std::get<float>(priorSpeed->value)) : "missing/nonfloat")
							+ ", publishedValue=" + (firstSpeed && std::holds_alternative<float>(firstSpeed->value)
								? std::to_string(std::get<float>(firstSpeed->value)) : "missing/nonfloat")
							+ ", snapshotAdvanced=" + std::to_string(publishedFirst && publishedFirst->snapshot
								&& publishedFirst->snapshot != preAnimationSnapshot));
					}
					runtime.AdvancePhysics();
					previousX = runtime.OwnerX();
				}
				// The final Physics boundary has one correctly delayed sample; finish
				// its following Locomotion and Animation phases before comparing state.
				runtime.Locomotion().Update(kFixedStep);
				std::shared_ptr<const LocomotionObservationRoot> finalSamplePin;
				const LocomotionObservation* finalSample = runtime.CurrentObservation(finalSamplePin);
				if(finalSample != nullptr) {
					lastSubmittedSpeed = finalSample->observedPlanarSpeed;
					lastSubmittedTick = finalSample->physicalTick;
				}
				context.Expect(ExecuteAnimationFrame(runtime.Animation()),
					"Animation must publish the final completed Physics feedback sample.");
				std::shared_ptr<const LocomotionObservationRoot> observationPin;
				const LocomotionObservation* observation = runtime.CurrentObservation(observationPin);
				const auto animator = runtime.Animation().ResolveAnimatorForObject(
					ObjectManager::instance().getUUID(runtime.Owner()));
				const PinnedAnimationAuthorityRow row =
					PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), animator);
				const auto* speed = row && row->snapshot
					? FindParameter(row->snapshot->playback, static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter))
					: nullptr;
				const auto* moving = row && row->snapshot
					? FindParameter(row->snapshot->playback, static_cast<std::uint32_t>(LocomotionAnimationFixture::kMovingParameter))
					: nullptr;
				context.Expect(observation != nullptr && observation->motionHistoryAvailable
					&& observation->physicalTick != 0U && observation->observedPlanarSpeed < 0.1,
					"The observation must report collision-limited realized speed from the completed physical tick.");
				context.Expect(previousX > 0.5 && previousX < 1.5,
					"The real controller must move forward and stop before the static wall rather than pass through it.");
				context.Expect(row && speed && speed->type == AnimationParameterType::Float
					&& std::holds_alternative<float>(speed->value) && std::get<float>(speed->value) < 0.1F
					&& observation != nullptr
					&& lastSubmittedTick != 0U && observation->physicalTick == lastSubmittedTick
					&& std::abs(static_cast<double>(std::get<float>(speed->value))
						- lastSubmittedSpeed) < 0.02
					&& moving && moving->type == AnimationParameterType::Boolean
					&& std::holds_alternative<bool>(moving->value) && !std::get<bool>(moving->value),
					"The next immutable Animation publication must expose Physics-realized wall feedback, not requested intent speed.");
			});

		registry.Add("Engine.Locomotion.AnimationObserved.BadOptionalTypeDoesNotBlockVelocityOrPartiallyPublish",
			[](TestContext& context) {
				ObservedAnimationRuntime runtime(context, false, true);
				const UUID ownerId = ObjectManager::instance().getUUID(runtime.Owner());
				const CapturedLocomotionAnimator captured = CaptureLocomotionAnimator(
					runtime.Handle(), *runtime.Movement());
				context.Expect(captured.mapping.error == MotionParameterError::WrongType,
					"A trigger mapped into the float Speed field must invalidate only the presentation mapping.");
				const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(ownerId);
				const PinnedAnimationAuthorityRow before = PinnedAnimationAuthorityRow::ForAnimator(
					runtime.Animation(), animator);
				context.Expect(before && before->snapshot,
					"The wrong-type mapping test needs the actual initial controller publication.");
				const CommandReceipt acquired = runtime.AcquireAndSetIntent(831U);
				context.Expect(acquired.ownerToken != 0U,
					"An invalid optional visual mapping must not block independent velocity authority acquisition.");
			for(std::uint32_t frame = 0U; frame < 12U; ++frame) {
					runtime.Locomotion().Update(kFixedStep);
					runtime.AdvancePhysics();
					runtime.Locomotion().Update(kFixedStep);
					if(runtime.HasAnimator()) context.Expect(ExecuteAnimationFrame(runtime.Animation()),
						"Animation must keep publishing while optional locomotion parameter presentation is rejected.");
				}
				const double x = runtime.OwnerX();
				const PinnedAnimationAuthorityRow after = PinnedAnimationAuthorityRow::ForAnimator(
					runtime.Animation(), animator);
				context.Expect(x > 0.1 && after && after->snapshot && before && before->snapshot
					&& after->snapshot->playback.parameters == before->snapshot->playback.parameters,
					"Velocity must advance physically while a bad presentation mapping leaves the complete published parameter batch unchanged.");
			});

		registry.Add("Engine.Locomotion.AnimationObserved.MissingAnimatorDoesNotBlockVelocity",
			[](TestContext& context) {
				ObservedAnimationRuntime runtime(context, false, false, false);
				const auto captured = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement());
				context.Expect(!captured.animator.IsValid()
					&& captured.reason == LocomotionReason::MissingBinding,
					"A missing optional AnimatorComponent must remain a copied binding error rather than an owner failure.");
				const CommandReceipt acquired = runtime.AcquireAndSetIntent(832U);
				context.Expect(acquired.ownerToken != 0U,
					"A missing optional animator must not prevent independent velocity ownership.");
				for(std::uint32_t frame = 0U; frame < 12U; ++frame) {
					runtime.Locomotion().Update(kFixedStep);
					runtime.AdvancePhysics();
					runtime.Locomotion().Update(kFixedStep);
				}
				context.Expect(runtime.OwnerX() > 0.1,
					"The controller must continue moving under real Physics feedback without an animator attachment.");
			});

		registry.Add("Engine.Locomotion.AnimationObserved.CaptureValidatesSelfDescendantReparentAndReload",
			[](TestContext& context) {
				ObservedAnimationRuntime runtime(context, false, false);
				const UUID ownerId = ObjectManager::instance().getUUID(runtime.Owner());
				const auto self = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement());
				context.Expect(self.animator.IsValid() && self.objectId == ownerId
					&& self.reason == LocomotionReason::None && self.mapping.error == MotionParameterError::None,
					"Self binding must capture the exact active animator and a valid typed parameter mapping.");
				const auto controllerGeneration = self.controller.generation;
				const std::uint64_t bindingRevisionBeforeReload =
					ResourceManager::Instance().GetAnimationBindingResourceRevision();
				const bool reloaded = ResourceManager::Instance().ReloadAnimationAsset(
					runtime.Assets().ControllerId(), AssetType::AnimationController);
				const std::uint64_t bindingRevisionAfterReload =
					ResourceManager::Instance().GetAnimationBindingResourceRevision();
				context.Expect(reloaded && bindingRevisionAfterReload > bindingRevisionBeforeReload,
					"A controller reload must advance the revision observed by Animation binding reconciliation.");
				const auto staleCapture = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement(), &self);
				LocomotionObservation staleObservation{};
				staleObservation.motionHistoryAvailable = true;
				staleObservation.observedPlanarSpeed = 2.0;
				staleObservation.observationDurationSeconds = kFixedStep;
				const auto staleBatch = PrepareMotionParameters(staleCapture.mapping,
					staleObservation, 833U, 1U);
				context.Expect(reloaded && staleCapture.reason == LocomotionReason::AnimationUnavailable
					&& !staleCapture.animator.IsValid()
					&& staleBatch.error != MotionParameterError::None && staleBatch.count == 0U,
					"A controller generation replaced after capture must fail closed before stale parameter IDs can be submitted.");
				CapturedLocomotionAnimator reloadedCapture;
				for(std::uint32_t boundary = 0U; boundary < 8U; ++boundary) {
					if(!runtime.RefreshAnimationAndLocomotion()) break;
					reloadedCapture = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement(), &self);
					if(reloadedCapture.animator.IsValid()
						&& reloadedCapture.controller.generation != controllerGeneration) break;
				}
				context.Expect(reloadedCapture.animator.IsValid()
					&& reloadedCapture.controller.generation != controllerGeneration
					&& reloadedCapture.mapping.controllerGeneration == reloadedCapture.controller.generation
					&& reloadedCapture.mapping.error == MotionParameterError::None,
					"A changed controller asset generation must refresh the copied mapping without invalidating the live animator. reloaded="
					+ std::to_string(reloaded) + ", reason=" + std::to_string(static_cast<unsigned>(reloadedCapture.reason))
					+ ", validAnimator=" + std::to_string(reloadedCapture.animator.IsValid())
					+ ", capturedGeneration=" + std::to_string(reloadedCapture.controller.generation)
					+ ", mappingGeneration=" + std::to_string(reloadedCapture.mapping.controllerGeneration)
					+ ", oldGeneration=" + std::to_string(controllerGeneration)
					+ ", mappingError=" + std::to_string(static_cast<unsigned>(reloadedCapture.mapping.error))
					+ ", currentResourceGeneration=" + std::to_string(ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						runtime.Assets().ControllerId(), AssetType::AnimationController))
					+ ", authorityRowPresent=" + std::to_string(static_cast<bool>(PinnedAnimationAuthorityRow::ForAnimator(
						runtime.Animation(), runtime.Animation().ResolveAnimatorForObject(ownerId))))
					+ ", animatorAlive=" + std::to_string(runtime.Animation().IsAnimatorInstanceAlive(
						runtime.Animation().ResolveAnimatorForObject(ownerId)))
					+ ", reason=" + std::to_string(static_cast<unsigned>(reloadedCapture.reason)));

				const Object child = runtime.AddAnimatorDescendant();
				context.Expect(runtime.RefreshAnimationAndLocomotion(),
					"The child animator must enter the real authored Animation binding publication.");
				runtime.Movement()->animatorBinding = CharacterLocomotionComponent::kDescendantBinding;
				runtime.Movement()->animatorObjectId = ObjectManager::instance().getUUID(child);
				(void)runtime.RefreshAnimationAndLocomotion();
				const auto descendant = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement());
				context.Expect(descendant.animator.IsValid() && descendant.objectId == runtime.Movement()->animatorObjectId
					&& descendant.mapping.error == MotionParameterError::None,
					"A live same-scene descendant animator must be accepted after the owner hierarchy changes.");

				const Object outside = runtime.AddExternalParent();
				context.Expect(ObjectManager::instance().setParent(child, outside),
					"The fixture must reparent the named animator outside its locomotion owner hierarchy.");
				const auto detached = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement(), &descendant);
				context.Expect(!detached.animator.IsValid() && !detached.rootReady
					&& detached.reason == LocomotionReason::MissingBinding,
					"Reparenting a descendant outside the owner hierarchy must revoke the copied animator binding.");

				runtime.Movement()->animatorObjectId = GenerateUUID();
				const auto missing = CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement());
				context.Expect(!missing.animator.IsValid() && missing.reason == LocomotionReason::MissingBinding,
					"A missing descendant UUID must fail closed instead of falling back to another animator.");
			});

		registry.Add("Engine.Locomotion.AnimationObserved.ControllerReloadRejectsQueuedMappingAndRetriesFreshValues",
			[](TestContext& context) {
				ObservedAnimationRuntime runtime(context, false, false);
				const CommandReceipt acquired = runtime.AcquireAndSetIntent(834U);
				context.Expect(acquired.ownerToken != 0U,
					"The reload race test requires a live locomotion owner before queuing observed parameters.");
				if(acquired.ownerToken == 0U) return;
				const AnimatorInstanceHandle animator = runtime.Animation().ResolveAnimatorForObject(
					ObjectManager::instance().getUUID(runtime.Owner()));
				const CapturedLocomotionAnimator capturedBeforeReload =
					CaptureLocomotionAnimator(runtime.Handle(), *runtime.Movement());
				const PinnedAnimationAuthorityRow beforeReload = PinnedAnimationAuthorityRow::ForAnimator(
					runtime.Animation(), animator);
				const auto* baselineSpeed = beforeReload && beforeReload->snapshot
					? FindParameter(beforeReload->snapshot->playback,
						static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter)) : nullptr;
				const std::optional<float> baselineSpeedValue = baselineSpeed
					&& std::holds_alternative<float>(baselineSpeed->value)
					? std::optional<float>{std::get<float>(baselineSpeed->value)} : std::nullopt;

				// Queue a real sample against the currently published controller, then
				// replace its parameter identity before Animation reaches the command.
				runtime.Locomotion().Update(kFixedStep);
				runtime.AdvancePhysics();
				runtime.Locomotion().Update(kFixedStep);
				std::shared_ptr<const LocomotionObservationRoot> firstPin;
				const LocomotionObservation* first = runtime.CurrentObservation(firstPin);
				context.Expect(first != nullptr && first->physicalTick != 0U,
					"The queued parameter batch must carry a completed Physics sample.");
				if(first == nullptr) return;
				// This known-sequence sentinel proves terminal rejection independently
				// of the locomotion producer's private monotonic sequence counter.
				AnimationCommand sentinel;
				sentinel.target = animator;
				sentinel.expectedController = capturedBeforeReload.controller;
				sentinel.parameterId = static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter);
				sentinel.value = 37.0F;
				sentinel.producerId = 0x4C544553U;
				sentinel.producerSequence = 1U;
				sentinel.correlationId = 1U;
				const AnimationCommandReceipt queuedSentinel = runtime.Animation().SubmitCommand(sentinel);
				context.Expect(queuedSentinel.status == AnimationStatus::Queued
					&& capturedBeforeReload.controller.IsValid() && baselineSpeedValue.has_value(),
					"The stale-write sentinel must use a valid pre-reload mapping with a pinned authored baseline.");
				if(queuedSentinel.status != AnimationStatus::Queued || !baselineSpeedValue) return;

				constexpr std::uint32_t replacementSpeedId = 661U;
				auto replacement = runtime.Assets().Controller();
				auto speed = std::find_if(replacement.parameters.begin(), replacement.parameters.end(),
					[](const auto& parameter) { return parameter.name == "Speed"; });
				context.Expect(speed != replacement.parameters.end(),
					"The replacement controller must retain the mapped Speed parameter name.");
				if(speed == replacement.parameters.end()) return;
				speed->stableId = replacementSpeedId;
				speed->defaultValue = 9.25F;
				std::vector<std::uint8_t> bytes;
				context.Expect(SerializeAnimationControllerAsset(replacement, bytes) == AnimationStatus::Ok,
					"The replacement controller must serialize through the production asset format.");
				if(bytes.empty()) return;
				{
					std::ofstream stream(runtime.Assets().ControllerPath(), std::ios::binary | std::ios::trunc);
					stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
					context.Expect(stream.good(), "The fixture controller file must contain the replacement parameter identity.");
				}
				const std::uint64_t bindingRevisionBeforeReload =
					ResourceManager::Instance().GetAnimationBindingResourceRevision();
				const AnimationGeneration controllerGenerationBeforeReload =
					ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						runtime.Assets().ControllerId(), AssetType::AnimationController);
				const bool reloaded = ResourceManager::Instance().ReloadAnimationAsset(
					runtime.Assets().ControllerId(), AssetType::AnimationController);
				const std::uint64_t bindingRevisionAfterReload =
					ResourceManager::Instance().GetAnimationBindingResourceRevision();
				context.Expect(reloaded, "The ResourceManager must publish the replacement controller generation.");
				const AnimationGeneration controllerGenerationAfterReload =
					ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						runtime.Assets().ControllerId(), AssetType::AnimationController);
				context.Expect(bindingRevisionAfterReload > bindingRevisionBeforeReload
					&& controllerGenerationAfterReload > controllerGenerationBeforeReload,
					"Controller reload must advance both binding-visible resource and asset generations. beforeRevision="
					+ std::to_string(bindingRevisionBeforeReload) + ", afterRevision="
					+ std::to_string(bindingRevisionAfterReload) + ", beforeAssetGeneration="
					+ std::to_string(controllerGenerationBeforeReload) + ", afterAssetGeneration="
					+ std::to_string(controllerGenerationAfterReload));
				if(!reloaded) return;

				context.Expect(ExecuteAnimationFrame(runtime.Animation()),
					"Animation must reject old-generation parameter commands at its protected boundary.");
				const AnimationCommandReceipt rejectedSentinel = runtime.Animation().ReadCommandReceipt(
					sentinel.producerId, sentinel.producerSequence, sentinel.correlationId);
				context.Expect(rejectedSentinel.status == AnimationStatus::StaleHandle,
					"The old controller sentinel must receive a terminal stale-generation receipt.");
				auto row = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), animator);
				const auto* staleSpeed = row && row->snapshot
					? FindParameter(row->snapshot->playback, static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter))
					: nullptr;
				const auto* replacementSpeed = row && row->snapshot
					? FindParameter(row->snapshot->playback, replacementSpeedId) : nullptr;
				const bool oldSnapshotUnchanged = row && row->snapshot && staleSpeed
					&& std::holds_alternative<float>(staleSpeed->value)
					&& std::abs(std::get<float>(staleSpeed->value) - *baselineSpeedValue) < 1.0e-5F;
				const bool replacementDefaultPublished = row && row->snapshot && staleSpeed == nullptr && replacementSpeed
					&& std::holds_alternative<float>(replacementSpeed->value)
					&& std::abs(std::get<float>(replacementSpeed->value) - 9.25F) < 1.0e-5F;
				context.Expect(oldSnapshotUnchanged || replacementDefaultPublished,
					"A rejected old-generation batch must leave the retained snapshot unchanged or publish only the new authored default. row="
					+ std::to_string(static_cast<bool>(row)) + ", snapshot="
					+ std::to_string(row && static_cast<bool>(row->snapshot)) + ", staleId="
					+ std::to_string(staleSpeed != nullptr) + ", newId=" + std::to_string(replacementSpeed != nullptr)
					+ ", unchanged=" + std::to_string(oldSnapshotUnchanged)
					+ ", replacementDefault=" + std::to_string(replacementDefaultPublished)
					+ ", observed=" + std::to_string(first->observedPlanarSpeed) + ", newValue="
					+ (replacementSpeed && std::holds_alternative<float>(replacementSpeed->value)
						? std::to_string(std::get<float>(replacementSpeed->value)) : "missing/nonfloat")
					+ ", expectedDefault=" + (std::holds_alternative<float>(speed->defaultValue)
						? std::to_string(std::get<float>(speed->defaultValue)) : "nonfloat"));

				std::uint64_t replacementGeneration = 0U;
				for(std::uint32_t boundary = 0U; boundary < 8U; ++boundary) {
					row = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), animator);
					if(row && row->controller.generation != capturedBeforeReload.controller.generation) {
						replacementGeneration = row->controller.generation;
						break;
					}
					context.Expect(ExecuteAnimationFrame(runtime.Animation()),
						"Real Animation coordinator boundaries must finish rebinding the controller generation.");
				}
				row = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), animator);
				replacementSpeed = row && row->snapshot
					? FindParameter(row->snapshot->playback, replacementSpeedId) : nullptr;
				const auto* oldSpeedAfterRebind = row && row->snapshot
					? FindParameter(row->snapshot->playback,
						static_cast<std::uint32_t>(LocomotionAnimationFixture::kSpeedParameter)) : nullptr;
				const AnimationProductionIntegrationSnapshot bindingDiagnostics =
					runtime.Animation().ReadProductionIntegrationSnapshot();
				context.Expect(replacementGeneration != 0U && row
					&& row->controller.generation == replacementGeneration
					&& row->snapshot && oldSpeedAfterRebind == nullptr && replacementSpeed == nullptr,
					"Bounded Animation boundaries must publish the new controller generation without carrying the stale parameter ID; controller defaults are not materialized into runtime parameter snapshots until a parameter is written. currentResourceGeneration="
					+ std::to_string(ResourceManager::Instance().GetAnimationAssetRuntimeGeneration(
						runtime.Assets().ControllerId(), AssetType::AnimationController))
					+ ", rowPresent=" + std::to_string(static_cast<bool>(row))
					+ ", rowControllerGeneration=" + std::to_string(row ? row->controller.generation : 0U)
					+ ", rowStatus=" + std::to_string(static_cast<unsigned>(row ? row->status : AnimationStatus::NotReady))
					+ ", rowActive=" + std::to_string(row && row->active)
					+ ", rowFallback=" + std::to_string(row && row->safeFallback)
					+ ", snapshot=" + std::to_string(row && static_cast<bool>(row->snapshot))
					+ ", animatorAlive=" + std::to_string(runtime.Animation().IsAnimatorInstanceAlive(animator))
					+ ", replacementGeneration=" + std::to_string(replacementGeneration)
					+ ", bindingEpisode=" + std::to_string(bindingDiagnostics.bindingEpisodeCount)
					+ ", reconciled=" + std::to_string(bindingDiagnostics.bindingReconciledCount)
					+ ", failures=" + std::to_string(bindingDiagnostics.bindingFailureCount)
					+ ", resourceRevisionBefore=" + std::to_string(bindingRevisionBeforeReload)
					+ ", resourceRevisionAfter=" + std::to_string(bindingRevisionAfterReload)
					+ ", parameterCount=" + std::to_string(row && row->snapshot
						? row->snapshot->playback.parameters.size() : 0U)
					+ ", replacementParameterPresent=" + std::to_string(replacementSpeed != nullptr)
					+ ", replacementParameterType=" + std::to_string(replacementSpeed
						? static_cast<unsigned>(replacementSpeed->type) : 255U)
					+ ", replacementParameterValue=" + (replacementSpeed
						&& std::holds_alternative<float>(replacementSpeed->value)
						? std::to_string(std::get<float>(replacementSpeed->value)) : "missing/nonfloat")
					+ ", oldParameterPresent=" + std::to_string(oldSpeedAfterRebind != nullptr)
					+ ", observedAtQueue=" + (first ? std::to_string(first->observedPlanarSpeed) : "missing"));

				// Once the new generation is current, queue the latest completed Physics
				// sample through real locomotion and verify it reaches the replacement ID.
				runtime.Locomotion().ObserveProtectedBoundary();
				runtime.Locomotion().Update(kFixedStep);
				std::shared_ptr<const LocomotionObservationRoot> freshPin;
				const LocomotionObservation* fresh = runtime.CurrentObservation(freshPin);
				context.Expect(fresh != nullptr && ExecuteAnimationFrame(runtime.Animation()),
					"The fresh observed-value batch must run through the real protected Animation boundary.");
				row = PinnedAnimationAuthorityRow::ForAnimator(runtime.Animation(), animator);
				replacementSpeed = row && row->snapshot
					? FindParameter(row->snapshot->playback, replacementSpeedId) : nullptr;
				context.Expect(row && row->snapshot && replacementSpeed
					&& std::holds_alternative<float>(replacementSpeed->value)
					&& fresh != nullptr
					&& std::abs(static_cast<double>(std::get<float>(replacementSpeed->value))
						- fresh->observedPlanarSpeed) < 0.02,
					"The fresh generation mapping must publish the current observed speed rather than leaving the authored default.");
			});
	}
}
