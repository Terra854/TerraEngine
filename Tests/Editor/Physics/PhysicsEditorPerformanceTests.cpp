// PhysicsEditorPerformanceTests.cpp - bounded editor performance-regression contracts.
//
// The large fixture exercises creation, serializer load, Play Mode entry, and
// paused Next behavior. It belongs to the dedicated performance CTest target so
// ordinary editor unit-test slices stay small and deterministic.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSystem.h"
#include "Renderer/RenderSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "ThreadPool/ThreadPool.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	namespace {
		constexpr std::size_t kLargeObjectCount = 10000U;
		constexpr std::size_t kDynamicBodyCount = 1000U;
		constexpr std::uint32_t kPausedNextRequestCount = 100U;

		// A local shared-pool lifetime prevents the high-scale editor fixture from
		// borrowing queued worker work from a prior test collection.
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

		// Tests continue after failures, so always return singleton state to an
		// empty editor-owned scene and stop provider work before clearing objects.
		class EditorPhysicsPerformanceCleanup final {
		public:
			~EditorPhysicsPerformanceCleanup() {
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Physics::PublishedPhysicsDebugPackets().Clear();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
			}
		};

		std::vector<nlohmann::json> CreateAndCaptureLargePhysicsScene(TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			std::vector<nlohmann::json> serializedObjects;
			serializedObjects.reserve(kLargeObjectCount);

			for(std::size_t index = 0U; index < kLargeObjectCount; ++index) {
				Object object = objects.createObject("Physics Performance Object " + std::to_string(index));
				Transform* transform = objects.GetComponent<Transform>(object);
				context.Expect(transform != nullptr,
					"Every created editor object must expose its default Transform before performance capture.");
				transform->position = {
					static_cast<float>(index % 100U),
					static_cast<float>(index / 100U),
					static_cast<float>((index / 100U) % 10U)};

				// One thousand representative dynamic bodies make Play Mode entry and
				// fixed steps meaningful while the other objects retain authoring scale.
				if(index < kDynamicBodyCount) {
					RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
					ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
					context.Expect(body != nullptr && colliders != nullptr,
						"Each performance dynamic must create a rigid body and a collider set.");
					body->useGravity = true;
					colliders->Add();
				}
				objects.MarkObjectTransformDirty(object);
				serializedObjects.push_back(Serializer::SerializeObject(object));
			}
			return serializedObjects;
		}

		void RestoreLargePhysicsScene(TestContext& context, const std::vector<nlohmann::json>& serializedObjects) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			for(const nlohmann::json& serializedObject : serializedObjects) {
				context.Expect(Serializer::DeserializeObjectEntity(serializedObject) != entt::null,
					"Each captured large-scene object must deserialize into a live editor entity.");
			}
			context.ExpectEqual(kLargeObjectCount, objects.getRootObjects().size(),
				"Loading the captured large scene must restore every authored root object.");
		}
	}

	void RegisterPhysicsEditorPerformanceTests(TestRegistry& registry) {
		registry.Add("Physics.Editor.Performance.TenThousandObjectCreationLoadPlayModeAndPausedNextRemainObservableWithoutRenderer", [](TestContext& context) {
			ThreadPoolSession pool;
			EditorPhysicsPerformanceCleanup cleanup;
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();

			const std::vector<nlohmann::json> serializedObjects = CreateAndCaptureLargePhysicsScene(context);
			context.ExpectEqual(kLargeObjectCount, serializedObjects.size(),
				"The performance capture must serialize every one of the ten thousand created objects.");
			RestoreLargePhysicsScene(context, serializedObjects);

			auto& physics = Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(),
				"Large-scene Play Mode entry must build a fresh physics world from restored authored data.");
			context.Expect(physics.SetPaused(true),
				"The large-scene performance fixture must pause before issuing exact Next requests.");
			const std::uint64_t publicationsBefore = physics.ExplicitStepPublicationCount();
			for(std::uint32_t request = 0U; request < kPausedNextRequestCount; ++request) {
				context.Expect(physics.RequestExplicitStep(),
					"Each bounded paused Next request must be admitted before the following protected publication.");
				physics.Update(0.0F);
				const auto flush = mutations.Apply(objects, "editor-physics-performance-next");
				physics.OnAfterPhysicsFlush(flush.transactionResults);
			}
			context.ExpectEqual(publicationsBefore + kPausedNextRequestCount, physics.ExplicitStepPublicationCount(),
				"One hundred paused Next requests must publish exactly one fixed physics result each.");
			context.Expect(physics.IsPaused(),
				"The one-hundred-request performance pass must remain paused after every explicit fixed tick.");

			const auto diagnostics = physics.LatestDiagnostics();
			context.Expect(diagnostics != nullptr && diagnostics->counters.bodyCount >= kDynamicBodyCount,
				"Large-scene diagnostics must publish the dynamic-body count after Play Mode entry and fixed ticks.");
			context.Expect(diagnostics && diagnostics->scheduler != nullptr && !diagnostics->scheduler->empty(),
				"Diagnostic publication must retain scheduler declarations without reading a provider object.");
			const auto packets = Physics::PublishedPhysicsDebugPackets().Select(physics.WorldHandle(), diagnostics ? diagnostics->tick : 0U);
			// Explicitly remove the render backend so this check cannot accidentally
			// pass through an RHI left alive by an unrelated editor test fixture.
			RenderSystem::Instance().Shutdown();
			context.Expect(!RenderSystem::Instance().SubmitSceneViewPhysicsDebugPackets(packets, {}),
				"Renderer-unavailable diagnostics submission must fail safely without changing the physics workload.");
			context.Expect(physics.StopWorld(),
				"Large-scene Play Mode exit must drain provider work before the performance fixture clears authored objects.");
		});
	}
}
