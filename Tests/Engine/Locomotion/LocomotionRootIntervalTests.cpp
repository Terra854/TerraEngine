// LocomotionRootIntervalTests.cpp - once-only physical root interval contract.
//
// These tests submit copied root samples through the protected Physics API. They
// verify fixed-tick duration, capacity, yaw, and final evidence rather than
// treating an Animation or Physics queue receipt as physical application.

#include "../TestHarness.h"

#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsWorld.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <variant>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;

	namespace {
		struct RootEvidenceGuard final : CharacterMotionValidity {
			std::atomic<bool> valid{true};

			CharacterMotionValidityReason Validate() const noexcept override {
				return valid.load(std::memory_order_relaxed)
					? CharacterMotionValidityReason::Valid : CharacterMotionValidityReason::StaleEvidence;
			}
		};

		struct RootIntervalFixture final {
			Object object;
			PhysicsCharacterCommandTarget controller;
			CharacterMotionLease lease;

			explicit RootIntervalFixture(
				PhysicsCharacterRotationPolicy rotationPolicy = PhysicsCharacterRotationPolicy::AlignToUp,
				bool gravity = false) {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				auto& objects = ObjectManager::instance();
				objects.clear();
				Scene::SceneMutationQueue::Instance().Clear();
				auto& physics = PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
				object = objects.createObject("Root interval controller");
				auto* transform = objects.AddComponent<Transform>(object);
				if(transform == nullptr)
					throw std::runtime_error("Root fixture Transform creation failed.");
				// Keep a gravity fixture above ground so a vertical root sample
				// cannot be mistaken for support or floor penetration recovery.
				if(gravity) transform->worldPose.position.offsetMetres.y = 4.0;
				auto* character = objects.AddComponent<CharacterControllerComponent>(object);
				if(character == nullptr)
					throw std::runtime_error("Root fixture controller creation failed.");
				character->useGravity = gravity;
				character->rotationPolicy = rotationPolicy;
				if(!physics.StartWorld()) throw std::runtime_error("Root fixture Physics world start failed.");
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) Step();
				const auto packet = physics.LatestCharacterStatePacket();
				const UUID objectId = objects.getUUID(object);
				if(!packet || !packet->states)
					throw std::runtime_error("Root fixture character packet missing.");
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[&objectId](const PhysicsCharacterState& state) { return state.objectId == objectId; });
				if(found == packet->states->end())
					throw std::runtime_error("Root fixture controller was not published.");
				controller = {found->world, found->handle, found->objectId,
					found->objectGeneration, found->componentGeneration};
				CharacterMotionLease unleased;
				unleased.controller = controller;
				lease = unleased;
				lease.owner.sceneLifetime = 1U;
				lease.owner.objectId = controller.objectId;
				lease.owner.objectGeneration = controller.objectGeneration;
				lease.owner.attachmentGeneration = 1U;
				lease.owner.slotIndex = 1U;
				lease.owner.slotGeneration = 1U;
				lease.authorityEpoch = 1U;
				lease.motionEpoch = 1U;
				lease.publicationEpoch = 1U;
				lease.state = CharacterMotionLeaseState::LocomotionOwned;
				lease.mode = CharacterMotionMode::RootMotion;
				const std::array changes{CharacterMotionLeaseChange{unleased, lease}};
				if(!physics.PublishCharacterMotionLeases(changes).accepted)
					throw std::runtime_error("Root fixture controller acquisition failed.");
			}

			~RootIntervalFixture() {
				auto& physics = PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			void Step(float elapsedSeconds = -1.0F) {
				auto& objects = ObjectManager::instance();
				auto& physics = PhysicsSystem::Instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(elapsedSeconds < 0.0F ? physics.FixedTimeStep() : elapsedSeconds);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "locomotion-root-interval-physics-flush");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			PhysicsCharacterState State() const {
				const auto packet = PhysicsSystem::Instance().LatestCharacterStatePacket();
				if(!packet || !packet->states)
					throw std::runtime_error("Root fixture current character packet missing.");
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[this](const PhysicsCharacterState& state) { return state.handle == controller.character; });
				if(found == packet->states->end()) throw std::runtime_error("Root fixture controller disappeared.");
				return *found;
			}

			void AddStaticBox(PhysicsVector3 dimensions, PhysicsVector3 position) {
				// Collider-only authoring becomes a static provider body at the
				// ordinary scene/Physics protected publication boundaries.
				auto& objects = ObjectManager::instance();
				const Object box = objects.createObject("Root interval physical obstacle");
				auto* transform = objects.AddComponent<Transform>(box);
				auto* colliders = objects.AddComponent<ColliderSetComponent>(box);
				if(transform == nullptr || colliders == nullptr)
					throw std::runtime_error("Root fixture obstacle creation failed.");
				colliders->Add().shape = BoxColliderShape{{dimensions.x, dimensions.y, dimensions.z}};
				transform->worldPose.position.offsetMetres = {position.x, position.y, position.z};
				objects.MarkObjectTransformDirty(box);
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) Step();
			}

			RootMotionInterval Interval(std::uint64_t evaluationSequence,
				float duration, PhysicsVector3 translation, float yaw = 0.0F) const {
				RootMotionInterval interval;
				interval.lease = lease;
				interval.frame = {kPrimaryPhysicsFrameId, controller.world.worldGeneration};
				interval.animator.slotIndex = 8U;
				interval.animator.animatorGeneration = 1U;
				interval.animator.poseGeneration = static_cast<std::uint32_t>(evaluationSequence);
				interval.animator.sourceGeneration = 1U;
				interval.animator.sourceStateId = 5U;
				interval.animator.evaluationSequence = evaluationSequence;
				interval.animator.bindingGeneration = 1U;
				interval.animator.publicationGeneration = evaluationSequence;
				interval.animator.sourceBeginTick = static_cast<std::int64_t>(evaluationSequence - 1U);
				interval.animator.sourceEndTick = static_cast<std::int64_t>(evaluationSequence);
				interval.ledger.producerId = 9U;
				interval.ledger.producerSequence = evaluationSequence;
				interval.ledger.idempotencyKey = evaluationSequence;
				interval.ledger.correlationId = evaluationSequence;
				interval.localTranslation = translation;
				interval.frameTranslation = translation;
				interval.localYawRadians = yaw;
				interval.frameYawRadians = yaw;
				interval.durationSeconds = duration;
				interval.maximumSpeed = 100.0F;
				interval.maximumTurnRate = 100.0F;
				return interval;
			}
		};

		bool HasTerminal(const RootMotionResultPacket& packet, RootMotionIntervalId id) {
			return std::any_of(packet.records.begin(), packet.records.end(), [id](const RootMotionResultRecord& record) {
				const auto* terminal = std::get_if<RootMotionTerminalResult>(&record.result);
				return terminal != nullptr && terminal->intervalId == id;
			});
		}

		// Capacity stress uses the provider directly: thousands of bounded
		// reservations need no scene graph or render-frame scheduler overhead.
		struct RootProviderFixture final {
			Backend::JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{83U}, 1U};

			RootProviderFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				if(!backend.Initialize().Succeeded())
					throw std::runtime_error("Root provider fixture initialization failed.");
				Backend::PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.frame = {kPrimaryPhysicsFrameId, world.worldGeneration};
				if(!backend.CreateWorld(configuration).Succeeded())
					throw std::runtime_error("Root provider fixture world creation failed.");
			}

			~RootProviderFixture() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
				ThreadPool::instance().shutdown();
			}

			CharacterMotionLease CreateLeased(std::uint32_t slot) {
				PhysicsCharacterCommandTarget controller;
				controller.world = world;
				controller.character = {{83U}, 1U, slot, 1U};
				controller.objectId = TerraEngine::GenerateUUID();
				controller.objectGeneration = 1U;
				controller.componentGeneration = 1U;
				Backend::PhysicsCharacterCreateRequest create;
				create.handle = controller.character;
				create.objectId = controller.objectId;
				create.objectGeneration = 1U;
				create.componentGeneration = 1U;
				create.pose.position.x = static_cast<float>(slot) * 3.0F;
				create.useGravity = false;
				Backend::PhysicsBackendStagingToken staging;
				if(!backend.CreateCharacter(create, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Root provider fixture character creation failed.");
				CharacterMotionLease unleased;
				unleased.controller = controller;
				CharacterMotionLease owned = unleased;
				owned.owner.sceneLifetime = 1U;
				owned.owner.objectId = controller.objectId;
				owned.owner.objectGeneration = 1U;
				owned.owner.attachmentGeneration = 1U;
				owned.owner.slotIndex = slot;
				owned.owner.slotGeneration = 1U;
				owned.authorityEpoch = 1U;
				owned.motionEpoch = 1U;
				owned.publicationEpoch = 1U;
				owned.state = CharacterMotionLeaseState::LocomotionOwned;
				owned.mode = CharacterMotionMode::RootMotion;
				const std::array change{CharacterMotionLeaseChange{unleased, owned}};
				if(!backend.PublishCharacterMotionLeases(change).Succeeded())
					throw std::runtime_error("Root provider fixture lease acquisition failed.");
				return owned;
			}

			RootMotionInterval Interval(const CharacterMotionLease& lease,
				std::uint64_t evaluation, float duration) const {
				RootMotionInterval interval;
				interval.lease = lease;
				interval.frame = {kPrimaryPhysicsFrameId, world.worldGeneration};
				interval.animator.slotIndex = lease.owner.slotIndex;
				interval.animator.animatorGeneration = 1U;
				interval.animator.poseGeneration = static_cast<std::uint32_t>(evaluation);
				interval.animator.sourceGeneration = 1U;
				interval.animator.sourceStateId = 1U;
				interval.animator.evaluationSequence = evaluation;
				interval.animator.bindingGeneration = 1U;
				interval.animator.publicationGeneration = evaluation;
				interval.animator.sourceBeginTick = static_cast<std::int64_t>(evaluation - 1U);
				interval.animator.sourceEndTick = static_cast<std::int64_t>(evaluation);
				interval.ledger.producerId = 1U;
				interval.ledger.producerSequence = evaluation;
				interval.ledger.idempotencyKey = evaluation;
				interval.ledger.correlationId = evaluation;
				interval.durationSeconds = duration;
				interval.maximumSpeed = 1.0F;
				interval.maximumTurnRate = 1.0F;
				return interval;
			}

			void AddStaticBox(PhysicsVector3 dimensions, PhysicsVector3 position) {
				Backend::PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = dimensions;
				const PhysicsShapeHandle shape{{83U}, 1U, 9000U, 1U};
				Backend::PhysicsBodyCreateRequest body;
				body.handle = {{83U}, 1U, 9000U, 1U};
				body.objectId = TerraEngine::GenerateUUID();
				body.objectGeneration = 1U;
				body.componentGeneration = 1U;
				body.shape = shape;
				body.motionType = PhysicsMotionType::Static;
				body.pose.position = position;
				Backend::PhysicsBackendStagingToken staging;
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()
					|| !backend.CreateBody(body, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Root provider fixture static wall creation failed.");
			}

			Backend::PhysicsBackendCharacterState Step(
				const PhysicsCharacterCommandTarget& controller, PhysicsTick tick) {
				constexpr float dt = 1.0F / 60.0F;
				if(!backend.AdvanceCharacters(world, tick, dt).Succeeded()
					|| !backend.Step({world, tick, dt, 1U}).Succeeded())
					throw std::runtime_error("Root provider fixture fixed step failed.");
				Backend::PhysicsBackendExtraction extraction;
				extraction.world = world;
				if(!backend.Extract(extraction).Succeeded())
					throw std::runtime_error("Root provider fixture extraction failed.");
				const auto found = std::find_if(extraction.characters.begin(), extraction.characters.end(),
					[&controller](const Backend::PhysicsBackendCharacterState& state) {
						return state.handle == controller.character;
					});
				if(found == extraction.characters.end())
					throw std::runtime_error("Root provider fixture character disappeared.");
				return *found;
			}
		};
	}

	void RegisterLocomotionRootIntervalTests(TestRegistry& registry) {
		registry.Add("Locomotion.RootInterval.ZeroStepRetainsThenConsumesOnce", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U, dt, {0.1F, 0.0F, 0.0F}));
			context.Expect(reserved.accepted, "A valid root sample must reserve physical and receipt capacity.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The reserved sample must commit exactly once after ledger reservation.");
			const float before = fixture.State().pose.position.x;
			fixture.Step(0.0F);
			context.Expect(std::fabs(fixture.State().pose.position.x - before) < 0.001F,
				"A zero-step render frame must retain, not consume, pending root motion.");
			fixture.Step();
			const float after = fixture.State().pose.position.x;
			context.Expect(after > before + 0.08F && after < before + 0.12F,
				"A completed fixed step must apply the retained interval exactly once.");
			fixture.Step();
			context.Expect(std::fabs(fixture.State().pose.position.x - after) < 0.002F,
				"The completed root sample must never replay on a later fixed step.");
			const auto results = physics.ReadRootMotionResults(0U);
			context.Expect(results && HasTerminal(*results, reserved.reservation.intervalId),
				"A final root receipt must become visible only after full physical consumption.");
		});

		registry.Add("Locomotion.RootInterval.FragmentsAggregateAcrossWholeTick", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			const auto first = physics.ReserveRootMotionInterval(fixture.Interval(1U, dt * 0.5F, {0.1F, 0.0F, 0.0F}));
			const auto second = physics.ReserveRootMotionInterval(fixture.Interval(2U, dt * 0.5F, {0.1F, 0.0F, 0.0F}));
			context.Expect(first.accepted && second.accepted,
				"Two half-step intervals must both reserve capacity before either is consumed.");
			if(!first.accepted || !second.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(first.reservation).accepted
				&& physics.CommitRootMotionInterval(second.reservation).accepted,
				"Both ordered root fragments must commit without replacing each other.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			const float delta = fixture.State().pose.position.x - before;
			context.Expect(delta > 0.18F && delta < 0.22F,
				"The full tick must apply 0.2 m once, not divide each short fragment by its own duration.");
			const auto results = physics.ReadRootMotionResults(0U);
			bool sharedTick = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* tick = std::get_if<RootMotionTickResult>(&record.result);
				if(tick && tick->contributingCount == 2U
					&& tick->contributingIntervals[0] == first.reservation.intervalId
					&& tick->contributingIntervals[1] == second.reservation.intervalId)
					sharedTick = true;
			}
			context.Expect(sharedTick, "A collided or adjusted shared tick must name both contributing interval IDs.");
		});

		registry.Add("Locomotion.RootInterval.PartialIntervalReceivesOnlyTerminalReceipt", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U, 2.0F * dt, {0.2F, 0.0F, 0.0F}));
			context.Expect(reserved.accepted, "A two-step physical interval must reserve once.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"A two-step physical interval must commit once.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			const auto partial = physics.ReadRootMotionResults(0U);
			context.Expect(fixture.State().pose.position.x > before + 0.08F
				&& partial && !HasTerminal(*partial, reserved.reservation.intervalId),
				"The first fragment may move, but may not resolve the ledger terminal receipt.");
			fixture.Step();
			const auto complete = physics.ReadRootMotionResults(0U);
			context.Expect(fixture.State().pose.position.x > before + 0.18F
				&& complete && HasTerminal(*complete, reserved.reservation.intervalId),
				"Only complete duration consumption may publish the final interval receipt.");
		});

		registry.Add("Locomotion.RootInterval.CapacityAndOrderingRejectAtomically", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			std::array<RootMotionIntervalReservation, 8U> reservations{};
			bool firstEightAccepted = true;
			for(std::uint64_t index = 0U; index < reservations.size(); ++index) {
				const auto accepted = physics.ReserveRootMotionInterval(
					fixture.Interval(index + 1U, dt * 0.25F, {0.01F, 0.0F, 0.0F}));
				firstEightAccepted = firstEightAccepted && accepted.accepted;
				reservations[index] = accepted.reservation;
			}
			context.Expect(firstEightAccepted, "Eight pending samples must fit the per-controller limit.");
			if(!firstEightAccepted) return;
			const auto overflow = physics.ReserveRootMotionInterval(fixture.Interval(9U, dt * 0.25F, {0.01F, 0.0F, 0.0F}));
			context.Expect(!overflow.accepted && overflow.error == RootMotionIntervalError::CapacityExceeded,
				"The ninth pending interval must reject without evicting any earlier valid reservation.");
			context.Expect(physics.CancelRootMotionIntervalReservation(reservations.back()),
				"Cancelling an uncommitted reservation must release only its own capacity.");
			const auto replacement = physics.ReserveRootMotionInterval(fixture.Interval(9U, dt * 0.25F, {0.01F, 0.0F, 0.0F}));
			context.Expect(replacement.accepted,
				"A later interval must fit after one exact reservation cancellation.");
		});

		registry.Add("Locomotion.RootInterval.InvalidGuardAndLockedYawFailClosed", [](TestContext& context) {
			RootIntervalFixture locked(PhysicsCharacterRotationPolicy::Locked);
			auto& physics = PhysicsSystem::Instance();
			auto yaw = locked.Interval(1U, physics.FixedTimeStep(), {}, 0.25F);
			const auto unsupported = physics.ReserveRootMotionInterval(yaw);
			context.Expect(!unsupported.accepted && unsupported.error == RootMotionIntervalError::UnsupportedController,
				"Root yaw on a Locked controller must reject before reserving ledger or provider capacity.");
			auto guard = std::make_shared<RootEvidenceGuard>();
			auto movement = locked.Interval(1U, physics.FixedTimeStep(), {0.1F, 0.0F, 0.0F});
			movement.validityGuard = guard;
			const auto reserved = physics.ReserveRootMotionInterval(movement);
			context.Expect(reserved.accepted,
				"A valid source guard may reserve a translation-only root sample on a Locked controller.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The valid source sample must commit before simulated source invalidation.");
			guard->valid.store(false, std::memory_order_relaxed);
			const float before = locked.State().pose.position.x;
			locked.Step();
			context.Expect(std::fabs(locked.State().pose.position.x - before) < 0.001F,
				"A source invalidated before protected Physics stepping may contribute no stale root motion.");
		});

		registry.Add("Locomotion.RootInterval.ZeroLimitsAdjustWithoutRejecting", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			auto interval = fixture.Interval(1U, physics.FixedTimeStep(), {0.1F, 0.0F, 0.0F}, 0.2F);
			interval.maximumSpeed = 0.0F;
			interval.maximumTurnRate = 0.0F;
			const auto reserved = physics.ReserveRootMotionInterval(interval);
			context.Expect(reserved.accepted, "A valid zero-speed, zero-turn profile must reserve but physically clamp its root sample.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The zero-limit root sample must commit as a consumed interval.");
			const auto before = fixture.State();
			fixture.Step();
			const auto after = fixture.State();
			context.Expect(std::fabs(after.pose.position.x - before.pose.position.x) < 0.001F
				&& std::fabs(after.requestedYawRadians) < 0.001F,
				"Zero limits must remove translation and yaw without rejecting the producer's evaluation.");
			const auto results = physics.ReadRootMotionResults(0U);
			bool adjusted = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* terminal = std::get_if<RootMotionTerminalResult>(&record.result);
				if(terminal && terminal->intervalId == reserved.reservation.intervalId)
					adjusted = terminal->disposition == RootMotionTerminalDisposition::Adjusted;
			}
			context.Expect(adjusted, "A physically clamped root interval must terminally report adjustment.");
		});

		registry.Add("Locomotion.RootInterval.VerticalRootDoesNotReplayWithGravity", [](TestContext& context) {
			RootIntervalFixture fixture(PhysicsCharacterRotationPolicy::AlignToUp, true);
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U, dt, {0.0F, 0.1F, 0.0F}));
			context.Expect(reserved.accepted, "A vertical root sample on a falling controller must reserve.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The falling controller must commit its one-shot vertical root sample.");
			const float before = fixture.State().pose.position.y;
			fixture.Step();
			const auto applied = fixture.State();
			fixture.Step();
			const auto uncovered = fixture.State();
			context.Expect(applied.pose.position.y > before + 0.07F,
				"The first tick must physically apply its upward root translation.");
			context.Expect(uncovered.pose.position.y < applied.pose.position.y + 0.02F
				&& std::fabs(uncovered.requestedPlanarVelocity.y) < 0.001F,
				"An uncovered tick must not inherit the prior interval's vertical root velocity; gravity remains independent.");
		});

		registry.Add("Locomotion.RootInterval.ReverseSampleAndYawAreOneShot", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			auto reverse = fixture.Interval(1U, physics.FixedTimeStep(), {-0.05F, 0.0F, 0.0F}, -0.25F);
			reverse.animator.sourceBeginTick = 8;
			reverse.animator.sourceEndTick = 4;
			const auto reserved = physics.ReserveRootMotionInterval(reverse);
			context.Expect(reserved.accepted, "A reverse-playback sample must retain signed source ticks and motion.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The reverse sample must commit exactly once.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			const auto applied = fixture.State();
			context.Expect(applied.pose.position.x < before - 0.04F
				&& std::fabs(applied.requestedYawRadians + 0.25F) < 0.02F
				&& std::fabs(applied.realizedYawRadians + 0.25F) < 0.02F,
				"A reverse interval must apply signed translation and upright yaw in one completed tick.");
			fixture.Step();
			const auto uncovered = fixture.State();
			context.Expect(std::fabs(uncovered.pose.position.x - applied.pose.position.x) < 0.002F
				&& std::fabs(uncovered.requestedYawRadians) < 0.001F,
				"The uncovered next tick must not replay translation or root yaw.");
		});

		registry.Add("Locomotion.RootInterval.CommitOrderingAndDuplicateStayExact", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			const auto first = physics.ReserveRootMotionInterval(fixture.Interval(1U, dt, {0.02F, 0.0F, 0.0F}));
			const auto second = physics.ReserveRootMotionInterval(fixture.Interval(2U, dt, {0.02F, 0.0F, 0.0F}));
			context.Expect(first.accepted && second.accepted, "Two sequential evaluations must reserve.");
			if(!first.accepted || !second.accepted) return;
			const auto premature = physics.CommitRootMotionInterval(second.reservation);
			context.Expect(!premature.accepted && premature.error == RootMotionIntervalError::OutOfOrder,
				"A later evaluation cannot commit ahead of an unresolved earlier reservation.");
			context.Expect(physics.CommitRootMotionInterval(first.reservation).accepted
				&& physics.CommitRootMotionInterval(second.reservation).accepted,
				"The rejected premature commit must leave both reservations intact for ordered resolution.");
			const auto replay = physics.ReserveRootMotionInterval(fixture.Interval(2U, dt, {0.02F, 0.0F, 0.0F}));
			context.Expect(!replay.accepted
				&& (replay.error == RootMotionIntervalError::Duplicate
					|| replay.error == RootMotionIntervalError::OutOfOrder),
				"A duplicate evaluation and ledger idempotency identity must not reserve again.");
		});

		registry.Add("Locomotion.RootInterval.DurationAndZeroStepAdmissionAreBounded", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			auto invalid = fixture.Interval(1U, 0.0F, {0.01F, 0.0F, 0.0F});
			const auto nonzeroZeroDuration = physics.ReserveRootMotionInterval(invalid);
			context.Expect(!nonzeroZeroDuration.accepted
				&& nonzeroZeroDuration.error == RootMotionIntervalError::InvalidArgument,
				"A zero-duration nonzero delta must reject before reserving physical or receipt capacity.");
			const auto full = physics.ReserveRootMotionInterval(fixture.Interval(1U, 0.25F, {}));
			context.Expect(full.accepted, "Exactly 0.25 seconds of pending root evaluation must fit.");
			if(!full.accepted) return;
			const auto overflow = physics.ReserveRootMotionInterval(fixture.Interval(2U,
				physics.FixedTimeStep(), {}));
			context.Expect(!overflow.accepted && overflow.error == RootMotionIntervalError::CapacityExceeded,
				"Remaining pending duration beyond 0.25 seconds must reject without consuming the accepted interval.");
			context.Expect(physics.CancelRootMotionIntervalReservation(full.reservation),
				"Exact cancellation must release the pending duration budget.");
			context.Expect(physics.ReserveRootMotionInterval(fixture.Interval(2U,
				physics.FixedTimeStep(), {})).accepted,
				"Duration capacity must be reusable after the earlier reservation is cancelled.");
		});

		registry.Add("Locomotion.RootInterval.TerminalCursorAcknowledgesWithoutEviction", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U,
				physics.FixedTimeStep(), {0.02F, 0.0F, 0.0F}));
			context.Expect(reserved.accepted, "The cursor fixture interval must reserve.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The cursor fixture interval must commit.");
			fixture.Step();
			const auto first = physics.ReadRootMotionResults(0U, 1U);
			context.Expect(first && first->records.size() == 1U && first->hasMore,
				"A one-record immutable page must expose further unacknowledged results.");
			if(!first || first->records.empty()) return;
			const auto second = physics.ReadRootMotionResults(first->nextCursor, 1U);
			context.Expect(second && second->records.size() == 1U
				&& HasTerminal(*second, reserved.reservation.intervalId),
				"The terminal must remain readable behind the tick cursor until explicit acknowledgement.");
			if(!second) return;
			context.Expect(physics.AcknowledgeRootMotionResults(first->nextCursor),
				"Acknowledging the tick must release only the tick record.");
			const auto retained = physics.ReadRootMotionResults(first->nextCursor);
			context.Expect(retained && HasTerminal(*retained, reserved.reservation.intervalId),
				"The terminal record must survive acknowledgement of earlier tick evidence.");
			context.Expect(physics.AcknowledgeRootMotionResults(second->nextCursor),
				"Acknowledging the terminal must finally release its bounded result storage.");
			const auto empty = physics.ReadRootMotionResults(second->nextCursor);
			context.Expect(empty && empty->records.empty(), "An acknowledged terminal must not replay to later readers.");
		});

		registry.Add("Locomotion.RootInterval.LeaseTransitionCancelsWithoutMotion", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U,
				2.0F * physics.FixedTimeStep(), {0.2F, 0.0F, 0.0F}));
			context.Expect(reserved.accepted, "The pending root interval must reserve under its original lease.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The original interval must commit before its lease is revoked.");
			CharacterMotionLease released = fixture.lease;
			released.state = CharacterMotionLeaseState::Unleased;
			released.mode = CharacterMotionMode::Velocity;
			released.owner = {};
			released.authorityEpoch += 1U;
			released.motionEpoch += 1U;
			released.publicationEpoch += 1U;
			const std::array changes{CharacterMotionLeaseChange{fixture.lease, released}};
			context.Expect(physics.PublishCharacterMotionLeases(changes).accepted,
				"A protected lease release must revoke the pending physical interval.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			context.Expect(std::fabs(fixture.State().pose.position.x - before) < 0.001F,
				"A revoked interval must contribute zero even if its evaluation was already committed.");
			const auto results = physics.ReadRootMotionResults(0U);
			bool cancelled = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* terminal = std::get_if<RootMotionTerminalResult>(&record.result);
				if(terminal && terminal->intervalId == reserved.reservation.intervalId)
					cancelled = terminal->disposition == RootMotionTerminalDisposition::Cancelled;
			}
			context.Expect(cancelled, "The exact committed ledger interval must terminally report cancellation.");
		});

		registry.Add("Locomotion.RootInterval.EmergencyClearSealsOldRootLease", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const auto pending = physics.ReserveRootMotionInterval(fixture.Interval(1U,
				physics.FixedTimeStep(), {0.1F, 0.0F, 0.0F}));
			context.Expect(pending.accepted, "An interval must be active before emergency root clear.");
			if(!pending.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(pending.reservation).accepted,
				"The active interval must commit before its owner is cleared.");
			CharacterMotionTarget clear;
			clear.controller = fixture.controller;
			clear.owner = fixture.lease.owner;
			clear.authorityEpoch = fixture.lease.authorityEpoch;
			clear.motionEpoch = fixture.lease.motionEpoch;
			clear.locomotionUpdateEpoch = std::numeric_limits<std::uint64_t>::max();
			clear.mode = CharacterMotionMode::RootMotion;
			clear.frame = {kPrimaryPhysicsFrameId, fixture.controller.world.worldGeneration};
			clear.clearImmediately = true;
			context.Expect(physics.PublishCharacterMotionTarget(clear).Succeeded(),
				"A max-epoch emergency target must clear the root contribution immediately.");
			const auto replay = physics.ReserveRootMotionInterval(fixture.Interval(2U,
				physics.FixedTimeStep(), {0.1F, 0.0F, 0.0F}));
			context.Expect(!replay.accepted && replay.error == RootMotionIntervalError::StaleEvidence,
				"The old root lease must remain sealed against fresh intervals after emergency clear.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			context.Expect(std::fabs(fixture.State().pose.position.x - before) < 0.001F,
				"The cancelled interval cannot resume on the next fixed tick.");
			const auto results = physics.ReadRootMotionResults(0U);
			context.Expect(results && HasTerminal(*results, pending.reservation.intervalId),
				"The cleared interval must retain its exact terminal ledger receipt.");
		});

		registry.Add("Locomotion.RootInterval.LeasedTeleportRejectsThenReleasedTeleportCancels", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const auto pending = physics.ReserveRootMotionInterval(fixture.Interval(1U,
				2.0F * physics.FixedTimeStep(), {0.2F, 0.0F, 0.0F}));
			context.Expect(pending.accepted, "The root sample must reserve before a competing legacy teleport.");
			if(!pending.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(pending.reservation).accepted,
				"The root sample must commit before the stale teleport is submitted.");
			PhysicsPose destination = fixture.State().pose;
			destination.position.x = 5.0F;
			PhysicsCommandMetadata staleMetadata;
			staleMetadata.producerId = 42U;
			staleMetadata.producerSequence = 1U;
			(void)physics.SubmitCommand(PhysicsCommand::MakeCharacterTeleport(
				fixture.controller, destination, staleMetadata));
			fixture.Step();
			context.Expect(fixture.State().pose.position.x < 1.0F,
				"A legacy teleport cannot override an active root-motion lease.");
			CharacterMotionLease released = fixture.lease;
			released.owner = {};
			released.state = CharacterMotionLeaseState::Unleased;
			released.mode = CharacterMotionMode::Velocity;
			released.authorityEpoch += 1U;
			released.motionEpoch += 1U;
			released.publicationEpoch += 1U;
			const std::array release{CharacterMotionLeaseChange{fixture.lease, released}};
			context.Expect(physics.PublishCharacterMotionLeases(release).accepted,
				"A protected release must cancel the partial root interval before a legitimate teleport.");
			PhysicsCommandMetadata freshMetadata;
			freshMetadata.producerId = 42U;
			freshMetadata.producerSequence = 2U;
			context.Expect(physics.SubmitCommand(PhysicsCommand::MakeCharacterTeleport(
				fixture.controller, destination, freshMetadata)).accepted,
				"The released controller must accept a fresh legacy teleport command.");
			fixture.Step();
			context.Expect(std::fabs(fixture.State().pose.position.x - 5.0F) < 0.01F
				&& fixture.State().motionDiscontinuity,
				"The fresh teleport must apply once and mark derivative observations discontinuous.");
			const auto results = physics.ReadRootMotionResults(0U);
			bool cancelled = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* terminal = std::get_if<RootMotionTerminalResult>(&record.result);
				if(terminal && terminal->intervalId == pending.reservation.intervalId)
					cancelled = terminal->disposition == RootMotionTerminalDisposition::Cancelled;
			}
			context.Expect(cancelled, "The partially consumed root interval must report one terminal cancellation after release.");
		});

		registry.Add("Locomotion.RootInterval.WallAdjustsReceiptWithoutCatchup", [](TestContext& context) {
			RootProviderFixture fixture;
			const auto lease = fixture.CreateLeased(1U);
			fixture.AddStaticBox({0.2F, 2.0F, 3.0F}, {4.0F, 1.0F, 0.0F});
			auto interval = fixture.Interval(lease, 1U, 1.0F / 60.0F);
			interval.localTranslation = {0.8F, 0.0F, 0.0F};
			interval.frameTranslation = interval.localTranslation;
			interval.maximumSpeed = 100.0F;
			const auto reserved = fixture.backend.ReserveRootMotionInterval(interval);
			context.Expect(reserved.accepted, "A root sample aimed at a solid wall must reserve normally.");
			if(!reserved.accepted) return;
			context.Expect(fixture.backend.CommitRootMotionInterval(reserved.reservation).accepted,
				"The wall-bound root sample must commit before physical contact.");
			const float before = 3.0F;
			const float blocked = fixture.Step(lease.controller, 1U).pose.position.x;
			context.Expect(blocked < before + 0.7F,
				"The solid wall must physically reduce requested root displacement.");
			const auto results = fixture.backend.ReadRootMotionResults(0U, 256U);
			bool collisionAdjusted = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* tick = std::get_if<RootMotionTickResult>(&record.result);
				if(tick && tick->contributingCount == 1U
					&& tick->contributingIntervals[0] == reserved.reservation.intervalId)
					collisionAdjusted = tick->adjustedByCollision
						&& !tick->adjustedByLimit
						&& tick->realizedTranslation.x < tick->requestedTranslation.x - 0.1F;
			}
			context.Expect(collisionAdjusted,
				"The shared physical tick receipt must distinguish solver collision from input limits.");
			context.Expect(std::fabs(fixture.Step(lease.controller, 2U).pose.position.x - blocked) < 0.01F,
				"Unrealized blocked root translation must not be carried into a later uncovered tick.");
		});

		registry.Add("Locomotion.RootInterval.CeilingBlockedRootYDoesNotReverseGravity", [](TestContext& context) {
			RootIntervalFixture fixture(PhysicsCharacterRotationPolicy::AlignToUp, true);
			fixture.AddStaticBox({3.0F, 0.5F, 3.0F}, {0.0F, 5.3F, 0.0F});
			auto& physics = PhysicsSystem::Instance();
			const auto reserved = physics.ReserveRootMotionInterval(fixture.Interval(1U,
				physics.FixedTimeStep(), {0.0F, 0.2F, 0.0F}));
			context.Expect(reserved.accepted, "A root sample pushing into a ceiling must reserve.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The ceiling sample must commit once.");
			const float before = fixture.State().pose.position.y;
			fixture.Step();
			const float blocked = fixture.State().pose.position.y;
			fixture.Step();
			const float uncovered = fixture.State().pose.position.y;
			context.Expect(blocked < before + 0.16F,
				"Ceiling contact must trim the authored upward root displacement.");
			context.Expect(uncovered > blocked - 0.06F && uncovered < blocked + 0.02F,
				"A blocked root Y must not produce an artificial downward burst or replay upward on the next gravity tick.");
		});

		registry.Add("Locomotion.RootInterval.MultiStepLimitsDoNotAccumulateRemainder", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			auto interval = fixture.Interval(1U, 2.0F * dt, {0.2F, 0.0F, 0.0F}, 0.4F);
			interval.maximumSpeed = 3.0F;
			interval.maximumTurnRate = 6.0F;
			const auto reserved = physics.ReserveRootMotionInterval(interval);
			context.Expect(reserved.accepted, "A finite two-step speed and turn limit must reserve.");
			if(!reserved.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(reserved.reservation).accepted,
				"The limited interval must commit once.");
			const float before = fixture.State().pose.position.x;
			fixture.Step();
			const auto first = fixture.State();
			fixture.Step();
			const auto second = fixture.State();
			context.Expect(first.pose.position.x > before + 0.04F
				&& first.pose.position.x < before + 0.06F
				&& second.pose.position.x > first.pose.position.x + 0.04F
				&& second.pose.position.x < first.pose.position.x + 0.06F,
				"Each complete fixed tick must independently cap its root displacement to speed times dt.");
			context.Expect(std::fabs(first.requestedYawRadians - 0.1F) < 0.02F
				&& std::fabs(second.requestedYawRadians - 0.1F) < 0.02F,
				"Each complete fixed tick must independently cap upright root yaw to turn rate times dt.");
			fixture.Step();
			context.Expect(std::fabs(fixture.State().pose.position.x - second.pose.position.x) < 0.002F,
				"Adjusted translation must not catch up after the interval is terminally consumed.");
			const auto results = physics.ReadRootMotionResults(0U);
			bool adjusted = false;
			if(results) for(const RootMotionResultRecord& record : results->records) {
				const auto* terminal = std::get_if<RootMotionTerminalResult>(&record.result);
				if(terminal && terminal->intervalId == reserved.reservation.intervalId)
					adjusted = terminal->disposition == RootMotionTerminalDisposition::Adjusted;
			}
			context.Expect(adjusted, "The final ledger receipt must report the multi-step adjustment once.");
		});

		registry.Add("Locomotion.RootInterval.Global4096ReservationsBounded", [](TestContext& context) {
			RootProviderFixture fixture;
			std::vector<RootMotionIntervalReservation> reservations;
			reservations.reserve(4096U);
			bool admittedAll = true;
			for(std::uint32_t slot = 1U; slot <= 512U && admittedAll; ++slot) {
				const auto lease = fixture.CreateLeased(slot);
				for(std::uint64_t evaluation = 1U; evaluation <= 8U; ++evaluation) {
					const auto admitted = fixture.backend.ReserveRootMotionInterval(
						fixture.Interval(lease, evaluation, 0.0F));
					if(!admitted.accepted) { admittedAll = false; break; }
					reservations.push_back(admitted.reservation);
				}
			}
			context.Expect(admittedAll && reservations.size() == 4096U,
				"The exact global limit of 4096 pending interval reservations must fit without eviction.");
			if(!admittedAll) return;
			const auto extraLease = fixture.CreateLeased(513U);
			const auto overflow = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(extraLease, 1U, 0.0F));
			context.Expect(!overflow.accepted && overflow.error == RootMotionIntervalError::CapacityExceeded,
				"A free ninth controller must still reject when the global interval limit is full.");
			context.Expect(fixture.backend.CancelRootMotionIntervalReservation(reservations.back()),
				"One exact uncommitted reservation must release global capacity.");
			const auto reused = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(extraLease, 1U, 0.0F));
			context.Expect(reused.accepted,
				"A new controller must acquire only the released global slot, without disturbing other reservations.");
		});

		registry.Add("Locomotion.RootInterval.ResultBackpressurePreservesTerminalSlot", [](TestContext& context) {
			RootProviderFixture fixture;
			const auto lease = fixture.CreateLeased(1U);
			// Each committed zero-duration interval emits one terminal and frees
			// its live slot. Fill the real 65536-record ring to one free entry.
			bool filled = true;
			for(std::uint64_t evaluation = 1U; evaluation <= 65535U; ++evaluation) {
				const auto admitted = fixture.backend.ReserveRootMotionInterval(
					fixture.Interval(lease, evaluation, 0.0F));
				if(!admitted.accepted || !fixture.backend.CommitRootMotionInterval(
					admitted.reservation).accepted) { filled = false; break; }
			}
			context.Expect(filled, "All terminal-only results must fit without implicit acknowledgement or eviction.");
			if(!filled) return;
			const auto pending = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(lease, 65536U, 1.0F / 60.0F));
			context.Expect(pending.accepted, "The final free record must be reserved for this live interval's terminal.");
			if(!pending.accepted) return;
			context.Expect(fixture.backend.CommitRootMotionInterval(pending.reservation).accepted,
				"The live interval must commit while retaining its one permanent terminal slot.");
			context.Expect(!fixture.backend.PrepareRootMotionStepResults(fixture.world, 1U),
				"Physical root consumption must pause when no independent tick-result slot remains.");
			context.Expect(fixture.backend.CancelRootMotionInterval(
				pending.reservation.intervalId, RootMotionIntervalError::Cancelled),
				"Safety cancellation must still succeed under full tick-result backpressure.");
			const auto finalPage = fixture.backend.ReadRootMotionResults(65534U, 3U);
			context.Expect(finalPage && HasTerminal(*finalPage, pending.reservation.intervalId),
				"The reserved terminal slot must publish cancellation without dropping older unacknowledged evidence.");
			// Report the actual ABI size, not an estimated memory cost. The
			// production vector's high-water bytes equal this size times 65536.
			std::cout << "RootMotionResultRecord bytes=" << sizeof(RootMotionResultRecord)
				<< " ring high-water bytes=" << sizeof(RootMotionResultRecord) * 65536U << '\n';
		});

		registry.Add("Locomotion.RootInterval.NewPoseRetainedButSourceChangeRevokes", [](TestContext& context) {
			RootIntervalFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			const float dt = physics.FixedTimeStep();
			auto guard = std::make_shared<RootEvidenceGuard>();
			auto older = fixture.Interval(1U, 2.0F * dt, {0.2F, 0.0F, 0.0F});
			older.validityGuard = guard;
			const auto first = physics.ReserveRootMotionInterval(older);
			context.Expect(first.accepted, "The original animation pose must reserve a two-step interval.");
			if(!first.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(first.reservation).accepted,
				"The original pose must commit before a newer pose is published.");
			fixture.Step();
			const float partial = fixture.State().pose.position.x;
			auto newerPose = fixture.Interval(2U, dt, {0.05F, 0.0F, 0.0F});
			newerPose.validityGuard = guard;
			const auto second = physics.ReserveRootMotionInterval(newerPose);
			context.Expect(second.accepted,
				"A newer pose/evaluation of the same source must not invalidate the retained older interval.");
			if(!second.accepted) return;
			context.Expect(physics.CommitRootMotionInterval(second.reservation).accepted,
				"The newer evaluation must queue after the still-partial older interval.");
			fixture.Step();
			context.Expect(fixture.State().pose.position.x > partial + 0.08F,
				"The retained older interval must finish despite advancing pose generation.");
			auto newSource = fixture.Interval(3U, dt, {0.05F, 0.0F, 0.0F});
			newSource.animator.sourceGeneration = 2U;
			const auto conflicting = physics.ReserveRootMotionInterval(newSource);
			context.Expect(!conflicting.accepted && conflicting.error == RootMotionIntervalError::StaleEvidence,
				"A changed source generation must not interleave behind a pending old-source interval.");
			guard->valid.store(false, std::memory_order_relaxed);
			fixture.Step();
			const auto cancelled = physics.ReadRootMotionResults(0U);
			context.Expect(cancelled && HasTerminal(*cancelled, second.reservation.intervalId),
				"The old-source guarded interval must terminally resolve when source validity changes.");
			const auto replacement = physics.ReserveRootMotionInterval(newSource);
			context.Expect(replacement.accepted,
				"After exact old-source cancellation, the new source generation may reserve a fresh interval.");
		});

		registry.Add("Locomotion.RootInterval.TinyFixedStepsPageWholeTickEvidence", [](TestContext& context) {
			RootProviderFixture fixture;
			const auto lease = fixture.CreateLeased(1U);
			const auto admitted = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(lease, 1U, 0.25F));
			context.Expect(admitted.accepted, "The maximum-duration interval must reserve at a tiny fixed step.");
			if(!admitted.accepted) return;
			context.Expect(fixture.backend.CommitRootMotionInterval(admitted.reservation).accepted,
				"The tiny-step interval must commit once.");
			constexpr float tinyStep = 0.25F / 300.0F + 1.0e-8F;
			context.Expect(fixture.backend.PrepareRootMotionStepResults(fixture.world, 300U),
				"The bounded result ring must reserve all 300 whole-tick records before stepping.");
			bool stepped = true;
			for(PhysicsTick tick = 1U; tick <= 300U; ++tick) {
				if(!fixture.backend.AdvanceCharacters(fixture.world, tick, tinyStep).Succeeded()
					|| !fixture.backend.Step({fixture.world, tick, tinyStep, 1U}).Succeeded()) {
					stepped = false;
					break;
				}
			}
			context.Expect(stepped, "All 300 tiny fixed steps must advance without dropping interval evidence.");
			if(!stepped) return;
			const auto first = fixture.backend.ReadRootMotionResults(0U, 256U);
			context.Expect(first && first->records.size() == 256U && first->hasMore,
				"The first copied result page must expose exactly 256 whole-tick records and a continuation cursor.");
			if(!first) return;
			const auto second = fixture.backend.ReadRootMotionResults(first->nextCursor, 256U);
			context.Expect(second && second->records.size() == 45U
				&& HasTerminal(*second, admitted.reservation.intervalId),
				"The second page must contain the remaining 44 ticks and the one final terminal receipt.");
		});

		registry.Add("Locomotion.RootInterval.DisableRequiresNewLeaseBeforeReenable", [](TestContext& context) {
			RootProviderFixture fixture;
			const auto owned = fixture.CreateLeased(1U);
			const auto active = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(owned, 1U, 1.0F / 60.0F));
			context.Expect(active.accepted, "The root interval must reserve before controller disable.");
			if(!active.accepted) return;
			context.Expect(fixture.backend.CommitRootMotionInterval(active.reservation).accepted,
				"The root interval must commit before controller disable.");
			Backend::PhysicsCharacterUpdateRequest update;
			update.handle = owned.controller.character;
			update.objectId = owned.controller.objectId;
			update.objectGeneration = owned.controller.objectGeneration;
			update.componentGeneration = owned.controller.componentGeneration;
			update.enabled = false;
			context.Expect(fixture.backend.UpdateCharacter(update).Succeeded(),
				"Provider disable must be a protected root cancellation boundary.");
			const auto cancelled = fixture.backend.ReadRootMotionResults(0U, 256U);
			context.Expect(cancelled && HasTerminal(*cancelled, active.reservation.intervalId),
				"Disable must terminally resolve the committed interval before reenable.");
			update.enabled = true;
			update.characterMotionEpoch = owned.motionEpoch;
			context.Expect(!fixture.backend.UpdateCharacter(update).Succeeded(),
				"The same owned lease must not be reenabled through the legacy update path.");
			const auto oldRetry = fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(owned, 2U, 1.0F / 60.0F));
			context.Expect(!oldRetry.accepted,
				"The disabled old root lease cannot admit a new physical interval.");
			CharacterMotionLease released = owned;
			released.owner = {};
			released.state = CharacterMotionLeaseState::Unleased;
			released.mode = CharacterMotionMode::Velocity;
			released.authorityEpoch += 1U;
			released.motionEpoch += 1U;
			released.publicationEpoch += 1U;
			const std::array release{CharacterMotionLeaseChange{owned, released}};
			context.Expect(fixture.backend.PublishCharacterMotionLeases(release).Succeeded(),
				"Protected release must retire the old owner before reenable.");
			update.characterMotionEpoch = released.motionEpoch;
			context.Expect(fixture.backend.UpdateCharacter(update).Succeeded(),
				"An unleased controller may then be safely reenabled.");
			CharacterMotionLease renewed = owned;
			renewed.authorityEpoch = released.authorityEpoch + 1U;
			renewed.motionEpoch = released.motionEpoch + 1U;
			renewed.publicationEpoch = released.publicationEpoch + 1U;
			const std::array reacquire{CharacterMotionLeaseChange{released, renewed}};
			context.Expect(fixture.backend.PublishCharacterMotionLeases(reacquire).Succeeded(),
				"A fresh protected lease must be required for root motion after reenable.");
			context.Expect(!fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(owned, 2U, 1.0F / 60.0F)).accepted,
				"The pre-disable lease identity must stay stale after recovery.");
			context.Expect(fixture.backend.ReserveRootMotionInterval(
				fixture.Interval(renewed, 2U, 1.0F / 60.0F)).accepted,
				"Only the new generation-qualified lease may submit root intervals.");
		});

		registry.Add("Locomotion.RootInterval.FailedRagdollAdmissionRequiresExplicitCurrentPoseGrant",
			[](TestContext& context) {
				RootIntervalFixture fixture;
				auto& physics = PhysicsSystem::Instance();
				const auto beforeRoot = physics.ReserveRootMotionInterval(
					fixture.Interval(1U, physics.FixedTimeStep(), {0.2F, 0.0F, 0.0F}));
				context.Expect(beforeRoot.accepted,
					"The fixture must move before a failed Activate exclusion is granted.");
				if(!beforeRoot.accepted) return;
				context.Expect(physics.CommitRootMotionInterval(beforeRoot.reservation).accepted,
					"The movement must commit before the external handoff.");
				fixture.Step();
				const auto moved = fixture.State().pose;
				context.Expect(moved.position.x > 0.15F,
					"The character must physically move before the current-pose grant test.");

				CharacterMotionLease external = fixture.lease;
				external.owner = {};
				external.state = CharacterMotionLeaseState::ExternalExclusive;
				external.externalOwnerId = 0x12345678U;
				++external.authorityEpoch;
				++external.motionEpoch;
				++external.publicationEpoch;
				const std::array exclude{CharacterMotionLeaseChange{fixture.lease, external}};
				context.Expect(physics.PublishCharacterMotionLeases(exclude).accepted,
					"A protected ragdoll admission must physically exclude root motion.");
				context.Expect(!physics.ReserveRootMotionInterval(
					fixture.Interval(2U, physics.FixedTimeStep(), {0.1F, 0.0F, 0.0F})).accepted,
					"The previous root owner must not submit after external exclusion.");

				auto guard = std::make_shared<RootEvidenceGuard>();
				CharacterFailedRagdollAdmissionProof proof;
				proof.expectedExternalLease = external;
				proof.externalOwnerId = external.externalOwnerId;
				proof.ragdollId = 47U;
				proof.failedActivationCorrelationId = 91U;
				proof.failurePublicationGeneration = 1U;
				proof.failureFrameTick = 1U;
				proof.frame = {kPrimaryPhysicsFrameId, fixture.controller.world.worldGeneration};
				proof.bindingGeneration = 1U;
				proof.bindingGuard = guard;
				auto wrongOwner = proof;
				wrongOwner.externalOwnerId += 1U;
				context.Expect(!physics.AbortFailedExternalRagdollAdmission(wrongOwner).accepted,
					"A different external owner cannot use the failed-admission grant.");
				const auto stillExternal = physics.ReadCharacterMotionLease(fixture.controller);
				context.Expect(stillExternal && *stillExternal == external,
					"A rejected grant must preserve the entire external lease and epochs.");
				guard->valid = false;
				context.Expect(!physics.AbortFailedExternalRagdollAdmission(proof).accepted,
					"A stale animator binding cannot release the external owner.");
				guard->valid = true;
				const auto granted = physics.AbortFailedExternalRagdollAdmission(proof);
				context.Expect(granted.accepted
					&& granted.releasedLease.state == CharacterMotionLeaseState::Unleased
					&& granted.releasedLease.motionEpoch > external.motionEpoch,
					"A later explicit terminal-failure grant must clear authority with a new physical epoch.");
				context.Expect(!physics.AbortFailedExternalRagdollAdmission(proof).accepted,
					"The exact failed-admission proof must not replay after its lease changes.");
				fixture.Step();
				const auto after = fixture.State();
				context.Expect(std::fabs(after.pose.position.x - moved.position.x) < 0.002F
					&& std::fabs(after.pose.position.y - moved.position.y) < 0.002F,
					"Abort must clear at the provider's current controller pose, not teleport to a producer pose.");
			});
		registry.Add("Locomotion.RootInterval.UnpublishedBodyTombstonePreservesCharacterPublicationGate", [](TestContext& context) {
			PhysicsWorld world{{85U}};
			if(!world.Create(PhysicsSettings::Defaults()))
				throw std::runtime_error("Unpublished tombstone fixture world start failed.");
			IncrementalBodyStateUpdateBatch initial;
			initial.world = world.Handle();
			initial.tick = 1U;
			initial.providerOrderGeneration = 1U;
			initial.fullState = true;
			context.Expect(world.PublishPreparedBodyStates(initial).has_value(),
				"The body-state store must first publish a complete empty Physics tick.");

			// A ragdoll Create followed by Activate can retire this handle before
			// either body reaches the immutable body-state store. Its provider
			// tombstone must not reject unrelated live state and character poses.
			const PhysicsBodyHandle unpublished = world.AllocateBody();
			const PhysicsBodyHandle live = world.AllocateBody();
			if(!unpublished.IsValid() || !world.Release(unpublished))
				throw std::runtime_error("Unpublished ragdoll body retirement failed.");
			if(!live.IsValid()) throw std::runtime_error("Live body allocation failed.");
			PhysicsBodyState liveState;
			liveState.handle = live;
			liveState.objectId = TerraEngine::GenerateUUID();
			liveState.objectGeneration = 1U;
			liveState.componentGeneration = 1U;
			IncrementalBodyStateUpdateBatch mixed;
			mixed.world = world.Handle();
			mixed.tick = 2U;
			mixed.providerOrderGeneration = 2U;
			mixed.updates.push_back({liveState, {}, PhysicsBodyStateFieldMask::BodyState,
				PhysicsBodyStateChangeMask::Created, false});
			mixed.updates.push_back({{}, unpublished, PhysicsBodyStateFieldMask::None,
				PhysicsBodyStateChangeMask::Removed, true});
			const auto accepted = world.PublishPreparedBodyStates(mixed);
			context.Expect(accepted && accepted->complete->tick == 2U
				&& accepted->complete->Find(live) != nullptr
				&& accepted->complete->Find(unpublished) == nullptr
				&& accepted->changed->changes->size() == 1U,
				"Never-published ragdoll removal must not poison the same-tick live-state publication.");

			IncrementalBodyStateUpdateBatch contradictory = mixed;
			contradictory.tick = 3U;
			contradictory.providerOrderGeneration = 3U;
			contradictory.updates.back().removedHandle = live;
			context.Expect(!world.PublishPreparedBodyStates(contradictory)
				&& world.AcquireCompleteBodyState()->tick == 2U,
				"A same-handle update/removal conflict must still reject without partial publication.");
			if(!world.Release(live)) throw std::runtime_error("Known body retirement failed.");
			IncrementalBodyStateUpdateBatch knownRemoval;
			knownRemoval.world = world.Handle();
			knownRemoval.tick = 3U;
			knownRemoval.providerOrderGeneration = 3U;
			knownRemoval.updates.push_back({{}, live, PhysicsBodyStateFieldMask::None,
				PhysicsBodyStateChangeMask::Removed, true});
			const auto removed = world.PublishPreparedBodyStates(knownRemoval);
			context.Expect(removed && removed->complete->tick == 3U
				&& removed->complete->Find(live) == nullptr
				&& removed->changed->changes->size() == 1U,
				"A previously published body removal must remain an exact terminal change.");
			const PhysicsBodyHandle duplicate = world.AllocateBody();
			if(!duplicate.IsValid() || !world.Release(duplicate))
				throw std::runtime_error("Duplicate unpublished tombstone setup failed.");
			IncrementalBodyStateUpdateBatch duplicateRemoval;
			duplicateRemoval.world = world.Handle();
			duplicateRemoval.tick = 4U;
			duplicateRemoval.providerOrderGeneration = 4U;
			duplicateRemoval.updates.push_back({{}, duplicate, PhysicsBodyStateFieldMask::None,
				PhysicsBodyStateChangeMask::Removed, true});
			duplicateRemoval.updates.push_back(duplicateRemoval.updates.front());
			context.Expect(!world.PublishPreparedBodyStates(duplicateRemoval)
				&& world.AcquireCompleteBodyState()->tick == 3U,
				"Duplicate never-published tombstones must still reject the complete batch atomically.");
		});
		registry.Add("Locomotion.RootInterval.ExternalCarryBatchExceedsOrdinaryLeaseLimit", [](TestContext& context) {
			// A scene publication may refresh more than the ordinary 256-controller
			// lease command limit at once. One protected root carries all 257 owners.
			constexpr std::size_t count = 257U;
			ThreadPool::instance().shutdown();
			ThreadPool::instance().initialize();
			PhysicsWorld world{{84U}};
			Backend::JoltPhysicsBackend backend{ThreadPool::instance()};
			if(!world.Create(PhysicsSettings::Defaults()) || !backend.Initialize().Succeeded())
				throw std::runtime_error("External carry fixture world initialization failed.");
			Backend::PhysicsBackendWorldConfiguration configuration;
			configuration.world = world.Handle();
			configuration.frame = {kPrimaryPhysicsFrameId, configuration.world.worldGeneration};
			if(!backend.CreateWorld(configuration).Succeeded())
				throw std::runtime_error("External carry fixture provider world creation failed.");
			std::vector<CharacterExternalCarry> carries;
			std::vector<PhysicsCharacterHandle> retiring;
			carries.reserve(count);
			retiring.reserve(count);
			for(std::size_t index = 0U; index < count; ++index) {
				const UUID objectId = TerraEngine::GenerateUUID();
				auto create = [&](std::uint32_t componentGeneration) {
					PhysicsCharacterCommandTarget target;
					target.world = world.Handle();
					target.character = world.AllocateCharacter();
					target.objectId = objectId;
					target.objectGeneration = 1U;
					target.componentGeneration = componentGeneration;
					if(!target.character.IsValid())
						throw std::runtime_error("External carry fixture character allocation failed.");
					Backend::PhysicsCharacterCreateRequest request;
					request.handle = target.character;
					request.objectId = objectId;
					request.objectGeneration = 1U;
					request.componentGeneration = componentGeneration;
					request.shapeKind = PhysicsShapeKind::Capsule;
					request.radius = 0.35F;
					request.height = 1.8F;
					request.pose.position = {static_cast<float>(index) * 3.0F, 1.0F, 0.0F};
					request.useGravity = false;
					Backend::PhysicsBackendStagingToken staging;
					if(!backend.CreateCharacter(request, staging).Succeeded()
						|| !backend.CommitStaging(staging).Succeeded())
						throw std::runtime_error("External carry fixture character creation failed.");
					return target;
				};
				const auto oldTarget = create(1U);
				const auto newTarget = create(2U);
				CharacterMotionLease unleased;
				unleased.controller = oldTarget;
				CharacterMotionLease external = unleased;
				external.state = CharacterMotionLeaseState::ExternalExclusive;
				external.externalOwnerId = static_cast<std::uint64_t>(index) + 1U;
				external.authorityEpoch = 1U;
				external.motionEpoch = 1U;
				external.publicationEpoch = 1U;
				const std::array grant{CharacterMotionLeaseChange{unleased, external}};
				if(!world.PublishCharacterMotionLeases(grant).accepted
					|| !backend.PublishCharacterMotionLeases(grant).Succeeded())
					throw std::runtime_error("External carry fixture owner grant failed.");
				CharacterMotionLease next = external;
				next.controller = newTarget;
				++next.authorityEpoch;
				++next.motionEpoch;
				++next.publicationEpoch;
				carries.push_back({external, next});
				retiring.push_back(oldTarget.character);
			}
			auto invalid = carries;
			++invalid[count / 2U].nextExternalLease.externalOwnerId;
			context.Expect(!world.PrepareExternalCharacterCarry(invalid, retiring).result.accepted
				&& !backend.PreflightCharacterExternalCarry(invalid).Succeeded(),
				"A mismatched owner in a large carry batch must reject before either root changes.");
			for(const auto& carry : carries)
				context.Expect(world.ReadCharacterMotionLease(carry.expectedExternalLease.controller)
					== carry.expectedExternalLease,
					"Rejected carry must preserve every prior external owner, including the tail.");
			auto prepared = world.PrepareExternalCharacterCarry(carries, retiring);
			context.Expect(prepared.result.accepted
				&& backend.PreflightCharacterExternalCarry(carries).Succeeded(),
				"A 257-controller carry must preflight beyond the ordinary lease limit.");
			if(prepared.result.accepted) {
				context.Expect(backend.CommitCharacterExternalCarry(carries)
					&& world.CommitPreparedCharacterMotionLeases(std::move(prepared)),
					"The prevalidated large handoff must commit provider and World roots.");
				for(const auto& carry : carries) {
					context.Expect(world.ReadCharacterMotionLease(carry.nextExternalLease.controller)
						== carry.nextExternalLease,
						"Every replacement must retain its external owner and advanced epochs.");
					context.Expect(backend.DestroyCharacter(
						carry.expectedExternalLease.controller.character).Succeeded()
						&& world.Release(carry.expectedExternalLease.controller.character)
						&& !world.ReadCharacterMotionLease(carry.expectedExternalLease.controller),
						"Old full targets must stale without releasing replacement owners.");
				}
			}
			(void)backend.DestroyWorld(world.Handle());
			(void)backend.Shutdown();
			ThreadPool::instance().shutdown();
		});
	}
}
