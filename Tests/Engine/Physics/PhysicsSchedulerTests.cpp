#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/Transform.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/TransactionalMutation.h"

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

		registry.Add("Physics.Scheduler.SevenWorkloadsHaveStrictGlobalOrder", [](TestContext& context) {
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
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> physicsWorkloads;
			std::copy_if(workloads.begin(), workloads.end(), std::back_inserter(physicsWorkloads), [](const auto& workload) {
				return workload.ownerSystem == "PhysicsSystem";
			});
			context.ExpectEqual(static_cast<std::size_t>(7), physicsWorkloads.size(),
				"PhysicsSystem must lower exactly seven named workloads.");
			for(std::size_t index = 1U; index < physicsWorkloads.size(); ++index) {
				context.Expect(physicsWorkloads[index - 1U].executionOrder < physicsWorkloads[index].executionOrder,
					"Every local physics workload must receive a unique increasing global order.");
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
	}
}
