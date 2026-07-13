// Editor Play Mode physics lifecycle regression tests.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/Serializer.h"
#include "ThreadPool/ThreadPool.h"

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
	}

	void RegisterPhysicsPlayModeTests(TestRegistry& registry) {
		registry.Add("Physics.Editor.PlayPauseNextStopRestoresExactAuthoredState", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			TerraEngine::Scene::SceneMutationQueue& mutations = TerraEngine::Scene::SceneMutationQueue::Instance();
			objects.clear();
			mutations.Clear();

			Object authoredObject = objects.createObject("Play Mode Physics Body");
			Transform* authoredTransform = objects.AddComponent<Transform>(authoredObject);
			RigidBodyComponent* rigidBody = objects.AddComponent<RigidBodyComponent>(authoredObject);
			ColliderSetComponent* colliders = objects.AddComponent<ColliderSetComponent>(authoredObject);
			context.Expect(authoredTransform != nullptr && rigidBody != nullptr && colliders != nullptr,
				"The editor fixture must create every authored primitive-body component.");
			authoredTransform->position = {0.0F, 5.0F, 0.0F};
			rigidBody->useGravity = true;
			colliders->Add();
			objects.MarkObjectTransformDirty(authoredObject);
			const nlohmann::json authoredSnapshot = Serializer::SerializeObject(authoredObject);

			ThreadPoolSession pool;
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			physics.Shutdown();
			physics.SetEditorLifecycleControlled(true);
			physics.Initialize();
			context.Expect(physics.StartWorld(), "Play must create a fresh Jolt 5.6 physics world before scripting starts.");
			const TerraEngine::Physics::PhysicsWorldHandle firstWorld = physics.WorldHandle();
			context.Expect(firstWorld.IsValid(), "The active Play session must expose a generation-safe world handle.");
			context.Expect(physics.SetPaused(true), "Pause must suspend the shared physics world.");
			const std::uint64_t publicationsBefore = physics.ExplicitStepPublicationCount();
			context.Expect(physics.RequestExplicitStep(), "Paused Next must admit exactly one explicit physics token.");
			physics.Update(100.0F);
			const auto flush = mutations.Apply(objects, "editor-physics-play-next");
			physics.OnAfterPhysicsFlush(flush.transactionResults);
			context.ExpectEqual(publicationsBefore + 1U, physics.ExplicitStepPublicationCount(),
				"Next must publish exactly one configured tick after the protected scene flush.");
			context.Expect(physics.IsPaused(), "Next must leave the shared world paused.");

			context.Expect(physics.StopWorld(), "Stop must drain provider work before authored objects are restored.");
			context.Expect(!physics.WorldHandle().IsValid(), "No provider body handle may survive the Stop boundary.");
			objects.clear();
			Serializer::DeserializeObjectEntity(authoredSnapshot);
			auto roots = objects.getRootObjects();
			context.ExpectEqual(static_cast<std::size_t>(1), roots.size(), "Restore must reconstruct exactly one authored root.");
			Object restoredObject = roots.front();
			context.Expect(Serializer::SerializeObject(restoredObject) == authoredSnapshot,
				"Play-time Transform and provider state must not alter the exact authored scene payload.");

			context.Expect(physics.StartWorld(), "A later Play action must create another fresh provider world.");
			context.Expect(physics.WorldHandle().IsValid() && physics.WorldHandle() != firstWorld,
				"World generations must change across Play sessions so stale wrappers cannot revive.");
			context.Expect(physics.StopWorld(), "The second Play fixture world must drain cleanly.");
			physics.Shutdown();
			mutations.Clear();
			objects.clear();
		});
	}
}
