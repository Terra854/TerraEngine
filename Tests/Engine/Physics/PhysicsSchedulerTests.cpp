#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>

namespace TerraEngine::Tests {
	namespace {
		class DeclarationOnlySystem final : public SystemsBase {
		public:
			explicit DeclarationOnlySystem(TerraEngine::Scheduling::SystemDeclaration declaration)
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

		// This fixture models PhysicsPublish's only permitted scene-write path:
		// it queues a typed Transform transaction, then lets the scheduler-owned
		// After Physics barrier make that transaction visible to later systems.
		class PhysicsTransformPublisherFixture final : public SystemsBase {
		public:
			explicit PhysicsTransformPublisherFixture(TerraEngine::UUID target)
				: targetObject(target) {
			}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
			std::string SystemName() override { return "PhysicsSystem"; }

			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override {
				TerraEngine::Scheduling::SystemDeclaration declaration;
				declaration.name = SystemName();
				declaration.phase = "Physics";
				declaration.after = {"ScriptingSystem"};
				declaration.before = {"UIObserver"};
				declaration.reads = {"ObjectManager"};
				declaration.writes = {"PhysicsTransformTransactions", "SceneMutationQueue"};
				declaration.mainThread = true;
				declaration.requiresFlushedSceneMutations = true;
				declaration.usesDefaultMetadata = false;
				return declaration;
			}

			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float) override {
				TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
				workload.workloadName = "PhysicsPublishFixture";
				workload.reads = {"ObjectManager"};
				workload.writes = {"PhysicsTransformTransactions", "SceneMutationQueue"};
				workload.mainThreadRequired = true;
				workload.workerEligible = false;
				workload.mutationPolicy = TerraEngine::Scheduling::FrameWorkloadMutationPolicy::DeferredOnly;
				workload.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
				workload.sameFrameVisibilityRequired = true;
				workload.protectedReason = "Fixture queues the typed physics Transform publication.";
				workload.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
					ObjectManager& objects = ObjectManager::instance();
					TerraEngine::Scene::TransactionalMutationBuilder builder(
						"physics-after-flush-visibility",
						TerraEngine::Scene::TransactionOrigin::System,
						"PhysicsSchedulerFixture");
					builder.ObserveObject(objects, targetObject)
						.UpdateComponentProperty(targetObject, std::string(Transform::kComponentName), "Position",
							TerraEngine::Reflection::Variant{glm::vec3(4.0F, 5.0F, 6.0F)});
					TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(builder.Build());
				};
				return {std::move(workload)};
			}

		private:
			TerraEngine::UUID targetObject;
		};

		class TransformObserverFixture final : public SystemsBase {
		public:
			explicit TransformObserverFixture(Object target)
				: targetObject(target) {
			}

			void Initialize() override {}
			void Update(float) override {
				const Transform* transform = ObjectManager::instance().GetComponent<Transform>(targetObject);
				observedPublishedTransform = transform != nullptr
					&& transform->position == glm::vec3(4.0F, 5.0F, 6.0F);
			}
			void Shutdown() override {}
			std::string SystemName() override { return "UIObserver"; }
			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override {
				TerraEngine::Scheduling::SystemDeclaration declaration;
				declaration.name = SystemName();
				declaration.phase = "UI";
				declaration.after = {"PhysicsSystem"};
				declaration.reads = {"ObjectManager"};
				declaration.mainThread = true;
				declaration.requiresFlushedSceneMutations = true;
				declaration.usesDefaultMetadata = false;
				return declaration;
			}

			bool observedPublishedTransform = false;

		private:
			Object targetObject;
		};

