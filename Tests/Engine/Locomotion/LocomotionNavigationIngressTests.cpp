// LocomotionNavigationIngressTests.cpp - final Physics ingress and budget tests.
//
// A fake immutable-evidence guard lets these tests revoke guidance after a
// target is accepted but before the next protected character step.

#include "../TestHarness.h"

#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		struct MutableEvidenceGuard final : CharacterMotionValidity {
			std::atomic<bool> valid{true};
			mutable std::atomic<std::uint32_t> checks{0U};

			CharacterMotionValidityReason Validate() const noexcept override {
				checks.fetch_add(1U, std::memory_order_relaxed);
				return valid.load(std::memory_order_relaxed)
					? CharacterMotionValidityReason::Valid : CharacterMotionValidityReason::StaleEvidence;
			}
		};

		struct ProtectedIngressFixture final {
			Object object;
			PhysicsCharacterCommandTarget controller;
			CharacterMotionLease lease;

			ProtectedIngressFixture() {
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
				object = objects.createObject("Guarded character ingress");
				if(objects.AddComponent<Transform>(object) == nullptr)
					throw std::runtime_error("Guard fixture Transform creation failed.");
				auto* character = objects.AddComponent<CharacterControllerComponent>(object);
				if(character == nullptr) throw std::runtime_error("Guard fixture controller creation failed.");
				character->useGravity = false;
				if(!physics.StartWorld()) throw std::runtime_error("Guard fixture Physics world start failed.");
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) Step();
				const auto packet = physics.LatestCharacterStatePacket();
				const UUID objectId = objects.getUUID(object);
				if(!packet || !packet->states) throw std::runtime_error("Guard fixture character packet missing.");
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[&objectId](const PhysicsCharacterState& state) { return state.objectId == objectId; });
				if(found == packet->states->end()) throw std::runtime_error("Guard fixture controller was not published.");
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
				const std::array changes{CharacterMotionLeaseChange{unleased, lease}};
				if(!physics.PublishCharacterMotionLeases(changes).accepted)
					throw std::runtime_error("Guard fixture controller acquisition failed.");
			}

			~ProtectedIngressFixture() {
				auto& physics = PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			void Step() {
				auto& objects = ObjectManager::instance();
				auto& physics = PhysicsSystem::Instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(physics.FixedTimeStep());
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "locomotion-navigation-ingress-physics-flush");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			float X() const {
				const auto packet = PhysicsSystem::Instance().LatestCharacterStatePacket();
				if(!packet || !packet->states) throw std::runtime_error("Guard fixture has no current state packet.");
				const auto found = std::find_if(packet->states->begin(), packet->states->end(),
					[this](const PhysicsCharacterState& state) { return state.handle == controller.character; });
				if(found == packet->states->end()) throw std::runtime_error("Guard fixture lost its controller.");
				return found->pose.position.x;
			}

			CharacterMotionTarget Target(std::uint64_t updateEpoch,
				std::shared_ptr<const CharacterMotionValidity> guard) const {
				CharacterMotionTarget target;
				target.controller = controller;
				target.owner = lease.owner;
				target.authorityEpoch = lease.authorityEpoch;
				target.motionEpoch = lease.motionEpoch;
				target.locomotionUpdateEpoch = updateEpoch;
				target.mode = CharacterMotionMode::Velocity;
				target.frame = {kPrimaryPhysicsFrameId, controller.world.worldGeneration};
				target.planarVelocity = {4.0F, 0.0F, 0.0F};
				target.acceleration = 100.0F;
				target.braking = 100.0F;
				target.validityGuard = std::move(guard);
				return target;
			}
		};

		struct BudgetFixture final {
			JoltPhysicsBackend backend{ThreadPool::instance()};
			PhysicsWorldHandle world{{74U}, 1U};
			PhysicsCharacterCommandTarget controller;
			CharacterMotionLease lease;

			BudgetFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				if(!backend.Initialize().Succeeded()) throw std::runtime_error("Budget fixture backend initialization failed.");
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.frame = {kPrimaryPhysicsFrameId, 1U};
				if(!backend.CreateWorld(configuration).Succeeded()) throw std::runtime_error("Budget fixture world creation failed.");
				controller.world = world;
				controller.character = {{74U}, 1U, 8U, 1U};
				controller.objectId = TerraEngine::GenerateUUID();
				controller.objectGeneration = 1U;
				controller.componentGeneration = 1U;
				PhysicsCharacterCreateRequest request;
				request.handle = controller.character;
				request.objectId = controller.objectId;
				request.objectGeneration = controller.objectGeneration;
				request.componentGeneration = controller.componentGeneration;
				request.pose.position = {0.0F, 1.1F, 0.0F};
				request.useGravity = false;
				PhysicsBackendStagingToken staging;
				if(!backend.CreateCharacter(request, staging).Succeeded()
					|| !backend.CommitStaging(staging).Succeeded())
					throw std::runtime_error("Budget fixture controller creation failed.");
				CharacterMotionLease unleased;
				unleased.controller = controller;
				lease = unleased;
				lease.owner.sceneLifetime = 1U;
				lease.owner.objectId = controller.objectId;
				lease.owner.objectGeneration = 1U;
				lease.owner.attachmentGeneration = 1U;
				lease.owner.slotIndex = 1U;
				lease.owner.slotGeneration = 1U;
				lease.authorityEpoch = 1U;
				lease.motionEpoch = 1U;
				lease.publicationEpoch = 1U;
				lease.state = CharacterMotionLeaseState::LocomotionOwned;
				const std::array changes{CharacterMotionLeaseChange{unleased, lease}};
				if(!backend.PublishCharacterMotionLeases(changes).Succeeded())
					throw std::runtime_error("Budget fixture lease acquisition failed.");
			}

			~BudgetFixture() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
				ThreadPool::instance().shutdown();
			}

			PhysicsBackendCharacterState Step(PhysicsTick tick) {
				constexpr float duration = 1.0F / 60.0F;
				if(!backend.AdvanceCharacters(world, tick, duration).Succeeded()
					|| !backend.Step({world, tick, duration, 1U}).Succeeded())
					throw std::runtime_error("Budget fixture fixed step failed.");
				PhysicsBackendExtraction extraction;
				extraction.world = world;
				if(!backend.Extract(extraction).Succeeded()) throw std::runtime_error("Budget fixture extraction failed.");
				const auto found = std::find_if(extraction.characters.begin(), extraction.characters.end(),
					[this](const PhysicsBackendCharacterState& state) { return state.handle == controller.character; });
				if(found == extraction.characters.end()) throw std::runtime_error("Budget fixture controller missing.");
				return *found;
			}
		};
	}

	void RegisterLocomotionNavigationIngressTests(TestRegistry& registry) {
		registry.Add("Locomotion.NavigationIngress.StationaryCharacterSkipsTransformMutation", [](TestContext& context) {
			ProtectedIngressFixture fixture;
			auto& physics = PhysicsSystem::Instance();
			auto& objects = ObjectManager::instance();
			const auto initial = physics.LatestCharacterStatePacket();
			if(!initial || !initial->states || initial->states->empty())
				throw std::runtime_error("Stationary character state packet missing.");
			const PhysicsPose initialPose = initial->states->front().pose;
			const PhysicsTick initialTick = initial->tick;
			const std::uint64_t initialRevision = objects.GetObjectTransformRevision(fixture.object);
			for(std::uint32_t step = 0U; step < 4U; ++step) fixture.Step();
			const auto stopped = physics.LatestCharacterStatePacket();
			if(!stopped || !stopped->states || stopped->states->empty())
				throw std::runtime_error("Stationary character state packet did not advance.");
			const PhysicsPose stoppedPose = stopped->states->front().pose;
			const bool samePose = stoppedPose.position.x == initialPose.position.x
				&& stoppedPose.position.y == initialPose.position.y
				&& stoppedPose.position.z == initialPose.position.z
				&& stoppedPose.rotation.x == initialPose.rotation.x
				&& stoppedPose.rotation.y == initialPose.rotation.y
				&& stoppedPose.rotation.z == initialPose.rotation.z
				&& stoppedPose.rotation.w == initialPose.rotation.w;
			context.Expect(samePose && stopped->tick > initialTick
				&& stopped->states->front().completedStepDuration > 0.0F,
				"A stationary controller must still publish completed fixed-tick observations without changing its world pose.");
			context.Expect(objects.GetObjectTransformRevision(fixture.object) == initialRevision,
				"An unchanged character pose must not enqueue a redundant Transform mutation or advance the source revision.");
			CharacterMotionTarget moving = fixture.Target(1U, {});
			moving.hasFacing = true;
			moving.desiredFacing = {1.0F, 0.0F, 0.0F};
			moving.turnRate = 1.0F;
			context.Expect(physics.PublishCharacterMotionTarget(moving).Succeeded(),
				"The leased character must accept planar movement and a gradual yaw target.");
			// Repeated small Jolt yaw increments exercise the near-unit quaternion
			// produced by a real moving character, not only identity orientation.
			for(std::uint32_t step = 0U; step < 100U; ++step) fixture.Step();
			context.Expect(fixture.X() > initialPose.position.x + 0.05F
				&& objects.GetObjectTransformRevision(fixture.object) > initialRevision,
				"Real controller displacement must still publish a Transform mutation and advance the source revision.");
			CharacterMotionTarget clear = fixture.Target(2U, {});
			clear.planarVelocity = {};
			clear.clearImmediately = true;
			context.Expect(physics.PublishCharacterMotionTarget(clear).Succeeded(),
				"An explicit stop must clear the prior physical velocity before testing settled publication.");
			fixture.Step();
			const auto settled = physics.LatestCharacterStatePacket();
			if(!settled || !settled->states || settled->states->empty())
				throw std::runtime_error("Stopped character state packet missing.");
			const PhysicsPose settledPose = settled->states->front().pose;
			const PhysicsTick settledTick = settled->tick;
			const std::uint64_t settledRevision = objects.GetObjectTransformRevision(fixture.object);
			for(std::uint32_t step = 0U; step < 4U; ++step) fixture.Step();
			const auto later = physics.LatestCharacterStatePacket();
			if(!later || !later->states || later->states->empty())
				throw std::runtime_error("Stopped character state packet did not advance.");
			const PhysicsPose laterPose = later->states->front().pose;
			const bool stillSettled = laterPose.position.x == settledPose.position.x
				&& laterPose.position.y == settledPose.position.y
				&& laterPose.position.z == settledPose.position.z
				&& laterPose.rotation.x == settledPose.rotation.x
				&& laterPose.rotation.y == settledPose.rotation.y
				&& laterPose.rotation.z == settledPose.rotation.z
				&& laterPose.rotation.w == settledPose.rotation.w;
			context.Expect(stillSettled && later->tick > settledTick
				&& later->states->front().completedStepDuration > 0.0F,
				"The stopped controller must keep publishing physical observations without further pose change.");
			context.Expect(objects.GetObjectTransformRevision(fixture.object) == settledRevision,
				"A settled post-movement controller must not republish its unchanged Transform.");
		});

		registry.Add("Locomotion.NavigationIngress.GuardReadsStayProtected", [](TestContext& context) {
			ProtectedIngressFixture fixture;
			auto guard = std::make_shared<MutableEvidenceGuard>();
			context.Expect(PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(1U, guard)).Succeeded(),
				"A guarded target must be active for the scheduling declaration check.");
			const auto declaration = PhysicsSystem::Instance().SchedulerDeclaration();
			const auto workloads = PhysicsSystem::Instance().DeclareFrameWorkloads(
				PhysicsSystem::Instance().FixedTimeStep());
			const auto commit = std::find_if(workloads.begin(), workloads.end(), [](const auto& workload) {
				return workload.workloadName == "PhysicsCommandCommit";
			});
			const auto simulate = std::find_if(workloads.begin(), workloads.end(), [](const auto& workload) {
				return workload.workloadName == "PhysicsSimulate";
			});
			const std::array<const char*, 4U> guardReads{
				TerraEngine::Scheduling::kResourceSceneChangePublication,
				TerraEngine::Scheduling::kResourceNavigationAgentState,
				TerraEngine::Scheduling::kResourceNavigationAgentGuidance,
				TerraEngine::Scheduling::kResourceNavigationRouteValidity};
			bool declared = commit != workloads.end() && simulate != workloads.end()
				&& commit->active && !commit->workerEligible && simulate->workerEligible;
			for(const char* resource : guardReads) {
				declared = declared
					&& std::find(declaration.reads.begin(), declaration.reads.end(), resource) != declaration.reads.end()
					&& commit != workloads.end()
					&& std::find(commit->reads.begin(), commit->reads.end(), resource) != commit->reads.end()
					&& simulate != workloads.end()
					&& std::find(simulate->reads.begin(), simulate->reads.end(), resource) == simulate->reads.end();
			}
			context.Expect(declared,
				"Live guidance rechecks must remain in protected PhysicsCommandCommit, never the worker-safe solver workload.");
		});

		registry.Add("Locomotion.NavigationIngress.RevokedBetweenPublishAndStepCannotMove", [](TestContext& context) {
			ProtectedIngressFixture fixture;
			auto guard = std::make_shared<MutableEvidenceGuard>();
			context.Expect(PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(1U, guard)).Succeeded(),
				"A valid guarded target should reach the leased controller.");
			const float before = fixture.X();
			guard->valid.store(false, std::memory_order_relaxed);
			fixture.Step();
			context.Expect(std::fabs(fixture.X() - before) < 0.01F && guard->checks.load() >= 2U,
				"Physics must recheck copied guidance before its first fixed step and clear an invalid target.");
		});

		registry.Add("Locomotion.NavigationIngress.InvalidGuidanceNeverAutoResumes", [](TestContext& context) {
			ProtectedIngressFixture fixture;
			auto guard = std::make_shared<MutableEvidenceGuard>();
			context.Expect(PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(1U, guard)).Succeeded(),
				"The initial valid guarded target should be accepted.");
			for(std::uint32_t tick = 0U; tick < 5U; ++tick) fixture.Step();
			const float movingX = fixture.X();
			context.Expect(movingX > 0.05F, "The first guarded target must physically move before invalidation.");
			guard->valid.store(false, std::memory_order_relaxed);
			fixture.Step();
			const float stoppedX = fixture.X();
			context.Expect(std::fabs(stoppedX - movingX) < 0.01F,
				"A stale guard must neutralize the active physical contribution before the next step.");
			guard->valid.store(true, std::memory_order_relaxed);
			fixture.Step();
			context.Expect(std::fabs(fixture.X() - stoppedX) < 0.01F,
				"Revalidated evidence alone must not resurrect a target that was already neutralized.");
			context.Expect(PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(2U, guard)).Succeeded(),
				"A fresh target with a later update epoch may resume physical movement.");
			fixture.Step();
			context.Expect(fixture.X() > stoppedX + 0.01F,
				"Only that newly published target may move the controller again.");
		});

		registry.Add("Locomotion.NavigationIngress.RejectedGuardAndLegacyCannotReplaceLease", [](TestContext& context) {
			ProtectedIngressFixture fixture;
			auto live = std::make_shared<MutableEvidenceGuard>();
			context.Expect(PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(1U, live)).Succeeded(),
				"The retained valid target must be published first.");
			auto stale = std::make_shared<MutableEvidenceGuard>();
			stale->valid.store(false, std::memory_order_relaxed);
			context.Expect(!PhysicsSystem::Instance().PublishCharacterMotionTarget(fixture.Target(2U, stale)).Succeeded(),
				"A stale candidate guard must reject before replacing the valid physical target.");
			PhysicsCommandMetadata metadata;
			metadata.producerId = 91U;
			metadata.producerSequence = 1U;
			const auto legacy = PhysicsSystem::Instance().SubmitCommand(
				PhysicsCommand::MakeCharacterMove(fixture.controller, {-8.0F, 0.0F, 0.0F}, metadata));
			context.Expect(legacy.accepted,
				"The ordinary legacy queue may admit a command before final authority rejection.");
			const float before = fixture.X();
			fixture.Step();
			context.Expect(fixture.X() > before && live->valid.load(std::memory_order_relaxed),
				"Rejected guarded and conflicting legacy writes must leave the prior valid leased target moving.");
		});

		registry.Add("Locomotion.NavigationIngress.ApproachBudgetClipsEveryFixedStep", [](TestContext& context) {
			BudgetFixture fixture;
			CharacterMotionTarget target;
			target.controller = fixture.controller;
			target.owner = fixture.lease.owner;
			target.authorityEpoch = fixture.lease.authorityEpoch;
			target.motionEpoch = fixture.lease.motionEpoch;
			target.locomotionUpdateEpoch = 1U;
			target.mode = CharacterMotionMode::Velocity;
			target.frame = {kPrimaryPhysicsFrameId, 1U};
			target.planarVelocity = {10.0F, 0.0F, 0.0F};
			target.acceleration = 1000.0F;
			target.braking = 1000.0F;
			target.approachDistanceMetres = 0.22F;
			context.Expect(fixture.backend.PublishCharacterMotionTarget(target).Succeeded(),
				"A route target with a finite remaining approach budget should be admitted.");
			PhysicsBackendCharacterState state;
			for(PhysicsTick tick = 1U; tick <= 5U; ++tick) state = fixture.Step(tick);
			context.Expect(state.pose.position.x > 0.19F && state.pose.position.x <= 0.23F,
				"Several fixed steps may approach, but never exceed, the 0.22-metre route budget.");
			context.Expect(std::fabs(state.realizedDisplacement.x) < 0.01F
				&& std::fabs(state.requestedPlanarVelocity.x) < 0.01F,
				"Once spent, the target must contribute zero additional motion on quiet fixed steps.");
		});
	}
}
