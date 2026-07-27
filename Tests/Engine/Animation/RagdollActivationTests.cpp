// RagdollActivationTests.cpp - Complete-pose and protected-boundary regressions.

#include "TestHarness.h"

#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Ragdoll/RagdollPhysicsBridge.h"
#include "Physics/PhysicsSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Animation;

	namespace {
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}
			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		AnimationGuid Guid(std::uint8_t seed) {
			AnimationGuid value;
			value.value.front() = seed;
			value.value.back() = static_cast<std::uint8_t>(seed ^ 0xa5U);
			return value;
		}

		RagdollGenerationStamp ValidGeneration() {
			return {1U, 2U, 3U, 4U, 5};
		}

		PhysicsPoseSnapshot ValidPose() {
			return {ValidGeneration(), {{1U, 10U, {}}}, true};
		}

		RagdollProfileAsset RuntimeProfile(AnimationGeneration generation) {
			RagdollProfileAsset profile;
			profile.assetId = Guid(1U);
			profile.runtimeGeneration = generation;
			profile.skeletonAssetId = Guid(2U);
			profile.skeletonFingerprint = 0x2222U;
			profile.referencePoseHash = 0x3333U;
			profile.physicsProviderFingerprint = 0x4a4f4c54U;
			profile.rootBodyId = 100U;
			profile.allowedCollisionLayerMask = 0x1U;
			RagdollBodyRecord root;
			root.bodyId = 100U;
			root.stableJointId = 10U;
			root.jointToBodyOffset = {0.0F, 0.25F, 0.0F};
			root.massKilograms = 8.0F;
			root.gravityFactor = 0.0F;
			// This fixture isolates activation velocity transfer. Provider damping
			// would legitimately change velocity during the fixed step being tested.
			root.linearDamping = 0.0F;
			root.angularDamping = 0.0F;
			root.collisionLayer = 0U;
			root.collisionMask = 0x1U;
			RagdollColliderRecord rootCollider;
			rootCollider.colliderId = 1000U;
			rootCollider.shape = RagdollColliderShape::Sphere;
			rootCollider.dimensions = {0.25F, 0.25F, 0.25F};
			root.colliders.push_back(rootCollider);
			RagdollBodyRecord child = root;
			child.bodyId = 200U;
			child.stableJointId = 20U;
			child.jointToBodyOffset = {};
			child.massKilograms = 4.0F;
			child.colliders.front().colliderId = 2000U;
			RagdollConstraintRecord constraint;
			constraint.constraintId = 300U;
			constraint.parentBodyId = root.bodyId;
			constraint.childBodyId = child.bodyId;
			constraint.kind = RagdollConstraintKind::SwingTwist;
			// Both constraint anchors begin at the same world point. An inconsistent
			// zero/zero frame would make Jolt correct the initial 0.75 m separation and
			// obscure whether activation transferred the requested velocity exactly.
			constraint.parentFrame.localPosition = {0.0F, 0.75F, 0.0F};
			constraint.motors[3U].mode = RagdollMotorMode::Position;
			constraint.motors[3U].targetPosition = 0.1F;
			constraint.motors[3U].maximumForce = 100.0F;
			profile.bodies = {root, child};
			profile.constraints = {constraint};
			return profile;
		}

		PhysicsPoseSnapshot RuntimePose(const RagdollGenerationStamp& generation) {
			PhysicsPoseSnapshot pose;
			pose.generation = generation;
			pose.completeCurrentStep = true;
			PhysicsPoseBodySnapshot root;
			root.bodyId = 100U;
			root.stableJointId = 10U;
			root.modelPose.translation = {1.0F, 2.0F, 3.0F};
			root.linearVelocity = {4.0F, 5.0F, 6.0F};
			root.angularVelocity = {0.1F, 0.2F, 0.3F};
			PhysicsPoseBodySnapshot child;
			child.bodyId = 200U;
			child.stableJointId = 20U;
			child.modelPose.translation = {1.0F, 3.0F, 3.0F};
			child.linearVelocity = root.linearVelocity;
			child.angularVelocity = root.angularVelocity;
			pose.bodies = {root, child};
			return pose;
		}

		PhysicsPoseSnapshot ProviderVelocityTransferPose(const RagdollGenerationStamp& generation) {
			PhysicsPoseSnapshot pose = RuntimePose(generation);
			// Equal angular velocities at different constraint-frame offsets produce
			// different anchor velocities, so Jolt correctly applies an impulse. This
			// live fixture removes that independent effect and observes linear transfer.
			for(PhysicsPoseBodySnapshot& body : pose.bodies) body.angularVelocity = {};
			return pose;
		}
	}

	void RegisterRagdollActivationTests(TestRegistry& registry) {
		registry.Add("Animation.Ragdoll.ActivationNeedsCompleteGeneration", [](TestContext& context) {
			PhysicsPoseSnapshot pose = ValidPose();
			RagdollActivationPlan plan;
			context.Expect(PrepareRagdollActivation(pose, ValidGeneration(), plan) == AnimationStatus::Ok,
				"One complete generation must create an all-or-nothing activation plan.");
			pose.completeCurrentStep = false;
			context.Expect(PrepareRagdollActivation(pose, ValidGeneration(), plan) != AnimationStatus::Ok,
				"An incomplete physics step must not initialize a ragdoll.");

			RagdollProfileAsset profile = RuntimeProfile(ValidGeneration().profileGeneration);
			PhysicsPoseSnapshot mappedPose = RuntimePose(ValidGeneration());
			context.Expect(PrepareRagdollActivation(mappedPose, profile, ValidGeneration(), plan)
				== AnimationStatus::Ok && plan.bodies.size() == 2U,
				"A profile-compatible complete pose must map every body as one sealed plan.");
			if(plan.bodies.size() == 2U) {
				context.Expect(plan.bodies.front().worldPose.translation.y == 2.25F
					&& plan.bodies.front().linearVelocity == mappedPose.bodies.front().linearVelocity
					&& plan.bodies.front().angularVelocity == mappedPose.bodies.front().angularVelocity,
					"Activation must apply the joint-to-body offset and preserve both velocity channels exactly.");
			}
			mappedPose.bodies.pop_back();
			const RagdollActivationPlan lastGood = plan;
			context.Expect(PrepareRagdollActivation(mappedPose, profile, ValidGeneration(), plan)
				== AnimationStatus::IncompatibleAsset && plan.bodies == lastGood.bodies,
				"A missing final body must reject atomically without replacing the last complete plan.");
		});

		registry.Add("Animation.Ragdoll.ActivationDerivesMappedBodyMotionFromCompletePoseHistory", [](TestContext& context) {
			const RagdollGenerationStamp currentGeneration = ValidGeneration();
			RagdollGenerationStamp previousGeneration = currentGeneration;
			--previousGeneration.publicationGeneration;
			--previousGeneration.fixedStep;
			PhysicsPoseSnapshot previous = RuntimePose(previousGeneration);
			PhysicsPoseSnapshot current = RuntimePose(currentGeneration);
			for(PhysicsPoseBodySnapshot& body : current.bodies) {
				body.modelPose.translation.x += 1.0F;
				body.modelPose.rotation = {0.0F, 0.0F, 0.70710677F, 0.70710677F};
			}
			RagdollActivationPlan plan;
			const float elapsedSeconds = 1.0F / static_cast<float>(kAnimationFixedTicksPerSecond);
			context.Expect(PrepareRagdollActivation(current, previous, elapsedSeconds,
				RuntimeProfile(currentGeneration.profileGeneration), currentGeneration, plan)
				== AnimationStatus::Ok && plan.bodies.size() == 2U,
				"Two compatible complete animation generations must derive one atomic moving activation plan.");
			if(plan.bodies.size() == 2U) {
				context.Expect(std::abs(plan.bodies[1U].linearVelocity.x - 60.0F) <= 0.001F
					&& std::abs(plan.bodies[1U].angularVelocity.z - 94.24778F) <= 0.01F,
					"Mapped body origins must retain translation and shortest-arc angular motion at activation.");
			}

			const RagdollActivationPlan lastGood = plan;
			previous.generation.worldGeneration += 1U;
			context.Expect(PrepareRagdollActivation(current, previous, elapsedSeconds,
				RuntimeProfile(currentGeneration.profileGeneration), currentGeneration, plan)
				== AnimationStatus::StaleHandle && plan.bodies == lastGood.bodies,
				"A stale historical pose must reject without replacing the last complete moving plan.");
		});

		registry.Add("Animation.Ragdoll.PhysicsPoseSnapshotsRejectStaleWorldBodyAndProfileGenerations", [](TestContext& context) {
			RagdollPhysicsBridge bridge;
			const RagdollGenerationStamp expected = ValidGeneration();
			const std::array<PhysicsPoseExpectedBodyGeneration, 1U> bodies{{{1U, 1U}}};
			const PhysicsPoseSnapshot pose = ValidPose();
			context.Expect(bridge.PublishCurrentStepPose(77U, pose, expected, bodies) == AnimationStatus::Ok,
				"A complete current-step pose with matching world, profile, and body generations must publish.");
			const auto published = bridge.AcquireCurrentStepPose(77U, expected);
			context.Expect(published != nullptr && published->generation.fixedStep == expected.fixedStep,
				"Published ragdoll poses must be retained as immutable current-step records.");

			PhysicsPoseSnapshot staleProfile = pose;
			staleProfile.generation.profileGeneration += 1U;
			context.Expect(bridge.PublishCurrentStepPose(77U, staleProfile, expected, bodies) == AnimationStatus::StaleHandle,
				"A stale profile generation must reject before replacing the current pose.");
			PhysicsPoseSnapshot staleBody = pose;
			staleBody.bodies.front().bodyGeneration += 1U;
			context.Expect(bridge.PublishCurrentStepPose(77U, staleBody, expected, bodies) == AnimationStatus::StaleHandle,
				"A stale provider body generation must reject before publication.");
			context.Expect(bridge.AcquireCurrentStepPose(77U, expected) == published,
				"Rejected snapshots must leave the prior immutable record untouched.");
			RagdollGenerationStamp nextGeneration = expected;
			++nextGeneration.fixedStep;
			PhysicsPoseSnapshot nextPose = pose;
			nextPose.generation = nextGeneration;
			const AnimationStatus nextStatus = bridge.PublishCurrentStepPose(
				77U, nextPose, nextGeneration, bodies);
			const auto latest = bridge.AcquireLatestCurrentStepPose(77U, expected);
			context.Expect(nextStatus == AnimationStatus::Ok
				&& latest && latest->generation == nextGeneration,
				"Animation must be able to advance from an older observed tick to the latest compatible immutable step.");
		});

		registry.Add("Animation.Ragdoll.PhysicsBoundaryRejectsIncompleteProviderBindingAtomically", [](TestContext& context) {
			RagdollPhysicsBridge bridge;
			RagdollPhysicsRequest activate;
			activate.kind = RagdollPhysicsRequestKind::Activate;
			activate.ragdollId = 77U;
			activate.generation = ValidGeneration();
			activate.activationPose = ValidPose();
			activate.correlationId = 2U;
			activate.producerOrder = 2U;
			RagdollPhysicsRequest teardown = activate;
			teardown.kind = RagdollPhysicsRequestKind::Teardown;
			teardown.correlationId = 1U;
			teardown.producerOrder = 1U;
			context.Expect(bridge.Enqueue(activate) == AnimationStatus::Queued
				&& bridge.Enqueue(teardown) == AnimationStatus::Queued,
				"Non-protected ragdoll producers must only enqueue copied typed requests.");

			std::vector<RagdollPhysicsRequest> drained;
			context.Expect(bridge.Drain(drained) == AnimationStatus::Ok && drained.size() == 2U
				&& drained.front().correlationId == 1U,
				"The protected boundary must deterministically drain producer requests.");
			context.Expect(bridge.RejectBatchAtProtectedBoundary(drained, ValidGeneration().fixedStep,
				"fixture lacks a resolved provider binding") == AnimationStatus::Rejected,
				"An incomplete provider binding must reject the complete lifecycle batch.");
			const auto results = bridge.LatestResults();
			context.Expect(results != nullptr && results->results != nullptr && results->results->size() == 2U
				&& std::all_of(results->results->begin(), results->results->end(), [](const RagdollPhysicsResult& result) {
					return result.status == AnimationStatus::Rejected;
				}), "Create, activation, and teardown batch rejection must not expose a partial lifecycle transition.");
		});

		registry.Add("Animation.Ragdoll.LiveJoltLifecycleCommitsAndPublishesOneCompleteGeneration", [](TestContext& context) {
			ThreadPoolSession pool;
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The lifecycle regression needs one isolated Jolt world.");
			const auto world = physics.WorldHandle();
			RagdollGenerationStamp created{world.worldGeneration, 2U, 3U, 4U, 1};
			auto profile = std::make_shared<const RagdollProfileAsset>(RuntimeProfile(created.profileGeneration));
			RagdollPhysicsRequest create;
			create.kind = RagdollPhysicsRequestKind::Create;
			create.ragdollId = 77U;
			create.generation = created;
			create.profile = profile;
			create.activationPose = ProviderVelocityTransferPose(created);
			create.correlationId = 1U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(create) == AnimationStatus::Queued,
				"Create must enter only the typed deferred queue from producer code.");
			physics.Update(0.0F);
			auto results = physics.LatestRagdollPhysicsResults();
			context.Expect(results && results->results && results->results->size() == 1U
				&& results->results->front().status == AnimationStatus::Ok,
				"The protected boundary must atomically commit every profile body and constraint.");

			RagdollGenerationStamp activated = created;
			activated.publicationGeneration += 1U;
			RagdollPhysicsRequest activate;
			activate.kind = RagdollPhysicsRequestKind::Activate;
			activate.ragdollId = create.ragdollId;
			activate.generation = activated;
			activate.expectedCurrentGeneration = created;
			activate.activationPose = ProviderVelocityTransferPose(activated);
			activate.correlationId = 2U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(activate) == AnimationStatus::Queued,
				"Activation must remain deferred until PhysicsCommandCommit.");
			physics.Update(physics.FixedTimeStep());
			const auto pose = physics.LatestRagdollPhysicsPose(activate.ragdollId, activated);
			context.Expect(pose && pose->completeCurrentStep && pose->bodies.size() == profile->bodies.size(),
				"The same fixed step must publish one immutable complete provider pose for animation presentation.");
			if(pose && !pose->bodies.empty()) {
				context.Expect(pose->bodies.front().linearVelocity == activate.activationPose.bodies.front().linearVelocity,
					"Provider extraction must preserve activation motion when gravity, damping, and constraint error are absent.");
				context.Expect(pose->bodies.front().bodyGeneration != 0U,
					"Provider extraction must expose the current non-zero body-slot generation.");
			}

			RagdollPhysicsRequest motors;
			motors.kind = RagdollPhysicsRequestKind::UpdateMotors;
			motors.ragdollId = activate.ragdollId;
			motors.generation = activated;
			motors.expectedCurrentGeneration = activated;
			motors.motorDrives.push_back({profile->constraints.front().constraintId, 0.5F});
			motors.correlationId = 25U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(motors) == AnimationStatus::Queued,
				"Animation motor output must cross only the copied typed physics queue.");
			physics.Update(0.0F);
			results = physics.LatestRagdollPhysicsResults();
			context.Expect(results && results->results && results->results->size() == 1U
				&& results->results->front().kind == RagdollPhysicsRequestKind::UpdateMotors
				&& results->results->front().status == AnimationStatus::Ok,
				"The protected boundary must apply authored motor targets with the animation drive scale.");

			RagdollPhysicsRequest staleTeardown;
			staleTeardown.kind = RagdollPhysicsRequestKind::Teardown;
			staleTeardown.ragdollId = activate.ragdollId;
			staleTeardown.generation = activated;
			staleTeardown.expectedCurrentGeneration = created;
			staleTeardown.correlationId = 3U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(staleTeardown) == AnimationStatus::Queued,
				"A stale command is admitted to the queue so the protected boundary can reject it consistently.");
			physics.Update(0.0F);
			results = physics.LatestRagdollPhysicsResults();
			context.Expect(results && results->results && results->results->front().status == AnimationStatus::Rejected
				&& physics.LatestRagdollPhysicsPose(activate.ragdollId, activated) == pose,
				"A stale lifecycle generation must reject the whole batch and preserve the last complete pose.");

			RagdollPhysicsRequest teardown = staleTeardown;
			teardown.expectedCurrentGeneration = activated;
			teardown.correlationId = 4U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(teardown) == AnimationStatus::Queued,
				"The current generation must accept teardown through the same deferred path.");
			physics.Update(0.0F);
			results = physics.LatestRagdollPhysicsResults();
			context.Expect(results && results->results && results->results->front().status == AnimationStatus::Ok
				&& physics.LatestRagdollPhysicsPose(activate.ragdollId, activated) == nullptr,
				"Teardown must retire constraints before bodies and remove the immutable pose atomically.");
			context.Expect(physics.StopWorld(), "The isolated ragdoll world must tear down cleanly.");
			physics.Shutdown();
		});

		registry.Add("Animation.Ragdoll.PhysicsClockStampSurvivesFarAheadEditorFrames", [](TestContext& context) {
			ThreadPoolSession pool;
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The clock-domain regression needs one isolated Jolt world.");
			const auto world = physics.WorldHandle();
			constexpr AnimationFixedTick kSimulatedEditorFrameTick = 10000;
			const AnimationFixedTick physicsTick = physics.CurrentFixedTick();
			context.Expect(physicsTick > 0 && physicsTick != kSimulatedEditorFrameTick,
				"A newborn physics world must provide its own valid tick instead of inheriting far-ahead editor frames.");

			RagdollGenerationStamp created{world.worldGeneration, 2U, 3U, 4U, physicsTick};
			auto profile = std::make_shared<const RagdollProfileAsset>(RuntimeProfile(created.profileGeneration));
			RagdollPhysicsRequest create;
			create.kind = RagdollPhysicsRequestKind::Create;
			create.ragdollId = 78U;
			create.generation = created;
			create.profile = profile;
			create.activationPose = ProviderVelocityTransferPose(created);
			create.correlationId = 101U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(create) == AnimationStatus::Queued,
				"Creation must retain the physics-clock generation through the deferred bridge.");
			physics.Update(0.0F);

			RagdollGenerationStamp activation = created;
			++activation.publicationGeneration;
			// This intentionally does not use kSimulatedEditorFrameTick. It models
			// activation after a long editor preview before Physics has stepped.
			activation.fixedStep = physics.CurrentFixedTick();
			RagdollPhysicsRequest activate;
			activate.kind = RagdollPhysicsRequestKind::Activate;
			activate.ragdollId = create.ragdollId;
			activate.generation = activation;
			activate.expectedCurrentGeneration = created;
			activate.activationPose = ProviderVelocityTransferPose(activation);
			activate.correlationId = 102U;
			context.Expect(physics.SubmitRagdollPhysicsRequest(activate) == AnimationStatus::Queued,
				"Activation must retain the same copied physics-clock generation.");
			physics.Update(physics.FixedTimeStep());
			const auto recoveryPose = physics.LatestRagdollPhysicsPoseAtOrAfter(
				activate.ragdollId, activation);
			context.Expect(recoveryPose != nullptr
				&& recoveryPose->generation.fixedStep >= activation.fixedStep,
				"The first physics extraction after far-ahead editor frames must remain available for recovery.");
			context.Expect(physics.StopWorld(), "The clock-domain regression world must tear down cleanly.");
			physics.Shutdown();
		});

		registry.Add("Animation.Ragdoll.PhysicsSchedulerDeclaresProtectedBridgeBoundary", [](TestContext& context) {
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			const auto declaration = physics.SchedulerDeclaration();
			context.Expect(std::find(declaration.reads.begin(), declaration.reads.end(), "RagdollPhysicsQueue") != declaration.reads.end()
				&& std::find(declaration.writes.begin(), declaration.writes.end(), "RagdollPhysicsSnapshots") != declaration.writes.end(),
				"Physics diagnostics must expose the ragdoll queue and immutable snapshot resources.");
			const auto workloads = physics.DeclareFrameWorkloads(0.0F);
			const auto commit = std::find_if(workloads.begin(), workloads.end(), [](const auto& workload) {
				return workload.workloadName == "PhysicsCommandCommit";
			});
			context.Expect(commit != workloads.end() && commit->mainThreadRequired
				&& std::find(commit->reads.begin(), commit->reads.end(), "RagdollPhysicsQueue") != commit->reads.end(),
				"Only the declared protected command-commit workload may drain ragdoll requests.");
		});
	}
}
