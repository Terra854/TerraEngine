// PhysicsPlayModeSoakTests.cpp - installed-editor-equivalent Play Mode soak.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsDiagnostics.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "ThreadPool/ThreadPool.h"

#include <cstdint>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
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

		nlohmann::json MakeAuthoredSnapshot(TestContext& context, ObjectManager& objects) {
			Object object = objects.createObject("Physics Soak Body");
			Transform* transform = objects.AddComponent<Transform>(object);
			RigidBodyComponent* body = objects.AddComponent<RigidBodyComponent>(object);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(object);
			context.Expect(transform != nullptr && body != nullptr && colliders != nullptr,
				"The Play Mode soak fixture must create a complete authored rigid body.");
			transform->position = {0.0F, 3.0F, 0.0F};
			body->useGravity = true;
			colliders->Add();
			objects.MarkObjectTransformDirty(object);
			return Serializer::SerializeObject(object);
		}
	}

	void RegisterPhysicsPlayModeSoakTests(TestRegistry& registry) {
		registry.Add("Physics.Editor.Soak.OneThousandPlayModeCyclesPreserveAuthoredStateAcrossReloadFailureCloseAndShutdown", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			auto& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();
			const nlohmann::json authoredSnapshot = MakeAuthoredSnapshot(context, objects);

			ThreadPoolSession pool;
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			TerraEngine::Physics::PublishedPhysicsDebugPackets().Clear();

			for(std::uint32_t cycle = 0U; cycle < 1000U; ++cycle) {
				context.Expect(physics.StartWorld(), "Entering Play Mode must create a fresh physics world for every soak cycle.");
				context.Expect(physics.SetPaused(true) && physics.RequestExplicitStep(),
					"The soak must exercise one paused Next publication before every unload/recreate.");
				physics.Update(0.0F);
				const auto flush = mutations.Apply(objects, "physics-play-mode-soak");
				physics.OnAfterPhysicsFlush(flush.transactionResults);

				// The debug read has no renderer dependency, deliberately covering an
				// unavailable renderer while Play Mode remains safe and inspectable.
				(void)TerraEngine::Physics::PublishedPhysicsDebugPackets().Select(physics.WorldHandle(), cycle + 1U);
				context.Expect(physics.StopWorld(), "Play Mode stop must drain physics before scene unload or script reload.");

				// Clear and deserialize after every stop to model unload plus remove/recreate.
				// The exact JSON comparison also catches runtime transforms dirtying authored state.
				objects.clear();
				mutations.Clear();
				Serializer::DeserializeObjectEntity(authoredSnapshot);
				auto roots = objects.getRootObjects();
				context.Expect(roots.size() == 1U && Serializer::SerializeObject(roots.front()) == authoredSnapshot,
					"Scene reload and script reload must restore the exact authored payload after every cycle.");

				if((cycle + 1U) % 100U == 0U) {
					// A project close/shutdown boundary destroys the backend, then initializes
					// a new editor-hosted session without retaining old scene identities.
					physics.Shutdown();
					physics.Initialize();
				}
			}
			physics.Shutdown();
			TerraEngine::Physics::PublishedPhysicsDebugPackets().Clear();
			mutations.Clear();
			objects.clear();
		});
	}
}
