// PhysicsPerformanceTests.cpp - deterministic runtime performance-regression contracts.
//
// These tests intentionally verify work counts and published telemetry instead of
// comparing wall-clock durations. T105 and T106 record machine-specific
// RelWithDebInfo budgets; a unit-test clock threshold would be noisy on CI.

#include "../TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace TerraEngine::Tests {
	namespace {
		constexpr std::size_t kThroughputBodyCount = 64U;
		constexpr std::uint32_t kFixedTickCount = 60U;

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

		void AddDynamicFixtureBodies(TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			for(std::size_t index = 0U; index < kThroughputBodyCount; ++index) {
				const Object object = objects.createObject("Physics Performance Body");
				Transform* transform = objects.AddComponent<Transform>(object);
				RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
				ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
				context.Expect(transform != nullptr && body != nullptr && colliders != nullptr,
					"The fixed-tick fixture must create every dynamic-body component.");
				transform->position = {
					static_cast<float>(index % 8U),
					4.0F + static_cast<float>(index / 8U),
					static_cast<float>((index / 8U) % 8U)};
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
			firstTransform->position = {1.0F, 2.0F, 3.0F};
			secondTransform->position = {4.0F, 5.0F, 6.0F};
			const TerraEngine::UUID firstId = objects.getUUID(first);
			const TerraEngine::UUID secondId = objects.getUUID(second);
			const std::vector<Scene::TransactionTransformUpdate> updates{
				{firstId, {10.0F, 20.0F, 30.0F}, {1.0F, 0.0F, 0.0F, 0.0F}},
				{secondId, {40.0F, 50.0F, 60.0F}, {0.0F, 0.0F, 1.0F, 0.0F}}};

			Scene::TransactionalMutationBuilder staleBuilder(
				"physics-transform-batch-stale", Scene::TransactionOrigin::System, "PhysicsPerformanceTests");
			staleBuilder.UpdateTransformBatch(objects, updates);
			Scene::Transaction staleTransaction = std::move(staleBuilder).Build();
			context.Expect(staleTransaction.writeSet.operations.size() == 1U
				&& staleTransaction.writeSet.operations.front().kind == Scene::TransactionWriteKind::UpdateTransformBatch
				&& staleTransaction.writeSet.operations.front().transformUpdates.size() == updates.size(),
				"Physics publication must use one typed transform batch instead of reflected per-property writes.");
			mutations.EnqueueTransaction(std::move(staleTransaction));
			context.Expect(firstTransform->position == glm::vec3(1.0F, 2.0F, 3.0F),
				"A queued transform batch must remain invisible before the protected flush.");
			objects.RemoveComponent<Transform>(second);
			const Scene::SceneMutationBatchResult rejected = mutations.Apply(objects, "physics-transform-batch-rejected");
			context.Expect(rejected.transactionResults.size() == 1U
				&& rejected.transactionResults.front().status != Scene::TransactionStatus::Committed,
				"A stale transform target must reject the complete publication batch.");
			context.Expect(firstTransform->position == glm::vec3(1.0F, 2.0F, 3.0F),
				"Rejected transform publication must leave every earlier target unchanged.");

			context.Expect(objects.AddComponent<Transform>(second) != nullptr,
				"The accepted publication fixture must restore the removed Transform component.");
			Scene::TransactionalMutationBuilder acceptedBuilder(
				"physics-transform-batch-accepted", Scene::TransactionOrigin::System, "PhysicsPerformanceTests");
			acceptedBuilder.UpdateTransformBatch(objects, updates);
			mutations.EnqueueTransaction(std::move(acceptedBuilder).Build());
			context.Expect(firstTransform->position == glm::vec3(1.0F, 2.0F, 3.0F),
				"The accepted transform batch must also remain invisible before its flush.");
			const Scene::SceneMutationBatchResult committed = mutations.Apply(objects, "physics-transform-batch-committed");
			context.Expect(committed.transactionResults.size() == 1U
				&& committed.transactionResults.front().status == Scene::TransactionStatus::Committed
				&& committed.transactionResults.front().appliedWriteCount == updates.size(),
				"The protected flush must commit every typed transform update as one atomic publication.");
			firstTransform = objects.GetComponent<Transform>(first);
			secondTransform = objects.GetComponent<Transform>(second);
			context.Expect(firstTransform != nullptr && secondTransform != nullptr
				&& firstTransform->position == updates[0].position && firstTransform->rotation == updates[0].rotation
				&& secondTransform->position == updates[1].position && secondTransform->rotation == updates[1].rotation,
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
			transform->position = {3.0F, 4.0F, 5.0F};
			body->useGravity = false;
			colliders->Add();
			objects.MarkObjectTransformDirty(object);

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The same-pose fixture must start its isolated physics world.");
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

			physics.Update(physics.FixedTimeStep());
			// The first protected physics publication advances the authored Transform
			// revision. One following capture consumes that revision and may rebuild the
			// runtime body once, so flush it before measuring the steady-state path.
			const auto secondFlush = mutations.Apply(objects, "physics-same-pose-second-flush");
			physics.OnAfterPhysicsFlush(secondFlush.transactionResults, secondFlush.publicationGroupResults);

			physics.Update(physics.FixedTimeStep());
			const std::vector<Scene::TransactionPublicationGroup> steadyPublications = mutations.PendingPublicationGroups();
			const auto steadyDiagnostics = physics.LatestDiagnostics();
			std::string steadyMessage = "A settled unchanged awake dynamic pose must not enqueue another transform publication batch.";
			if(!steadyPublications.empty() && !steadyPublications.front().chunks.empty()
				&& !steadyPublications.front().chunks.front().writeSet.operations.empty()
				&& !steadyPublications.front().chunks.front().writeSet.operations.front().transformUpdates.empty()) {
				const Scene::TransactionTransformUpdate& repeated =
					steadyPublications.front().chunks.front().writeSet.operations.front().transformUpdates.front();
				steadyMessage += " Repeated position=(" + std::to_string(repeated.position.x) + ", "
					+ std::to_string(repeated.position.y) + ", " + std::to_string(repeated.position.z)
					+ ") rotation=(" + std::to_string(repeated.rotation.x) + ", "
					+ std::to_string(repeated.rotation.y) + ", " + std::to_string(repeated.rotation.z)
					+ ", " + std::to_string(repeated.rotation.w) + ").";
			}
			context.Expect(steadyPublications.empty(), steadyMessage);
			context.Expect(steadyDiagnostics
				&& steadyDiagnostics->authoring.snapshotReused
				&& steadyDiagnostics->authoring.changedObjects == 0U
				&& steadyDiagnostics->preparation.estimatedWorkItems == 0U
				&& steadyDiagnostics->publication.currentEntries == 0U,
				"The canonical unchanged frame must report zero authoring, preparation, and protected publication work.");
			const auto steadyFlush = mutations.Apply(objects, "physics-same-pose-steady-flush");
			physics.OnAfterPhysicsFlush(steadyFlush.transactionResults, steadyFlush.publicationGroupResults);
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
			transform->position.x = 3.0F;
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
			context.Expect(published && published->tick == diagnostics->tick,
				"Readers must observe the same completed-tick diagnostics snapshot that PhysicsSystem published.");
		});
	}
}