		// The singleton physics system and shared worker pool must be torn down in
		// this order so a failed expectation cannot leak provider work into later tests.
		class ScheduledPhysicsRuntimeGuard final {
		public:
			ScheduledPhysicsRuntimeGuard() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ScheduledPhysicsRuntimeGuard() {
				auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}
		};
	}

	void RegisterPhysicsSchedulerTests(TestRegistry& registry) {
		registry.Add("Physics.Scheduler.PhaseResourcesAndFlushAreDeclared", [](TestContext& context) {
			const auto phases = TerraEngine::Scheduling::DefaultSchedulerPhases();
			const auto scripting = std::find_if(phases.begin(), phases.end(), [](const auto& phase) { return phase.name == "Scripting"; });
			const auto physics = std::find_if(phases.begin(), phases.end(), [](const auto& phase) { return phase.name == "Physics"; });
			const auto ui = std::find_if(phases.begin(), phases.end(), [](const auto& phase) { return phase.name == "UI"; });
			context.Expect(scripting != phases.end() && physics != phases.end() && ui != phases.end(),
				"Scripting, Physics, and UI phases must all exist.");
			context.Expect(scripting->order < physics->order && physics->order < ui->order,
				"Physics must execute after Scripting and before UI.");

			const auto flushes = TerraEngine::Scheduling::DefaultMutationFlushPoints();
			const auto afterPhysics = std::find_if(flushes.begin(), flushes.end(), [](const auto& flush) {
				return flush.name == "After Physics" && flush.phaseName == "Physics";
			});
			context.Expect(afterPhysics != flushes.end(),
				"A protected After Physics scene-mutation flush must publish transforms in the same frame.");

			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			for(const char* required : {"PhysicsSettingsUpdateQueue", "PhysicsSettings", "PhysicsAuthoringSnapshot", "PhysicsPreparedWork", "PhysicsWorld",
				"PhysicsPublishedSnapshots", "PhysicsTransformTransactions"}) {
				context.Expect(std::any_of(resources.begin(), resources.end(), [required](const auto& resource) {
					return resource.name == required;
				}), "Each physics workload resource must be visible in scheduler diagnostics.");
			}
		});

		registry.Add("Physics.Scheduler.StableActivatedWorkloadsUseExplicitDependencies", [](TestContext& context) {
			TerraEngine::Scheduling::SystemDeclaration scriptingDeclaration;
			scriptingDeclaration.name = "ScriptingSystem";
			scriptingDeclaration.phase = "Scripting";
			scriptingDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem scripting(scriptingDeclaration);
			TerraEngine::Scheduling::SystemDeclaration uiDeclaration;
			uiDeclaration.name = "UISystem";
			uiDeclaration.phase = "UI";
			uiDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem ui(uiDeclaration);

			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting, scripting.SchedulerDeclaration());
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			scheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			scheduler.RegisterSystem(ui, ui.SchedulerDeclaration());
			context.Expect(scheduler.BuildSchedule().valid,
				"Physics dependencies and declared resources must validate in the real scheduler.");
			const auto workloads = scheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F);
			const auto repeated = scheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F);
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> physicsWorkloads;
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> repeatedPhysicsWorkloads;
			std::copy_if(workloads.begin(), workloads.end(), std::back_inserter(physicsWorkloads), [](const auto& workload) {
				return workload.ownerSystem == "PhysicsSystem";
			});
			std::copy_if(repeated.begin(), repeated.end(), std::back_inserter(repeatedPhysicsWorkloads), [](const auto& workload) {
				return workload.ownerSystem == "PhysicsSystem";
			});
			context.ExpectEqual(static_cast<std::size_t>(7), physicsWorkloads.size(),
				"PhysicsSystem must expose its seven stable phase descriptors for activation diagnostics.");
			context.ExpectEqual(physicsWorkloads.size(), repeatedPhysicsWorkloads.size(),
				"Repeated frames must reuse the same physics descriptor set.");
			for(std::size_t index = 1U; index < physicsWorkloads.size(); ++index) {
				context.Expect(std::find(physicsWorkloads[index].explicitDependencies.begin(),
					physicsWorkloads[index].explicitDependencies.end(), physicsWorkloads[index - 1U].stableDescriptorId)
					!= physicsWorkloads[index].explicitDependencies.end(),
					"Each physics phase must name its prerequisite descriptor instead of relying on global execution order.");
				context.Expect(physicsWorkloads[index].stableDescriptorGeneration == repeatedPhysicsWorkloads[index].stableDescriptorGeneration,
					"Unchanged physics descriptor metadata must retain its generation across frames.");
			}
			context.Expect(physicsWorkloads.front().workloadName == "PhysicsSnapshot"
				&& physicsWorkloads.back().workloadName == "PhysicsPublicationBarrier",
				"The workload chain must begin with snapshot and end at the publication barrier.");
			context.Expect(physicsWorkloads[1].workerEligible && physicsWorkloads[3].workerEligible
				&& physicsWorkloads[4].workerEligible,
				"Prepare, simulate, and extract must declare worker-safe execution.");
			context.Expect(physicsWorkloads[0].mainThreadRequired && physicsWorkloads[2].mainThreadRequired
				&& physicsWorkloads[5].mainThreadRequired && physicsWorkloads[6].mainThreadRequired,
				"Snapshot, command commit, publish, and publication barrier must remain protected.");
			context.Expect(std::all_of(physicsWorkloads.begin(), physicsWorkloads.end(), [](const auto& workload) {
				return !workload.activationPredicateKey.empty() && !workload.stableDescriptorId.empty();
			}), "Every physics phase must expose stable activation and descriptor identities.");
			if (!physics.WorldHandle().IsValid()) {
				context.Expect(std::none_of(physicsWorkloads.begin(), physicsWorkloads.end(), [](const auto& workload) {
					return workload.active;
				}), "An inactive physics world must omit every phase rather than dispatching no-op jobs.");
			}
		});

		registry.Add("Physics.Scheduler.SevenPhaseDeclarationsExposeBoundaryAndWorkEvidence", [](TestContext& context) {
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			const auto workloads = physics.DeclareFrameWorkloads(1.0F / 60.0F);
			const auto repeatedWorkloads = physics.DeclareFrameWorkloads(1.0F / 60.0F);
			const std::array<std::string_view, 7U> expectedNames{
				"PhysicsSnapshot", "PhysicsPrepare", "PhysicsCommandCommit", "PhysicsSimulate",
				"PhysicsExtract", "PhysicsPublish", "PhysicsPublicationBarrier"};
			context.Expect(workloads.size() == expectedNames.size(),
				"Physics must declare every stable phase even when the world is inactive so diagnostics remain inspectable.");
			for(std::size_t index = 0U; index < expectedNames.size() && index < workloads.size(); ++index) {
				const auto& workload = workloads[index];
				// Worker-safe phases declare immutable subwork, while protected phases explain
				// why their live-world boundary remains serialized on the main thread.
				const bool hasBoundaryEvidence = workload.workerEligible
					? !workload.safeSubwork.empty()
					: !workload.protectedReason.empty();
				context.Expect(workload.workloadName == expectedNames[index]
					&& !workload.reads.empty() && !workload.writes.empty()
					&& !workload.activationPredicateKey.empty() && hasBoundaryEvidence
					&& !workload.stableDescriptorId.empty(),
					"Every physics phase must expose its resources, activation predicate, boundary evidence, and stable diagnostic identity.");
				context.Expect(workload.workerEligible != workload.mainThreadRequired,
					"Every physics phase must state whether work is worker-safe or needs the protected main-thread boundary.");
				context.Expect(index < repeatedWorkloads.size()
					&& workload.active == repeatedWorkloads[index].active
					&& workload.ready == repeatedWorkloads[index].ready
					&& workload.estimatedWorkItemCount == repeatedWorkloads[index].estimatedWorkItemCount
					&& workload.inlineWorkItemThreshold == repeatedWorkloads[index].inlineWorkItemThreshold,
					"An unchanged frame must retain each phase activation, readiness, and bounded work estimate.");
				if(index != 0U) {
					context.Expect(std::find(workload.explicitDependencies.begin(), workload.explicitDependencies.end(),
						workloads[index - 1U].stableDescriptorId) != workload.explicitDependencies.end(),
						"Each declared phase must explicitly depend on its preceding physics phase.");
				}
			}
		});

		registry.Add("Physics.Scheduler.AfterPhysicsFlushPublishesTypedTransformBeforeUI", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			const Object target = objects.createObject("Physics Publication Target");
			context.Expect(objects.AddComponent<Transform>(target) != nullptr,
				"The visibility fixture needs a reflected Transform target.");
			objects.MarkObjectTransformDirty(target);

			TerraEngine::Scheduling::SystemDeclaration scriptingDeclaration;
			scriptingDeclaration.name = "ScriptingSystem";
			scriptingDeclaration.phase = "Scripting";
			scriptingDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem scripting(scriptingDeclaration);
			PhysicsTransformPublisherFixture physics(objects.getUUID(target));
			TransformObserverFixture ui(target);
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting, scripting.SchedulerDeclaration());
			scheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			scheduler.RegisterSystem(ui, ui.SchedulerDeclaration());
			context.Expect(scheduler.BuildSchedule().valid, "Fixture schedule must accept the Physics-to-UI publication path.");

			scheduler.ExecuteFrame(1.0F / 60.0F, [](const TerraEngine::Scheduling::MutationFlushPoint&) {
				TerraEngine::Scheduling::MutationFlushRuntimeState state;
				TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
				if(!queue.Empty()) {
					queue.Apply(ObjectManager::instance(), "physics-scheduler-visibility-flush");
				}
				state.pendingAfterFlush = queue.Size();
				return state;
			});

			context.Expect(ui.observedPublishedTransform,
				"UI must observe the queued physics Transform only after the protected After Physics flush.");
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			objects.clear();
		});

		registry.Add("Physics.Scheduler.PublicationBoundaryRequiresDeclaredSameFrameConsumer", [](TestContext& context) {
			TerraEngine::Scheduling::SystemDeclaration physicsDeclaration;
			physicsDeclaration.name = "PhysicsFixture";
			physicsDeclaration.phase = "Physics";
			physicsDeclaration.writes = {"SceneMutationQueue"};
			physicsDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem physics(physicsDeclaration);

			TerraEngine::Scheduling::SystemScheduler reducedScheduler;
			reducedScheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			context.Expect(reducedScheduler.BuildSchedule().valid, "The reduced physics-only schedule must validate.");
			(void)reducedScheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F);
			context.Expect(!TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive(),
				"A reduced schedule must defer physics transform visibility to Frame End.");

			TerraEngine::Scheduling::SystemDeclaration uiDeclaration;
			uiDeclaration.name = "UIConsumer";
			uiDeclaration.phase = "UI";
			uiDeclaration.reads = {"ObjectManager"};
			uiDeclaration.requiresFlushedSceneMutations = true;
			uiDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem ui(uiDeclaration);
			TerraEngine::Scheduling::SystemScheduler interactiveScheduler;
			interactiveScheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			interactiveScheduler.RegisterSystem(ui, ui.SchedulerDeclaration());
			context.Expect(interactiveScheduler.BuildSchedule().valid, "The interactive same-frame consumer schedule must validate.");
			(void)interactiveScheduler.BuildFrameWorkloadDeclarations(1.0F / 60.0F);
			context.Expect(TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive(),
				"A later declared scene reader must request After Physics transform visibility.");
		});

		registry.Add("Physics.Scheduler.ExtractionRetainsProviderEventsInImmutableHistory", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			ScheduledPhysicsRuntimeGuard runtimeGuard;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();

			// Two overlapping default boxes produce a real Jolt callback during the
			// scheduled simulation phase. A collider-only object becomes static while
			// the explicit body remains dynamic, avoiding two immovable participants.
			const Object support = objects.createObject("Scheduled Event Support");
			const Object dynamic = objects.createObject("Scheduled Event Dynamic");
			Transform* supportTransform = objects.AddComponent<Transform>(support);
			Transform* dynamicTransform = objects.AddComponent<Transform>(dynamic);
			ColliderSetComponent* supportColliders = objects.AddComponent<ColliderSetComponent>(support);
			ColliderSetComponent* dynamicColliders = objects.AddComponent<ColliderSetComponent>(dynamic);
			RigidBodyComponent* dynamicBody = objects.AddComponent<RigidBodyComponent>(dynamic);
			context.Expect(supportTransform != nullptr && dynamicTransform != nullptr
				&& supportColliders != nullptr && dynamicColliders != nullptr && dynamicBody != nullptr,
				"The scheduled provider-event fixture needs one static and one dynamic collider-backed object.");
			if(supportColliders != nullptr) supportColliders->Add();
			if(dynamicColliders != nullptr) dynamicColliders->Add();
			if(dynamicBody != nullptr) dynamicBody->useGravity = false;
			if(supportTransform != nullptr) supportTransform->position = {0.0F, 0.0F, 0.0F};
			if(dynamicTransform != nullptr) dynamicTransform->position = {0.0F, 0.25F, 0.0F};
			objects.MarkObjectTransformDirty(support);
			objects.MarkObjectTransformDirty(dynamic);

			auto& physics = PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The scheduled extraction regression needs a live provider world.");
			const PhysicsEventSubscription subscription = physics.SubscribeEvents(PhysicsEventSubscriptionPolicy{});
			context.Expect(subscription.id != 0U, "The live event stream must accept an all-events immutable subscription.");

			TerraEngine::Scheduling::SystemDeclaration scriptingDeclaration;
			scriptingDeclaration.name = "ScriptingSystem";
			scriptingDeclaration.phase = "Scripting";
			scriptingDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem scripting(scriptingDeclaration);
			TerraEngine::Scheduling::SystemDeclaration uiDeclaration;
			uiDeclaration.name = "UISystem";
			uiDeclaration.phase = "UI";
			uiDeclaration.reads = {"ObjectManager"};
			uiDeclaration.requiresFlushedSceneMutations = true;
			uiDeclaration.usesDefaultMetadata = false;
			DeclarationOnlySystem ui(uiDeclaration);
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(scripting, scripting.SchedulerDeclaration());
			scheduler.RegisterSystem(physics, physics.SchedulerDeclaration());
			scheduler.RegisterSystem(ui, ui.SchedulerDeclaration());
			context.Expect(scheduler.BuildSchedule().valid, "The live extraction regression must execute through the declared Physics workload graph.");

			std::shared_ptr<const PhysicsEventPacket> retained;
			for(std::uint32_t frame = 0U; frame < 4U && (!retained || !retained->events || retained->events->empty()); ++frame) {
				scheduler.ExecuteFrame(physics.FixedTimeStep(), [&](const TerraEngine::Scheduling::MutationFlushPoint& flushPoint) {
					TerraEngine::Scheduling::MutationFlushRuntimeState state;
					const auto flushed = mutations.Apply(objects, "physics-scheduler-event-history");
					if(flushPoint.name == "After Physics") {
						physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
					}
					state.pendingAfterFlush = mutations.Size();
					return state;
				});
				const auto diagnostics = physics.LatestDiagnostics();
				if(diagnostics && diagnostics->tick != 0U) retained = physics.ReadRetainedEventPacket(diagnostics->tick);
			}

			const auto diagnostics = physics.LatestDiagnostics();
			const std::uint32_t fields = diagnostics ? diagnostics->extraction.fieldMask : 0U;
			context.Expect(diagnostics != nullptr
				&& (fields & static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::Events)) != 0U
				&& (fields & static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::BodyState))
					== static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::BodyState),
				"Scheduled extraction must retain the Events bit while requesting the complete body-state mask.");
			context.Expect((fields & static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::Constraints)) == 0U
				&& (fields & static_cast<std::uint32_t>(PhysicsBodyStateFieldMask::Characters)) == 0U,
				"Constraint and character fields must remain conditional when those runtime families are absent.");
			context.Expect(retained != nullptr && retained->events != nullptr && !retained->events->empty(),
				"A live provider callback must survive scheduled extraction and reach immutable retained event history.");
			if(retained && retained->events && !retained->events->empty()) {
				context.Expect(std::any_of(retained->events->begin(), retained->events->end(), [](const PhysicsEvent& event) {
					return event.type == PhysicsEventType::CollisionEnter
						|| event.type == PhysicsEventType::CollisionStay
						|| event.type == PhysicsEventType::Contact;
				}), "The retained packet must contain an actual collision/contact callback, not an empty bookkeeping publication.");
			}
		});

		registry.Add("Physics.Scheduler.ColliderOnlyObjectsBecomeStaticAndBodiesWithoutCollidersAreExcluded", [](TestContext& context) {
			using namespace TerraEngine::Physics;

			// Capture is the public worker-safe authoring boundary.  It must translate a
			// collider-only object into the implicit static body promised to creators.
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object colliderOnly = objects.createObject("Static Collider");
			const Object unsupportedBody = objects.createObject("Body Without Collider");
			context.Expect(objects.AddComponent<Transform>(colliderOnly) != nullptr
				&& objects.AddComponent<Transform>(unsupportedBody) != nullptr,
				"Both authoring objects need Transforms before the immutable physics snapshot is captured.");
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(colliderOnly);
			context.Expect(colliders != nullptr, "The collider-only fixture must own a valid collider collection.");
			colliders->Add();
			context.Expect(objects.AddComponent<RigidBodyComponent>(unsupportedBody) != nullptr,
				"The exclusion fixture must contain an explicit rigid body without a collider.");

			const PhysicsWorldHandle world{{1U}, 1U};
			const auto snapshot = PhysicsAuthoringSnapshot::Capture(objects, world, PhysicsSettings::Defaults(), 1U);
			context.Expect(snapshot.has_value(), "Valid collider-only authoring must produce an immutable physics snapshot.");
			context.ExpectEqual(static_cast<std::size_t>(1), snapshot->Data().bodies.size(),
				"Only collider-backed objects may reach physics-body preparation.");
			context.Expect(snapshot->Data().bodies.front().objectId == objects.getUUID(colliderOnly)
				&& snapshot->Data().bodies.front().motionType == PhysicsMotionType::Static,
				"A valid collider without an explicit RigidBodyComponent must become static geometry.");
			context.ExpectEqual(static_cast<std::size_t>(1), snapshot->Data().colliders.size(),
				"The implicit static body must retain its authored collider record.");
			context.ExpectEqual(static_cast<std::size_t>(1), snapshot->Data().diagnostics.size(),
				"The excluded rigid body must publish one bounded authoring diagnostic.");
			if(!snapshot->Data().diagnostics.empty()) {
				const PhysicsAuthoringDiagnostic& diagnostic = snapshot->Data().diagnostics.front();
				context.Expect(diagnostic.objectId == objects.getUUID(unsupportedBody),
					"The diagnostic must identify the exact excluded object.");
				context.Expect(diagnostic.code == "RigidBodyMissingValidCollider"
					&& diagnostic.message.find("Add or enable a ColliderSetComponent record") != std::string::npos,
					"The diagnostic must use a stable code and tell the creator how to repair the object.");
			}
			objects.clear();
		});
	}
}
