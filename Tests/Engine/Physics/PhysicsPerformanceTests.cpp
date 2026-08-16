// PhysicsPerformanceTests.cpp - deterministic runtime performance-regression contracts.
//
// These tests intentionally verify work counts and published telemetry instead of
// comparing wall-clock durations. ImplementationCheck and ImplementationCheck record machine-specific
// RelWithDebInfo budgets; a unit-test clock threshold would be noisy on CI.

#include "../TestHarness.h"

#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace TerraEngine::Tests {
	namespace {
		constexpr std::size_t kThroughputBodyCount = 64U;
		constexpr std::uint32_t kFixedTickCount = 60U;

		Scene::WorldPose PoseAt(float x, float y, float z) {
			Scene::WorldPose pose;
			pose.position.offsetMetres = {x, y, z};
			return pose;
		}

		bool HasPose(const ObjectManager& objects, const Object& object, float x, float y, float z) {
			const auto pose = objects.GetCanonicalWorldPose(object);
			return pose && pose->position.offsetMetres == glm::dvec3{x, y, z};
		}

		// The shared pool is initialized only for this isolated runtime fixture so
		// fixed-tick work cannot inherit jobs or workers from another test.
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				TerraEngine::Scheduling::SetSameFramePhysicsPublicationConsumerActive(false);
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		// This cleanup guard protects later test collections when an assertion fails
		// while the fixture owns the singleton physics world or authored objects.
		class PhysicsPerformanceCleanup final {
		public:
			~PhysicsPerformanceCleanup() {
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Physics::PublishedPhysicsDebugPackets().Clear();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
			}
		};

		// Scheduled physics tests register lightweight neighbors so the real Physics
		// declaration can keep its authored Scripting -> Physics -> UI dependencies.
		class PhysicsNeighborSystem final : public SystemsBase {
		public:
			explicit PhysicsNeighborSystem(TerraEngine::Scheduling::SystemDeclaration declaration)
				: declaration(std::move(declaration)) {
			}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
			std::string SystemName() override { return declaration.name; }
			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override { return declaration; }

		private:
			TerraEngine::Scheduling::SystemDeclaration declaration;
		};

		void AddDynamicFixtureBodies(TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			for(std::size_t index = 0U; index < kThroughputBodyCount; ++index) {
				const Object object = objects.createObject("Physics Performance Body");
				Transform* transform = objects.AddComponent<Transform>(object);
				RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
				ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
				context.Expect(transform != nullptr && body != nullptr && colliders != nullptr,
					"The fixed-tick fixture must create every dynamic-body component.");
				(void)objects.SetCanonicalWorldPose(object, PoseAt(static_cast<float>(index % 8U),
					4.0F + static_cast<float>(index / 8U), static_cast<float>((index / 8U) % 8U)));
				body->useGravity = true;
				colliders->Add();
				objects.MarkObjectTransformDirty(object);
			}
		}

		bool ContainsWorkload(
			const std::vector<Physics::PhysicsSchedulerDeclaration>& declarations,
			const char* workload,
			const char* boundary) {
			return std::any_of(declarations.begin(), declarations.end(), [workload, boundary](const auto& declaration) {
				return declaration.workload == workload && declaration.boundary == boundary;
			});
		}

		// This helper creates valid immutable events without a live provider. It
		// isolates packet storage behavior from simulation timing and allocation noise.
		Physics::PhysicsEvent MakePerformanceEvent(std::uint32_t slot) {
			Physics::PhysicsEvent event;
			event.world = {{8U}, 1U};
			event.tick = 1U;
			event.first.objectId[15] = static_cast<std::uint8_t>(slot);
			event.first.objectGeneration = 1U;
			event.first.componentGeneration = 1U;
			event.first.colliderRecordId[15] = static_cast<std::uint8_t>(slot);
			event.first.recordGeneration = 1U;
			event.first.body = {{8U}, 1U, slot, 1U};
			event.hasSecondParticipant = false;
			return event;
		}

		std::uint64_t HashRagdollPoseWork(
			std::size_t ragdollCount,
			std::size_t bodiesPerRagdoll,
			std::uint64_t unrelatedStaticCount,
			std::uint64_t& visitedBodies) {
			std::uint64_t hash = 1469598103934665603ULL;
			visitedBodies = 0U;
			for(std::size_t ragdoll = 0U; ragdoll < ragdollCount; ++ragdoll) {
				Animation::RagdollGenerationStamp generation{1U, 2U, 3U, 4U,
					static_cast<Animation::AnimationFixedTick>(ragdoll + 1U)};
				Animation::PhysicsPoseSnapshot pose;
				pose.generation = generation;
				pose.completeCurrentStep = true;
				pose.bodies.reserve(bodiesPerRagdoll);
				std::vector<Animation::PhysicsPoseExpectedBodyGeneration> expected;
				expected.reserve(bodiesPerRagdoll);
				for(std::size_t body = 0U; body < bodiesPerRagdoll; ++body) {
					const std::uint64_t identity = 1U + ragdoll * bodiesPerRagdoll + body;
					Animation::PhysicsPoseBodySnapshot state;
					state.bodyId = identity;
					state.stableJointId = identity + 100000U;
					state.bodyGeneration = 1U;
					state.modelPose.scale = {1.0F, 1.0F, 1.0F};
					pose.bodies.push_back(state);
					expected.push_back({identity, 1U});
					++visitedBodies;
					hash ^= identity;
					hash *= 1099511628211ULL;
				}
				if(Animation::ValidatePhysicsPoseSnapshot(pose, generation, expected)
					!= Animation::AnimationStatus::Ok) return 0U;
			}
			// This value participates in the fixture identity but is deliberately not
			// traversed. Ragdoll publication must scale with explicit expected handles,
			// never with unrelated sleeping provider bodies.
			hash ^= unrelatedStaticCount;
			return hash * 1099511628211ULL;
		}
	}

	void RegisterPhysicsPerformanceTests(TestRegistry& registry) {
		registry.Add("Physics.Performance.TransformPublicationBatchIsTypedAtomicVisibleAndExact", [](TestContext& context) {
			PhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			const Object first = objects.createObject("Transform Publication First");
			const Object second = objects.createObject("Transform Publication Second");
			Transform* firstTransform = objects.AddComponent<Transform>(first);
			Transform* secondTransform = objects.AddComponent<Transform>(second);
			context.Expect(firstTransform != nullptr && secondTransform != nullptr,
				"The typed publication fixture requires two writable Transform components.");
			(void)objects.SetCanonicalWorldPose(first, PoseAt(1.0F, 2.0F, 3.0F));
			(void)objects.SetCanonicalWorldPose(second, PoseAt(4.0F, 5.0F, 6.0F));
			const TerraEngine::UUID firstId = objects.getUUID(first);
			const TerraEngine::UUID secondId = objects.getUUID(second);
			const std::vector<Scene::TransactionTransformUpdate> updates{
				{firstId, PoseAt(10.0F, 20.0F, 30.0F)},
				{secondId, PoseAt(40.0F, 50.0F, 60.0F)}};

			Scene::TransactionalMutationBuilder staleBuilder(
				"physics-transform-batch-stale", Scene::TransactionOrigin::System, "PhysicsPerformanceTests");
			staleBuilder.UpdateTransformBatch(objects, updates);
			Scene::Transaction staleTransaction = std::move(staleBuilder).Build();
			context.Expect(staleTransaction.writeSet.operations.size() == 1U
				&& staleTransaction.writeSet.operations.front().kind == Scene::TransactionWriteKind::UpdateTransformBatch
				&& staleTransaction.writeSet.operations.front().transformUpdates.size() == updates.size(),
				"Physics publication must use one typed transform batch instead of reflected per-property writes.");
			mutations.EnqueueTransaction(std::move(staleTransaction));
			context.Expect(HasPose(objects, first, 1.0F, 2.0F, 3.0F),
				"A queued transform batch must remain invisible before the protected flush.");
			objects.RemoveComponent<Transform>(second);
			const Scene::SceneMutationBatchResult rejected = mutations.Apply(objects, "physics-transform-batch-rejected");
			context.Expect(rejected.transactionResults.size() == 1U
				&& rejected.transactionResults.front().status != Scene::TransactionStatus::Committed,
				"A stale transform target must reject the complete publication batch.");
			context.Expect(HasPose(objects, first, 1.0F, 2.0F, 3.0F),
				"Rejected transform publication must leave every earlier target unchanged.");

			context.Expect(objects.AddComponent<Transform>(second) != nullptr,
				"The accepted publication fixture must restore the removed Transform component.");
			Scene::TransactionalMutationBuilder acceptedBuilder(
				"physics-transform-batch-accepted", Scene::TransactionOrigin::System, "PhysicsPerformanceTests");
			acceptedBuilder.UpdateTransformBatch(objects, updates);
			mutations.EnqueueTransaction(std::move(acceptedBuilder).Build());
			context.Expect(HasPose(objects, first, 1.0F, 2.0F, 3.0F),
				"The accepted transform batch must also remain invisible before its flush.");
			const Scene::SceneMutationBatchResult committed = mutations.Apply(objects, "physics-transform-batch-committed");
			context.Expect(committed.transactionResults.size() == 1U
				&& committed.transactionResults.front().status == Scene::TransactionStatus::Committed
				&& committed.transactionResults.front().appliedWriteCount == updates.size(),
				"The protected flush must commit every typed transform update as one atomic publication.");
			firstTransform = objects.GetComponent<Transform>(first);
			secondTransform = objects.GetComponent<Transform>(second);
			context.Expect(firstTransform != nullptr && secondTransform != nullptr
				&& HasPose(objects, first, 10.0F, 20.0F, 30.0F)
				&& HasPose(objects, second, 40.0F, 50.0F, 60.0F),
				"The typed publication must make the exact position and rotation values visible at the flush boundary.");
		});

		registry.Add("Physics.Performance.TransformPublicationSuppressesUnchangedDynamicPoses", [](TestContext& context) {
			ThreadPoolSession pool;
			PhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			const Object object = objects.createObject("Unchanged Dynamic Publication");
			Transform* transform = objects.AddComponent<Transform>(object);
			RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
			context.Expect(transform != nullptr && body != nullptr && colliders != nullptr,
				"The same-pose fixture requires a dynamic body and its authored collider.");
			(void)objects.SetCanonicalWorldPose(object, PoseAt(3.0F, 4.0F, 5.0F));
			body->useGravity = false;
			colliders->Add();
			objects.MarkObjectTransformDirty(object);

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The same-pose fixture must start its isolated physics world.");
			// Direct unit-test updates bypass SceneManager. Publish the same immutable
			// per-frame packets that the production scheduler supplies to PhysicsSnapshot.
			(void)objects.PublishSceneChangePacket();
			physics.Update(physics.FixedTimeStep());
			const std::vector<Scene::TransactionPublicationGroup> firstPublication = mutations.PendingPublicationGroups();
			context.Expect(firstPublication.size() == 1U
				&& firstPublication.front().chunks.size() == 1U
				&& firstPublication.front().chunks.front().writeSet.operations.size() == 1U
				&& firstPublication.front().chunks.front().writeSet.operations.front().kind == Scene::TransactionWriteKind::UpdateTransformBatch,
				"The first dynamic pose must publish through one typed tick-wide transform group.");
			const auto firstFlush = mutations.Apply(objects, "physics-same-pose-first-flush");
			context.Expect(firstFlush.publicationGroupResults.size() == 1U
				&& firstFlush.publicationGroupResults.front().status == Scene::TransactionStatus::Committed,
				"The first same-pose publication must commit before duplicate suppression is measured.");
			physics.OnAfterPhysicsFlush(firstFlush.transactionResults, firstFlush.publicationGroupResults);

			(void)objects.PublishSceneChangePacket();
			physics.Update(physics.FixedTimeStep());
			// The first protected physics publication advances the authored Transform
			// revision. One following capture consumes that revision and may rebuild the
			// runtime body once, so flush it before measuring the steady-state path.
			const auto secondFlush = mutations.Apply(objects, "physics-same-pose-second-flush");
			physics.OnAfterPhysicsFlush(secondFlush.transactionResults, secondFlush.publicationGroupResults);

			(void)objects.PublishSceneChangePacket();
			physics.Update(physics.FixedTimeStep());
			const std::vector<Scene::TransactionPublicationGroup> steadyPublications = mutations.PendingPublicationGroups();
			const auto steadyDiagnostics = physics.LatestDiagnostics();
			std::string steadyMessage = "A settled unchanged awake dynamic pose must not enqueue another transform publication batch.";
			if(!steadyPublications.empty() && !steadyPublications.front().chunks.empty()
				&& !steadyPublications.front().chunks.front().writeSet.operations.empty()
				&& !steadyPublications.front().chunks.front().writeSet.operations.front().transformUpdates.empty()) {
				const Scene::TransactionTransformUpdate& repeated =
					steadyPublications.front().chunks.front().writeSet.operations.front().transformUpdates.front();
				steadyMessage += " Repeated canonical offset=(" + std::to_string(repeated.pose.position.offsetMetres.x) + ", "
					+ std::to_string(repeated.pose.position.offsetMetres.y) + ", " + std::to_string(repeated.pose.position.offsetMetres.z) + ").";
			}
			context.Expect(steadyPublications.empty(), steadyMessage);
			std::string diagnosticsMessage = "The canonical unchanged frame must report zero authoring, preparation, and protected publication work.";
			if(steadyDiagnostics) {
				diagnosticsMessage += " reused=" + std::to_string(steadyDiagnostics->authoring.snapshotReused)
					+ " changed=" + std::to_string(steadyDiagnostics->authoring.changedObjects)
					+ " sceneRevision=" + std::to_string(steadyDiagnostics->authoring.scenePublicationRevision)
					+ " constraints=" + std::to_string(steadyDiagnostics->authoring.constraintClosureCount)
					+ " characters=" + std::to_string(steadyDiagnostics->authoring.characterClosureCount)
					+ " preparation=" + std::to_string(steadyDiagnostics->preparation.estimatedWorkItems)
					+ " publications=" + std::to_string(steadyDiagnostics->publication.currentEntries) + ".";
			}
			context.Expect(steadyDiagnostics
				&& steadyDiagnostics->authoring.snapshotReused
				&& steadyDiagnostics->authoring.changedObjects == 0U
				&& steadyDiagnostics->authoring.scenePublicationRevision != 0U
				&& steadyDiagnostics->authoring.constraintClosureCount == 0U
				&& steadyDiagnostics->authoring.characterClosureCount == 0U
				&& steadyDiagnostics->preparation.estimatedWorkItems == 0U
				&& steadyDiagnostics->publication.currentEntries == 0U,
				diagnosticsMessage);
			const auto steadyFlush = mutations.Apply(objects, "physics-same-pose-steady-flush");
			physics.OnAfterPhysicsFlush(steadyFlush.transactionResults, steadyFlush.publicationGroupResults);
		});

		registry.Add("Physics.Performance.PhysicsAuthoredTransformsDoNotResetGravityVelocity", [](TestContext& context) {
			ThreadPoolSession pool;
			PhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();

			const Object object = objects.createObject("Accelerating Gravity Body");
			Transform* transform = objects.AddComponent<Transform>(object);
			RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
			context.Expect(transform != nullptr && body != nullptr && colliders != nullptr,
				"The gravity regression fixture requires a dynamic body and an authored collider.");
			if(transform != nullptr) (void)objects.SetCanonicalWorldPose(object, PoseAt(0.0F, 100.0F, 0.0F));
			if(body != nullptr) body->useGravity = true;
			if(colliders != nullptr) colliders->Add();
			objects.MarkObjectTransformDirty(object);
			const Object unrelatedEditorObject = objects.createObject("Unrelated Editor Spatial Work");

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(),
				"The gravity regression fixture must start its isolated physics world.");
			TerraEngine::Scheduling::SystemDeclaration scriptingDeclaration;
			scriptingDeclaration.name = "ScriptingSystem";
			scriptingDeclaration.phase = "Scripting";
			scriptingDeclaration.usesDefaultMetadata = false;
			PhysicsNeighborSystem scripting(scriptingDeclaration);
			TerraEngine::Scheduling::SystemDeclaration uiDeclaration;
			uiDeclaration.name = "UISystem";
			uiDeclaration.phase = "UI";
			uiDeclaration.reads = {"ObjectManager"};
			uiDeclaration.requiresFlushedSceneMutations = true;
			uiDeclaration.usesDefaultMetadata = false;
			PhysicsNeighborSystem ui(uiDeclaration);
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting, scripting.SchedulerDeclaration());
			scheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			scheduler.RegisterSystem(ui, ui.SchedulerDeclaration());
			const auto& schedule = scheduler.BuildSchedule();
			context.Expect(schedule.valid,
				"The gravity regression fixture must execute through the editor's scheduled physics workload path. "
				+ schedule.formatted);
			const float fixedTimeStep = physics.FixedTimeStep();
			for(std::uint32_t tick = 0U; tick < 10U; ++tick) {
				scheduler.ExecuteFrame(fixedTimeStep,
					[&](const TerraEngine::Scheduling::MutationFlushPoint& flushPoint) {
						TerraEngine::Scheduling::MutationFlushRuntimeState state;
						Scene::SceneMutationBatchResult flush;
						// Without a same-frame consumer, physics publications intentionally
						// remain queued at After Physics and become visible at Frame End.
						if(flushPoint.name != "After Physics" && !mutations.Empty()) {
							flush = mutations.Apply(objects, "physics-gravity-acceleration");
						}
						if(flushPoint.name == "Frame End") {
							physics.OnAfterPhysicsFlush(
								flush.transactionResults, flush.publicationGroupResults);
						}
						state.pendingAfterFlush = mutations.Size();
						return state;
					});
				// Editor rendering and marker work can advance the global spatial
				// revision without editing this body. The following capture must not
				// reinterpret physics' own published pose as fresh authoring input.
				objects.MarkObjectTransformDirty(unrelatedEditorObject);
			}

			const auto snapshot = physics.LatestBodySnapshot();
			const TerraEngine::UUID objectId = objects.getUUID(object);
			const auto state = snapshot && snapshot->bodies
				? std::find_if(snapshot->bodies->begin(), snapshot->bodies->end(),
					[objectId](const Physics::PhysicsBodyState& candidate) {
						return candidate.objectId == objectId;
					})
				: std::vector<Physics::PhysicsBodyState>::const_iterator{};
			const bool found = snapshot && snapshot->bodies && state != snapshot->bodies->end();
			context.Expect(found,
				"The gravity regression fixture must publish the dynamic body's runtime state.");
			context.Expect(found && state->linearVelocity.y < -1.0F,
				"Ten fixed gravity ticks must accumulate downward velocity instead of resetting it after each physics-authored Transform publication.");
		});

		registry.Add("Physics.Performance.AuthoringSnapshotDetectsRealTransformAndComponentMutations", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Physics Snapshot Revision Fixture");
			RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
			context.Expect(body != nullptr && colliders != nullptr,
				"The snapshot revision fixture requires authored body and collider components.");
			colliders->Add();
			objects.MarkObjectTransformDirty(object);
			const auto first = Physics::PhysicsAuthoringSnapshot::Capture(
				objects, {{1U}, 1U}, Physics::PhysicsSettings::Defaults(), 0U);
			context.Expect(first && first->Data().dirtyObjects.size() == 1U,
				"The first authored body capture must mark its initial descriptor dirty.");
			const auto unchanged = Physics::PhysicsAuthoringSnapshot::Capture(
				objects, {{1U}, 1U}, Physics::PhysicsSettings::Defaults(), 0U, &*first);
			context.Expect(unchanged && unchanged->Data().dirtyObjects.empty(),
				"An unchanged authoring capture must retain its body without requesting a rebuild.");
			Transform* transform = objects.GetComponent<Transform>(object);
			context.Expect(transform != nullptr, "The snapshot revision fixture requires the default Transform.");
			(void)objects.SetCanonicalWorldPose(object, PoseAt(3.0F, 0.0F, 0.0F));
			objects.MarkObjectTransformDirty(object);
			const auto transformed = Physics::PhysicsAuthoringSnapshot::Capture(
				objects, {{1U}, 1U}, Physics::PhysicsSettings::Defaults(), 0U, &*unchanged);
			context.Expect(transformed && transformed->Data().dirtyObjects.size() == 1U
				&& transformed->Data().bodies.front().worldPose.position.x == 3.0F,
				"A real authored Transform mutation must invalidate the retained snapshot and publish its resolved pose.");
			body->useGravity = false;
			objects.notifyObjectStructureChanged(object);
			const auto componentChanged = Physics::PhysicsAuthoringSnapshot::Capture(
				objects, {{1U}, 1U}, Physics::PhysicsSettings::Defaults(), 0U, &*transformed);
			context.Expect(componentChanged && componentChanged->Data().dirtyObjects.size() == 1U
				&& !componentChanged->Data().bodies.front().useGravity,
				"A real authored component mutation must invalidate the retained snapshot and preserve its changed value.");
			objects.clear();
		});

		registry.Add("Physics.Performance.FixedTickThroughputPublishesBarrierAllocationAndDiagnosticTelemetry", [](TestContext& context) {
			ThreadPoolSession pool;
			PhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			AddDynamicFixtureBodies(context);

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(),
				"The performance fixture must enter Play Mode before fixed-tick throughput is measured.");
			const float fixedTimeStep = physics.FixedTimeStep();

			for(std::uint32_t tick = 0U; tick < kFixedTickCount; ++tick) {
				physics.Update(fixedTimeStep);
				const auto flush = mutations.Apply(objects, "physics-performance-fixed-tick");
				physics.OnAfterPhysicsFlush(flush.transactionResults, flush.publicationGroupResults);
			}
			// Publication runs before the protected After Physics flush marks the
			// current request completed. Run one zero-delta frame after the final
			// flush so the immutable diagnostics snapshot observes all sixty completed
			// ticks without scheduling a sixty-first simulation step.
			physics.Update(0.0F);
			const auto finalDiagnosticsFlush = mutations.Apply(objects, "physics-performance-final-diagnostics");
			physics.OnAfterPhysicsFlush(finalDiagnosticsFlush.transactionResults, finalDiagnosticsFlush.publicationGroupResults);

			const auto diagnostics = physics.LatestDiagnostics();
			const auto published = Physics::AcquirePhysicsDiagnosticsSnapshot();
			context.Expect(diagnostics != nullptr && published != nullptr,
				"Every fixed-tick performance pass must publish immutable diagnostics.");
			context.Expect(diagnostics && diagnostics->tick == kFixedTickCount,
				"A sixty-tick throughput pass must report each completed fixed physics tick.");
			context.Expect(diagnostics && diagnostics->counters.bodyCount >= kThroughputBodyCount,
				"Published diagnostics must expose all authored dynamic bodies rather than a partial worker snapshot.");
			context.Expect(diagnostics && diagnostics->memory.highWaterBytes >= diagnostics->memory.snapshotBytes
				&& diagnostics->memory.highWaterProviderAllocations >= diagnostics->memory.outstandingProviderAllocations,
				"Allocation and snapshot high-water telemetry must remain internally consistent at the publication boundary.");
			context.Expect(diagnostics
				&& diagnostics->extraction.totalBodies >= kThroughputBodyCount
				&& diagnostics->extraction.examinedBodies <= diagnostics->extraction.totalBodies
				&& diagnostics->extraction.extractedBodies <= diagnostics->extraction.examinedBodies
				&& diagnostics->stateStore.bodyCount >= kThroughputBodyCount
				&& diagnostics->publication.currentEntries <= diagnostics->extraction.changedBodies,
				"A complete fixed step must publish bounded, internally consistent extraction, state-store, and transform work volumes.");
			context.Expect(diagnostics && diagnostics->scheduler != nullptr
				// PhysicsSimulate is worker-prepared, but the provider remains the
				// authoritative owner of its world state. The current scheduler
				// contract names that protected concern "provider ownership"; the
				// older "provider barrier" label was stale diagnostic wording.
				&& ContainsWorkload(*diagnostics->scheduler, "PhysicsSimulate", "provider ownership")
				&& ContainsWorkload(*diagnostics->scheduler, "PhysicsPublicationBarrier", "Frame End"),
				"Reduced-schedule diagnostics must expose provider ownership and the deferred Frame End publication boundary.");
			if(diagnostics) {
				std::array<bool, static_cast<std::size_t>(Physics::PhysicsStageKind::Count)> observedStages{};
				for(const Physics::PhysicsStageMeasurement& stage : diagnostics->stages) {
					const std::size_t index = static_cast<std::size_t>(stage.stage);
					context.Expect(index < observedStages.size() && !observedStages[index],
						"Each immutable performance record must carry one distinct stage meaning.");
					if(index < observedStages.size()) observedStages[index] = true;
					context.Expect(stage.availability == Physics::PhysicsMetricAvailability::Available
						&& stage.unavailableReason == Physics::PhysicsMetricUnavailableReason::None,
						"A stage executed by the fixed-tick fixture must report measured availability explicitly.");
					context.Expect(!stage.allocationCountAvailable && !stage.allocationBytesAvailable
						&& stage.allocationCount == 0U && stage.allocationBytes == 0U,
						"Unsupported allocation evidence must carry unavailable flags; zero must not masquerade as a measurement.");
				}
				context.Expect(std::all_of(observedStages.begin(), observedStages.end(), [](bool observed) { return observed; }),
					"Diagnostics must publish all eight named physics performance stages every fixed tick.");
				context.Expect(diagnostics->quality.applied
					&& diagnostics->quality.velocityIterations == 10U
					&& diagnostics->quality.positionIterations == 2U,
					"Immutable diagnostics must identify the selected default solver profile applied to the live world.");
				context.Expect(diagnostics->jobs.submitted == diagnostics->jobs.completed + diagnostics->jobs.queued
					&& diagnostics->jobs.activePumps == 0U && diagnostics->jobs.idle,
					"Copied job telemetry must reconcile completed and queued work at the publication boundary.");
			}
			context.Expect(published && published->tick == diagnostics->tick,
				"Readers must observe the same completed-tick diagnostics snapshot that PhysicsSystem published.");
		});

		registry.Add("Physics.Performance.EventPacketsReserveObservedBoundedVolume", [](TestContext& context) {
			// Empty and lightly populated ticks are common. Their immutable packet must
			// not retain the maximum event capacity merely because another tick could be busy.
			Physics::PhysicsEventSubscriptionRegistry subscriptions;
			subscriptions.Subscribe({});
			const std::array<const Physics::PhysicsEventThreadLocalCollector*, 0U> noCollectors{};
			const Physics::PhysicsEventPacket empty = Physics::PhysicsEventPacketBuilder::MergeCollectors(
				{{8U}, 1U}, 1U, noCollectors, subscriptions.Snapshot());
			Physics::PhysicsEventThreadLocalCollector collector(8U);
			collector.Record(MakePerformanceEvent(1U));
			const Physics::PhysicsEventThreadLocalCollector* oneCollector[] = {&collector};
			const Physics::PhysicsEventPacket lowVolume = Physics::PhysicsEventPacketBuilder::MergeCollectors(
				{{8U}, 1U}, 1U, oneCollector, subscriptions.Snapshot());
			context.Expect(empty.events->empty() && empty.events->capacity() == 0U,
				"An empty event tick must publish an empty immutable result without reserving the configured packet maximum.");
			context.Expect(lowVolume.events->size() == 1U && lowVolume.events->capacity() < 4096U,
				"A lightly populated event tick must reserve from observed bounded volume instead of the 4096-record maximum.");
		});

		registry.Add("Physics.Performance.RagdollCrowdsScaleWithExplicitBodyWork", [](TestContext& context) {
			std::uint64_t smallVisited = 0U;
			std::uint64_t largeVisited = 0U;
			const std::uint64_t smallHash = HashRagdollPoseWork(64U, 16U, 8000U, smallVisited);
			const std::uint64_t repeatedHash = HashRagdollPoseWork(64U, 16U, 8000U, largeVisited);
			context.Expect(smallHash != 0U && smallHash == repeatedHash && smallVisited == 1024U
				&& largeVisited == 1024U,
				"The canonical 64x16 ragdoll fixture must retain one deterministic pose hash and visit exactly its explicit bodies.");

			const std::uint64_t largeHash = HashRagdollPoseWork(128U, 16U, 8000U, largeVisited);
			context.Expect(largeHash != 0U && largeHash != smallHash && largeVisited == 2048U
				&& largeVisited == smallVisited * 2U,
				"Doubling the ragdoll crowd must double bounded pose work without visiting the 8,000 unrelated sleeping statics.");
		});
	}
}
