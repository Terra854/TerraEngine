// LocomotionLeaseTests.cpp - physical authority regressions for character leases.
//
// These tests drive the provider and the deferred command queue together. A
// lease-map-only assertion would miss a stale velocity that still moves a body.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsCommands.h"
#include "Physics/PhysicsWorld.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		// A separate backend/world pair gives every test a fresh controller lifetime
		// and leaves the process-wide engine singleton untouched.
		struct LeaseFixture final {
			PhysicsWorld world{{71U}, PhysicsCommandQueueLimits{1U, 1U}};
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsCharacterCommandTarget target;

			LeaseFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				if(!world.Create(PhysicsSettings::Defaults()) || !backend.Initialize().Succeeded())
					throw std::runtime_error("The lease fixture could not initialize its world/backend.");
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world.Handle();
				configuration.frame = {kPrimaryPhysicsFrameId, 1U};
				if(!backend.CreateWorld(configuration).Succeeded())
					throw std::runtime_error("The lease fixture could not create its provider world.");
				target = CreateCharacter();
			}

			~LeaseFixture() {
				(void)backend.DestroyWorld(world.Handle());
				(void)backend.Shutdown();
				ThreadPool::instance().shutdown();
			}

			PhysicsCharacterCommandTarget CreateCharacter(float initialX = 0.0F) {
				PhysicsCharacterCommandTarget created;
				created.world = world.Handle();
				created.character = world.AllocateCharacter();
				if(!created.character.IsValid()) throw std::runtime_error("The lease fixture exhausted character handles.");
				created.objectId = TerraEngine::GenerateUUID();
				created.objectGeneration = 1U;
				created.componentGeneration = 1U;
				PhysicsCharacterCreateRequest request;
				request.handle = created.character;
				request.objectId = created.objectId;
				request.objectGeneration = created.objectGeneration;
				request.componentGeneration = created.componentGeneration;
				request.shapeKind = PhysicsShapeKind::Capsule;
				request.radius = 0.5F;
				request.height = 2.0F;
				request.pose.position = {initialX, 1.1F, 0.0F};
				request.useGravity = false;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateCharacter(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("The lease fixture could not commit its controller.");
				return created;
			}

			float AdvanceAndReadX(PhysicsTick tick) {
				constexpr float duration = 1.0F / 60.0F;
				if(!backend.AdvanceCharacters(world.Handle(), tick, duration).Succeeded()
					|| !backend.Step({world.Handle(), tick, duration, 1U}).Succeeded())
					throw std::runtime_error("The lease fixture could not advance its physical tick.");
				PhysicsBackendExtraction extraction;
				extraction.world = world.Handle();
				if(!backend.Extract(extraction).Succeeded())
					throw std::runtime_error("The lease fixture could not extract physical state.");
				for(const PhysicsBackendCharacterState& state : extraction.characters) {
					if(state.handle == target.character) return state.pose.position.x;
				}
				throw std::runtime_error("The lease fixture lost its expected physical controller.");
			}

			CharacterMotionLease Unleased(PhysicsCharacterCommandTarget controller) const {
				CharacterMotionLease lease;
				lease.controller = controller;
				return lease;
			}

			CharacterMotionLease Owned(PhysicsCharacterCommandTarget controller) const {
				CharacterMotionLease lease = Unleased(controller);
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
				lease.mode = CharacterMotionMode::Velocity;
				return lease;
			}

			bool Publish(const CharacterMotionLeaseChange& change) {
				const std::array changes{change};
				return backend.PublishCharacterMotionLeases(changes).Succeeded()
					&& world.PublishCharacterMotionLeases(changes).accepted;
			}
		};
	}

	void RegisterLocomotionLeaseTests(TestRegistry& registry) {
		registry.Add("Locomotion.Lease.AcquireAndReleaseClearPersistentVelocity", [](TestContext& context) {
			LeaseFixture fixture;
			PhysicsCharacterUpdateRequest move;
			move.handle = fixture.target.character;
			move.objectId = fixture.target.objectId;
			move.objectGeneration = fixture.target.objectGeneration;
			move.componentGeneration = fixture.target.componentGeneration;
			move.desiredVelocity = {6.0F, 0.0F, 0.0F};
			context.Expect(fixture.backend.UpdateCharacter(move).Succeeded(), "Unleased direct movement should retain its legacy behavior.");
			const float beforeAcquire = fixture.AdvanceAndReadX(1U);
			context.Expect(beforeAcquire > 0.05F, "The fixture must observe legacy movement before lease acquisition.");

			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease owned = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, owned}), "Acquisition should atomically publish the controller lease.");
			const float afterAcquire = fixture.AdvanceAndReadX(2U);
			context.Expect(std::fabs(afterAcquire - beforeAcquire) < 0.01F,
				"Acquisition must clear the old persistent legacy velocity before any fixed step.");

			CharacterMotionLease released = fixture.Unleased(fixture.target);
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			context.Expect(fixture.Publish({owned, released}), "Release should publish a fresh motion epoch.");
			const float afterRelease = fixture.AdvanceAndReadX(3U);
			context.Expect(std::fabs(afterRelease - beforeAcquire) < 0.01F,
				"Release must not restore the pre-lease velocity.");
		});

		registry.Add("Locomotion.Lease.NewAttachmentMayRestartAuthorityEpoch", [](TestContext& context) {
			LeaseFixture fixture;
			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease firstOwner = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, firstOwner}), "The first attachment must acquire the controller.");
			CharacterMotionLease released = unleased;
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			context.Expect(fixture.Publish({firstOwner, released}), "The first attachment must release its controller.");

			// Authority epochs are local to an attachment, while physical motion and
			// publication epochs keep increasing for the surviving controller.
			CharacterMotionLease secondOwner = fixture.Owned(fixture.target);
			secondOwner.owner.attachmentGeneration = 2U;
			secondOwner.owner.slotGeneration = 2U;
			secondOwner.authorityEpoch = 1U;
			secondOwner.motionEpoch = 3U;
			secondOwner.publicationEpoch = 3U;
			context.Expect(fixture.Publish({released, secondOwner}),
				"A new attachment may restart its authority epoch without reusing a physical motion epoch.");
			const auto current = fixture.world.ReadCharacterMotionLease(fixture.target);
			context.Expect(current && current->state == CharacterMotionLeaseState::LocomotionOwned
				&& current->owner == secondOwner.owner && current->motionEpoch == 3U,
				"The published lease must belong to the replacement attachment lifetime.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(1U)) < 0.01F,
				"Reacquisition must begin with no velocity inherited from the old attachment.");
		});

		registry.Add("Locomotion.Lease.QueuedBeforeAcquireCannotReplayAfterRelease", [](TestContext& context) {
			LeaseFixture fixture;
			PhysicsCommandMetadata metadata;
			metadata.effectiveTick = 4U;
			metadata.producerId = 17U;
			metadata.producerSequence = 1U;
			const auto queued = fixture.world.EnqueueCommand(
				PhysicsCommand::MakeCharacterMove(fixture.target, {5.0F, 0.0F, 0.0F}, metadata));
			context.Expect(queued.accepted, "The old legacy command should be queued before the first acquisition.");

			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease owned = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, owned}), "The first acquisition should succeed.");
			CharacterMotionLease released = fixture.Unleased(fixture.target);
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			context.Expect(fixture.Publish({owned, released}), "The release should succeed with the queue still occupied.");
			const PhysicsCommandBatch batch = fixture.world.MergeCommands(4U);
			context.ExpectEqual(std::size_t{1U}, batch.commands.size(), "The deferred command must reach the final ingress for a real outcome.");
			context.Expect(!fixture.backend.ApplyCommand(batch.commands.front()).Succeeded(),
				"A command stamped before acquisition must fail after release, even when the character is unleased again.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(4U)) < 0.01F,
				"A rejected old command must not produce physical displacement.");
		});

		registry.Add("Locomotion.Lease.DirectLegacyIngressCannotWriteLeasedCharacter", [](TestContext& context) {
			LeaseFixture fixture;
			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease owned = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, owned}), "The lease fixture must acquire a controller.");
			PhysicsCharacterUpdateRequest direct;
			direct.handle = fixture.target.character;
			direct.objectId = fixture.target.objectId;
			direct.objectGeneration = fixture.target.objectGeneration;
			direct.componentGeneration = fixture.target.componentGeneration;
			direct.desiredVelocity = {5.0F, 0.0F, 0.0F};
			context.Expect(!fixture.backend.UpdateCharacter(direct).Succeeded(),
				"Direct provider-neutral UpdateCharacter must reject a conflicting legacy velocity.");
			PhysicsCommandMetadata metadata;
			metadata.producerId = 18U;
			metadata.producerSequence = 1U;
			auto command = PhysicsCommand::MakeCharacterMove(fixture.target, {5.0F, 0.0F, 0.0F}, metadata);
			command.characterMotionEpoch = owned.motionEpoch;
			context.Expect(!fixture.backend.ApplyCommand(command).Succeeded(),
				"A matching numeric epoch does not authorize a legacy producer to bypass the lease.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(1U)) < 0.01F,
				"Rejected direct and queued ingress must leave the physical controller still.");
		});

		registry.Add("Locomotion.Lease.OldTypedTargetCannotResumeAfterEpochChange", [](TestContext& context) {
			LeaseFixture fixture;
			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease owned = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, owned}), "The first lease should acquire its controller.");
			CharacterMotionTarget target;
			target.controller = fixture.target;
			target.owner = owned.owner;
			target.authorityEpoch = owned.authorityEpoch;
			target.motionEpoch = owned.motionEpoch;
			target.locomotionUpdateEpoch = 1U;
			target.mode = CharacterMotionMode::Velocity;
			target.frame = {kPrimaryPhysicsFrameId, 1U};
			target.planarVelocity = {4.0F, 0.0F, 0.0F};
			target.acceleration = 20.0F;
			target.braking = 25.0F;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(target).Succeeded(),
				"A matching typed target should reach the leased controller.");
			const float controlledX = fixture.AdvanceAndReadX(1U);
			context.Expect(controlledX > 0.0F, "The first typed target should create physical movement.");
			CharacterMotionLease released = unleased;
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			context.Expect(fixture.Publish({owned, released}), "Release should advance the controller motion epoch.");
			context.Expect(!fixture.backend.PublishCharacterMotionTarget(target).Succeeded(),
				"A target copied from the previous owner epoch must not resume movement.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(2U) - controlledX) < 0.01F,
				"Release must clear the typed target's stored velocity before the next physical step.");
		});

		registry.Add("Locomotion.Lease.ExhaustedEpochEmergencyClearStopsMotion", [](TestContext& context) {
			LeaseFixture fixture;
			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			CharacterMotionLease owned = fixture.Owned(fixture.target);
			owned.authorityEpoch = std::numeric_limits<std::uint64_t>::max();
			owned.motionEpoch = std::numeric_limits<std::uint64_t>::max();
			owned.publicationEpoch = std::numeric_limits<std::uint64_t>::max();
			context.Expect(fixture.Publish({unleased, owned}), "The exhaustion fixture must acquire its controller.");
			CharacterMotionTarget moving;
			moving.controller = fixture.target;
			moving.owner = owned.owner;
			moving.authorityEpoch = owned.authorityEpoch;
			moving.motionEpoch = owned.motionEpoch;
			moving.locomotionUpdateEpoch = 1U;
			moving.mode = CharacterMotionMode::Velocity;
			moving.frame = {kPrimaryPhysicsFrameId, 1U};
			moving.planarVelocity = {6.0F, 0.0F, 0.0F};
			moving.acceleration = 20.0F;
			moving.braking = 25.0F;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(moving).Succeeded(),
				"An active typed target must reach the controller before emergency clearing.");
			float movingX = 0.0F;
			for(PhysicsTick tick = 1U; tick <= 10U; ++tick)
				movingX = fixture.AdvanceAndReadX(tick);
			context.Expect(movingX > 0.05F, "The active target must produce physical movement first.");

			// At counter saturation no newer lease can be published. The current
			// owner instead sends a frame-qualified, maximal-update safety clear.
			CharacterMotionTarget stop = moving;
			stop.locomotionUpdateEpoch = std::numeric_limits<std::uint64_t>::max();
			stop.clearImmediately = true;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(stop).Succeeded(),
				"The emergency clear must pass provider validation at the maximal update epoch.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(11U) - movingX) < 0.01F,
				"The emergency clear must remove the active velocity before the next fixed step.");
		});

		registry.Add("Locomotion.Lease.ExternalExclusionSurvivesLocomotionRelease", [](TestContext& context) {
			LeaseFixture fixture;
			const CharacterMotionLease unleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease owned = fixture.Owned(fixture.target);
			context.Expect(fixture.Publish({unleased, owned}), "The lease fixture must acquire a controller.");
			CharacterMotionLease external = fixture.Unleased(fixture.target);
			external.state = CharacterMotionLeaseState::ExternalExclusive;
			external.authorityEpoch = 2U;
			external.motionEpoch = 2U;
			external.publicationEpoch = 2U;
			external.externalOwnerId = 91U;
			context.Expect(fixture.Publish({owned, external}), "External handoff should revoke locomotion authority.");
			CharacterMotionLease released = fixture.Unleased(fixture.target);
			released.authorityEpoch = 3U;
			released.motionEpoch = 3U;
			released.publicationEpoch = 3U;
			context.Expect(!fixture.Publish({external, released}),
				"Ordinary locomotion release must not remove another owner's exclusive exclusion.");
			const auto current = fixture.world.ReadCharacterMotionLease(fixture.target);
			context.Expect(current && current->state == CharacterMotionLeaseState::ExternalExclusive,
				"The provider-neutral authority root must retain the external exclusion.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(1U)) < 0.01F,
				"Suspension must not replay old locomotion or legacy motion.");
		});

		registry.Add("Locomotion.Lease.RejectedBatchAndQueuePressureLeaveAuthorityAtomic", [](TestContext& context) {
			LeaseFixture fixture;
			// Keep the second capsule away from the first. Overlapping controllers
			// would block X movement and obscure the queue-pressure assertion.
			const auto second = fixture.CreateCharacter(10.0F);
			const CharacterMotionLease firstUnleased = fixture.Unleased(fixture.target);
			const CharacterMotionLease secondUnleased = fixture.Unleased(second);
			const CharacterMotionLease firstOwned = fixture.Owned(fixture.target);
			CharacterMotionLease secondOwned = fixture.Owned(second);
			secondOwned.owner.slotIndex = 2U;
			CharacterMotionLease badExpected = secondUnleased;
			badExpected.motionEpoch = 77U;
			const std::array< CharacterMotionLeaseChange, 2U> invalidBatch{{
				{firstUnleased, firstOwned}, {badExpected, secondOwned}}};
			context.Expect(!fixture.backend.PublishCharacterMotionLeases(invalidBatch).Succeeded(),
				"One stale member must reject the entire provider lease batch.");
			const CharacterMotionLeaseResult invalidWorld = fixture.world.PublishCharacterMotionLeases(invalidBatch);
			context.Expect(!invalidWorld.accepted && invalidWorld.error == CharacterMotionLeaseError::StaleEpoch,
				"The invalid member must fail specifically for its stale epoch, before any root publication.");
			context.Expect(fixture.world.ReadCharacterMotionLease(fixture.target)->state == CharacterMotionLeaseState::Unleased,
				"Rejected batch publication must leave the first controller unleased.");

			PhysicsCommandMetadata metadata;
			metadata.effectiveTick = 9U;
			metadata.producerId = 19U;
			metadata.producerSequence = 1U;
			context.Expect(fixture.world.EnqueueCommand(
				PhysicsCommand::MakeCharacterMove(fixture.target, {}, metadata)).accepted,
				"The one-entry legacy command queue should become full.");
			context.Expect(fixture.Publish({firstUnleased, firstOwned}),
				"A safety authority transition must not consume ordinary command-queue capacity.");
			CharacterMotionTarget target;
			target.controller = fixture.target;
			target.owner = firstOwned.owner;
			target.authorityEpoch = firstOwned.authorityEpoch;
			target.motionEpoch = firstOwned.motionEpoch;
			target.locomotionUpdateEpoch = 1U;
			target.mode = CharacterMotionMode::Velocity;
			target.frame = {kPrimaryPhysicsFrameId, 1U};
			target.planarVelocity = {6.0F, 0.0F, 0.0F};
			target.acceleration = 20.0F;
			target.braking = 25.0F;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(target).Succeeded(),
				"The leased target should be accepted while ordinary queue capacity is exhausted.");
			float movingX = 0.0F;
			for(PhysicsTick tick = 1U; tick <= 10U; ++tick)
				movingX = fixture.AdvanceAndReadX(tick);
			context.Expect(movingX > 0.05F, "The leased controller must physically move before revocation.");
			CharacterMotionLease released = firstUnleased;
			released.authorityEpoch = 2U;
			released.motionEpoch = 2U;
			released.publicationEpoch = 2U;
			context.Expect(fixture.Publish({firstOwned, released}),
				"Revocation must succeed while the ordinary command queue remains full.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(11U) - movingX) < 0.01F,
				"Queue pressure must not delay clearing the previously active physical target.");
		});

		registry.Add("Locomotion.Lease.ReusedControllerSlotRejectsOldLifetime", [](TestContext& context) {
			LeaseFixture fixture;
			const PhysicsCharacterCommandTarget old = fixture.target;
			const CharacterMotionLease oldUnleased = fixture.Unleased(old);
			const CharacterMotionLease oldOwned = fixture.Owned(old);
			context.Expect(fixture.Publish({oldUnleased, oldOwned}), "The old controller must be leased before destruction.");
			context.Expect(fixture.backend.DestroyCharacter(old.character).Succeeded()
				&& fixture.world.Release(old.character), "The old controller lifetime must end.");
			fixture.target = fixture.CreateCharacter();
			context.Expect(fixture.target.character.slotIndex == old.character.slotIndex
				&& fixture.target.character.slotGeneration > old.character.slotGeneration,
				"The replacement must reuse the exact slot with a newer generation.");
			const std::array oldTransition{CharacterMotionLeaseChange{oldUnleased, oldOwned}};
			context.Expect(!fixture.backend.PublishCharacterMotionLeases(oldTransition).Succeeded(),
				"A stale lease transition must not attach to a replacement provider character.");
			context.Expect(!fixture.world.PublishCharacterMotionLeases(oldTransition).accepted,
				"The provider-neutral authority root must reject a destroyed controller handle.");
			PhysicsCommandMetadata metadata;
			metadata.producerId = 20U;
			metadata.producerSequence = 1U;
			context.Expect(!fixture.backend.ApplyCommand(PhysicsCommand::MakeCharacterMove(
				old, {7.0F, 0.0F, 0.0F}, metadata)).Succeeded(),
				"A stale command must fail the physical character identity check.");
			context.Expect(std::fabs(fixture.AdvanceAndReadX(1U)) < 0.01F,
				"The replacement controller must remain motionless after stale input.");
		});
	}
}
