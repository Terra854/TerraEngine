// Editor Play Mode physics lifecycle regression tests.

#include "TestHarness.h"

#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/BulkScenePublication.h"
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
			TerraEngine::Scene::WorldPose authoredPose;
			authoredPose.position.offsetMetres = {0.0, 5.0, 0.0};
			context.Expect(objects.SetCanonicalWorldPose(authoredObject, authoredPose) == TerraEngine::Scene::WorldCoordinateError::None,
				"The Play Mode fixture canonical pose must be accepted.");
			rigidBody->useGravity = true;
			colliders->Add();
			objects.MarkObjectTransformDirty(authoredObject);
			const nlohmann::json authoredSnapshot = Serializer::SerializeObject(authoredObject);
			const TerraEngine::Scene::PreparedSceneDecode preparedRestore = Serializer::PrepareSceneDecode({
				{"format", TerraEngine::Scene::kTerraSceneUnboundedFormat},
				{"name", "Play Mode Physics"},
				{"objects", nlohmann::json::array({authoredSnapshot})}
			});
			const TerraEngine::Scene::BulkScenePublicationPacket authoredRestorePacket =
				TerraEngine::Scene::BuildBulkScenePublicationPacket(
					preparedRestore,
					objects.GetSceneGeneration(),
					objects.GetSceneGeneration());
			context.Expect(preparedRestore.accepted && !authoredRestorePacket.cancelled,
				"Play Mode must prepare an authored bulk packet without a JSON text dump/reparse round trip.");

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
			physics.OnAfterPhysicsFlush(flush.transactionResults, flush.publicationGroupResults);
			context.ExpectEqual(publicationsBefore + 1U, physics.ExplicitStepPublicationCount(),
				"Next must publish exactly one configured tick after the protected scene flush.");
			context.Expect(physics.IsPaused(), "Next must leave the shared world paused.");

			context.Expect(physics.StopWorld(), "Stop must drain provider work before authored objects are restored.");
			context.Expect(!physics.WorldHandle().IsValid(), "No provider body handle may survive the Stop boundary.");
			const TerraEngine::Scene::BulkScenePublicationResult restored =
				TerraEngine::Scene::PublishBulkScenePacket(authoredRestorePacket, objects);
			context.Expect(restored.published && !restored.rolledBack,
				"The protected Play Mode stop boundary must publish a complete authored bulk packet.");
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
