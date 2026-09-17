#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Physics/PhysicsAuthoringSnapshot.h"
#include "Physics/PhysicsCommands.h"
#include "Physics/PhysicsEvents.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"
#include "Scripting/ScriptGlue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <optional>

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
					TerraEngine::Scene::WorldPose pose;
					pose.position.offsetMetres = {4.0, 5.0, 6.0};
					builder.SetWorldTransform(objects, targetObject, pose);
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
				const auto pose = ObjectManager::instance().GetCanonicalWorldPose(targetObject);
				observedPublishedTransform = transform != nullptr && pose
					&& pose->position.offsetMetres == glm::dvec3(4.0, 5.0, 6.0);
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
				"PhysicsPublishedSnapshots", TerraEngine::Scheduling::kResourcePhysicsCharacterStatePackets, "PhysicsTransformTransactions"}) {
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
			if(supportTransform != nullptr) supportTransform->worldPose.position.offsetMetres = {0.0, 0.0, 0.0};
			if(dynamicTransform != nullptr) dynamicTransform->worldPose.position.offsetMetres = {0.0, 0.25, 0.0};
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
			const Object controllerOnly = objects.createObject("Controller With Disabled Own Collider");
			context.Expect(objects.AddComponent<Transform>(colliderOnly) != nullptr
				&& objects.AddComponent<Transform>(unsupportedBody) != nullptr
				&& objects.AddComponent<Transform>(controllerOnly) != nullptr,
				"Every authoring object needs a Transform before the immutable physics snapshot is captured.");
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(colliderOnly);
			context.Expect(colliders != nullptr, "The collider-only fixture must own a valid collider collection.");
			colliders->Add();
			context.Expect(objects.AddComponent<RigidBodyComponent>(unsupportedBody) != nullptr,
				"The exclusion fixture must contain an explicit rigid body without a collider.");
			ColliderSetComponent* disabledControllerCollider = objects.AddComponent<ColliderSetComponent>(controllerOnly);
			CharacterControllerComponent* controller = objects.AddComponent<CharacterControllerComponent>(controllerOnly);
			context.Expect(disabledControllerCollider != nullptr && controller != nullptr,
				"The character-only fixture must retain both independently-authored components.");
			if(disabledControllerCollider != nullptr) disabledControllerCollider->active = false;

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
			context.ExpectEqual(static_cast<std::size_t>(1), snapshot->Data().characters.size(),
				"An enabled controller must be captured even when its own ColliderSet is intentionally inactive.");
			const auto generation = snapshot
				? PhysicsAuthoringSnapshot::BuildGeneration(nullptr, *snapshot)
				: std::nullopt;
			context.Expect(generation && generation->Data().characterObjectIds.size() == 1U
				&& generation->Data().characterObjectIds.front() == objects.getUUID(controllerOnly),
				"A controller-only owner must enter the typed character closure instead of being skipped with collider-only body changes.");
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

		registry.Add("Physics.Scheduler.CharacterCommandsAcceptUnusedZeroPoseFields", [](TestContext& context) {
			using namespace TerraEngine::Physics;
			ScheduledPhysicsRuntimeGuard runtimeGuard;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			const Object controllerOnly = objects.createObject("Deferred Character Commands");
			context.Expect(objects.AddComponent<Transform>(controllerOnly) != nullptr
				&& objects.AddComponent<CharacterControllerComponent>(controllerOnly) != nullptr,
				"The command fixture must author one controller without a body or ColliderSet.");
			// Imported meshes often use a small scale. The provider publishes only a
			// rigid pose, so initializing or moving this controller must not resize it.
			Scene::WorldPose authoredPose;
			authoredPose.scale = {0.01F, 0.02F, 0.03F};
			context.Expect(objects.SetCanonicalWorldPose(controllerOnly, authoredPose) == Scene::WorldCoordinateError::None,
				"The character fixture must retain its authored non-unit visual scale.");

			auto& physics = PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "The deferred-character command fixture must start its isolated world.");
			const auto advancePhysics = [&]() {
				(void)objects.PublishSceneChangePacket();
				physics.Update(physics.FixedTimeStep());
			};
			const auto flushPhysics = [&]() {
				const auto flushed = mutations.Apply(objects, "physics-character-command-zero-pose");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			};
			const auto advanceFrame = [&]() {
				advancePhysics();
				flushPhysics();
			};
			std::shared_ptr<const PhysicsCharacterStatePacket> states;
			for(std::uint32_t frame = 0U; frame < 3U && (!states || !states->states || states->states->empty()); ++frame) {
				advanceFrame();
				states = physics.LatestCharacterStatePacket();
			}
			context.Expect(states != nullptr && states->states != nullptr && states->states->size() == 1U,
				"The controller-only fixture must publish a generation-validated runtime character before command submission.");
			if(states == nullptr || states->states == nullptr || states->states->empty()) return;
			Transform* transform = objects.GetComponent<Transform>(controllerOnly);
			context.Expect(transform != nullptr, "The controller command fixture must retain its visible Transform target.");
			if(transform == nullptr) return;
			context.Expect(transform->worldPose.scale == authoredPose.scale,
				"Controller initialization must not reset the authored mesh scale.");
			const double initialX = transform->worldPose.position.offsetMetres.x;
			const PhysicsCharacterState& state = states->states->front();
			const PhysicsCharacterCommandTarget target{state.world, state.handle, state.objectId,
				state.objectGeneration, state.componentGeneration};
			const auto metadata = [](PhysicsSequence sequence) {
				PhysicsCommandMetadata result;
				result.producerId = 2U;
				result.producerSequence = sequence;
				return result;
			};
			const PhysicsCommandReceipt move = physics.SubmitCommand(PhysicsCommand::MakeCharacterMove(
				target, {3.0F, 0.0F, 0.0F}, metadata(1U)));
			const PhysicsCommandReceipt jump = physics.SubmitCommand(PhysicsCommand::MakeCharacterJump(
				target, 2.0F, metadata(2U)));
			const PhysicsCommandReceipt resize = physics.SubmitCommand(PhysicsCommand::MakeCharacterResize(
				target, 2.0F, metadata(3U)));
			context.Expect(move.accepted && jump.accepted && resize.accepted,
				"Move, jump, and resize must enqueue through PhysicsSystem even though their ABI pose fields are zero-initialized.");

			NativePhysicsCharacterState nativeState{};
			nativeState.header = {kNativePhysicsAbiVersion, sizeof(nativeState)};
			context.Expect(Native_Physics_ResolveCharacter(static_cast<std::uint32_t>(controllerOnly.EntityId()), &nativeState)
				== NativePhysicsStatus::Success,
				"The copied scripting ABI must resolve the same published controller target used by gameplay commands.");
			const auto submitNative = [&nativeState](NativePhysicsCharacterCommandKind kind, float scalar, std::uint64_t sequence) {
				NativePhysicsCharacterCommandRequest request{};
				request.header = {kNativePhysicsAbiVersion, sizeof(request)};
				request.character = nativeState.character;
				request.identity = nativeState.identity;
				request.kind = kind;
				request.scalar = scalar;
				request.producerId = 1U;
				request.producerSequence = sequence;
				NativePhysicsCommandReceipt receipt{};
				receipt.header = {kNativePhysicsAbiVersion, sizeof(receipt)};
				return Native_Physics_SubmitCharacterCommand(&request, &receipt) == NativePhysicsStatus::Accepted
					&& receipt.status == NativePhysicsStatus::Accepted;
			};
			context.Expect(submitNative(NativePhysicsCharacterCommandKind::Move, 0.0F, 1U)
				&& submitNative(NativePhysicsCharacterCommandKind::Jump, 2.0F, 2U)
				&& submitNative(NativePhysicsCharacterCommandKind::Resize, 2.0F, 3U),
				"The exported character ABI must admit move, jump, and resize with its intentionally default zero pose fields.");
			advancePhysics();
			context.Expect(std::abs(transform->worldPose.position.offsetMetres.x - initialX) < 0.001,
				"A prepared controller pose must remain invisible until the declared Physics flush boundary.");
			flushPhysics();
			const auto results = physics.LatestCommandResults();
			const auto succeeded = [results](PhysicsCommandId commandId) {
				return results != nullptr && results->results != nullptr
					&& std::any_of(results->results->begin(), results->results->end(),
						[commandId](const PhysicsCommandResult& result) {
							return result.commandId == commandId && result.status == PhysicsResultStatus::Success;
						});
			};
			context.Expect(succeeded(move.commandId) && succeeded(jump.commandId) && succeeded(resize.commandId),
				"PhysicsCommandCommit must accept all non-teleport character commands without validating their unused zero pose rotations.");
			context.Expect(transform->worldPose.position.offsetMetres.x > initialX + 0.001,
				"A controller-only character pose must reach its Transform through the declared deferred Physics publication boundary.");
			context.Expect(transform->worldPose.scale == authoredPose.scale,
				"Controller movement must preserve non-unit scale after protected pose publication.");
		});
	}
}
