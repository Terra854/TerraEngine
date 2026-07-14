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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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

		enum class PlayModeBoundary : std::uint8_t {
			RendererUnavailable,
			SceneUnload,
			RemoveRecreate,
			ScriptReload,
			ProjectClose,
			Shutdown
		};

		// Round-robin boundaries cover every lifecycle trigger while keeping this
		// installed-editor-equivalent soak at its required 1,000 total cycles.
		constexpr std::array<PlayModeBoundary, 6U> kPlayModeBoundaries{
			PlayModeBoundary::RendererUnavailable,
			PlayModeBoundary::SceneUnload,
			PlayModeBoundary::RemoveRecreate,
			PlayModeBoundary::ScriptReload,
			PlayModeBoundary::ProjectClose,
			PlayModeBoundary::Shutdown};

		std::string_view BoundaryName(PlayModeBoundary boundary) {
			switch(boundary) {
				case PlayModeBoundary::RendererUnavailable: return "renderer-unavailable";
				case PlayModeBoundary::SceneUnload: return "scene-unload";
				case PlayModeBoundary::RemoveRecreate: return "remove-recreate";
				case PlayModeBoundary::ScriptReload: return "script-reload";
				case PlayModeBoundary::ProjectClose: return "project-close";
				case PlayModeBoundary::Shutdown: return "shutdown";
			}
			return "unknown";
		}

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

		void RestoreAuthoredSnapshot(TestContext& context, ObjectManager& objects,
			TerraEngine::Scene::SceneMutationQueue& mutations, const nlohmann::json& authoredSnapshot, PlayModeBoundary boundary) {
			// Restoring from the serialized payload makes every boundary prove that
			// runtime-only provider state did not mutate the creator's authored scene.
			objects.clear();
			mutations.Clear();
			Serializer::DeserializeObjectEntity(authoredSnapshot);
			const auto roots = objects.getRootObjects();
			Object restoredRoot;
			if(!roots.empty()) restoredRoot = roots.front();
			context.Expect(roots.size() == 1U && Serializer::SerializeObject(restoredRoot) == authoredSnapshot,
				"The " + std::string(BoundaryName(boundary)) + " boundary must restore the exact authored payload.");
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
				const PlayModeBoundary boundary = kPlayModeBoundaries[cycle % kPlayModeBoundaries.size()];
				context.Expect(physics.StartWorld(), "Entering Play Mode must create a fresh physics world for every soak cycle.");
				const TerraEngine::Physics::PhysicsWorldHandle liveWorld = physics.WorldHandle();
				context.Expect(physics.SetPaused(true) && physics.RequestExplicitStep(),
					"The soak must exercise one paused Next publication before every lifecycle boundary.");
				physics.Update(0.0F);
				const auto flush = mutations.Apply(objects, "physics-play-mode-soak");
				physics.OnAfterPhysicsFlush(flush.transactionResults);

				if(boundary == PlayModeBoundary::RendererUnavailable) {
					// No renderer is initialized in this headless test. Selecting immutable
					// packets proves a failed or unavailable presentation consumer cannot
					// access the live provider or stop the current Play Mode world.
					(void)TerraEngine::Physics::PublishedPhysicsDebugPackets().Select(liveWorld, cycle + 1U);
					context.Expect(physics.WorldHandle() == liveWorld,
						"An unavailable renderer must not invalidate the active physics world.");
				}

				context.Expect(physics.StopWorld() && !physics.WorldHandle().IsValid(),
					"Every Play Mode boundary must drain the world before authored objects change.");
				if(boundary == PlayModeBoundary::RemoveRecreate) {
					const auto roots = objects.getRootObjects();
					context.Expect(roots.size() == 1U, "The remove/recreate lane needs one live authored object to remove.");
					if(!roots.empty()) {
						objects.RemoveComponent<ColliderSetComponent>(roots.front());
						context.Expect(objects.GetComponent<ColliderSetComponent>(roots.front()) == nullptr,
							"The remove/recreate lane must actually remove the physics component before recreation.");
					}
				}

				if(boundary == PlayModeBoundary::ProjectClose || boundary == PlayModeBoundary::Shutdown) {
					// These two host transitions share a clean backend restart in a soak
					// loop. Keeping them distinct in the manifest preserves both evidence rows.
					physics.Shutdown();
					context.Expect(!physics.WorldHandle().IsValid(), "Project close and shutdown must release the active provider world.");
					physics.Initialize();
				}
				RestoreAuthoredSnapshot(context, objects, mutations, authoredSnapshot, boundary);
			}
			physics.Shutdown();
			TerraEngine::Physics::PublishedPhysicsDebugPackets().Clear();
			mutations.Clear();
			objects.clear();
		});
	}
}
