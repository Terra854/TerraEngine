// LocomotionLifecycleTests.cpp - live ECS and Physics lifecycle regressions.

#include "../TestHarness.h"

#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <cstdint>
#include <span>
#include <sstream>

namespace TerraEngine::Tests {
	namespace {
		// The real system singletons are intentionally isolated and drained in the
		// same order used by the Physics lifecycle tests.
		class LocomotionRuntimeGuard final {
		public:
			LocomotionRuntimeGuard() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				auto& physics = Physics::PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
			}

			~LocomotionRuntimeGuard() {
				Locomotion::LocomotionSystem::Instance().Shutdown();
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Start(TestContext& context) {
				auto& physics = Physics::PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The lifecycle fixture must start a real Physics world.");
				return physics.WorldHandle().IsValid();
			}

			void PublishCharacters() {
				auto& objects = ObjectManager::instance();
				auto& physics = Physics::PhysicsSystem::Instance();
				// The first publication creates provider characters; one more extraction
				// makes their generation-qualified state available to Locomotion.
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-lifecycle-physics-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
			}
		};

		Object MakeLocomotionCharacter(TestContext& context) {
			auto& objects = ObjectManager::instance();
			const Object object = objects.createObject("Locomotion lifecycle character");
			context.Expect(objects.AddComponent<Transform>(object) != nullptr
				&& objects.AddComponent<CharacterControllerComponent>(object) != nullptr
				&& objects.AddComponent<CharacterLocomotionComponent>(object) != nullptr,
				"A lifecycle fixture needs Transform, CharacterController, and CharacterLocomotion components.");
			return object;
		}

		Locomotion::CommandBatchAdmission SubmitStaleAcquire(Locomotion::LocomotionHandle handle) {
			Locomotion::LocomotionCommand command;
			command.kind = Locomotion::CommandKind::Acquire;
			command.handle = handle;
			command.expectedAuthorityEpoch = 1U;
			command.expectedConfigurationRevision = 1U;
			command.producerId = 91U;
			command.sequence = 1U;
			command.correlationId = 91U;
			return Locomotion::LocomotionSystem::Instance().SubmitBatch(std::span(&command, 1U));
		}
	}

	void RegisterLocomotionLifecycleTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Lifecycle.ControllerRemoveReaddInvalidatesOldWrapper", [](TestContext& context) {
			LocomotionRuntimeGuard runtime;
			ObjectManager& objects = ObjectManager::instance();
			const Object object = MakeLocomotionCharacter(context);
			if(!runtime.Start(context)) return;
			Locomotion::LocomotionSystem& locomotion = Locomotion::LocomotionSystem::Instance();
			locomotion.Initialize();
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			const UUID objectId = objects.getUUID(object);
			const auto oldHandle = locomotion.ResolvePublished(objectId);
			context.Expect(oldHandle.has_value(), "A live published Physics controller must bind its locomotion component.");
			if(!oldHandle) return;

			objects.RemoveComponent<CharacterControllerComponent>(object);
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			context.Expect(!locomotion.ResolvePublished(objectId).has_value(),
				"Removing the controller must revoke and remove its published locomotion binding.");

			context.Expect(objects.AddComponent<CharacterControllerComponent>(object) != nullptr,
				"The same authored object must accept a replacement controller component.");
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			const auto replacement = locomotion.ResolvePublished(objectId);
			auto packet = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
			std::ostringstream detail;
			detail << "Controller re-add diagnostic: packet=" << static_cast<bool>(packet)
				<< ", stateCount=" << (packet && packet->states ? packet->states->size() : 0U)
				<< ", ECS objectGeneration=" << objects.getObjectLifetimeGeneration(object)
				<< ", ECS componentGeneration=" << objects.getObjectComponentGeneration(object)
				<< ", oldHandle attachmentGeneration=" << oldHandle->attachmentGeneration
				<< ", old slot=" << oldHandle->slotIndex << ':' << oldHandle->slotGeneration
				<< ", replacementPublished=" << replacement.has_value()
				<< ", liveSlots=" << locomotion.ReadDiagnostics().liveSlots;
			bool sawObjectState = false;
			if(packet && packet->states) {
				for(const auto& state : *packet->states) {
					if(state.objectId != objectId) continue;
					sawObjectState = true;
					detail << ", currentState handle=" << state.handle.slotIndex << ':' << state.handle.slotGeneration
						<< ", objectGeneration=" << state.objectGeneration
						<< ", componentGeneration=" << state.componentGeneration;
				}
			}
			detail << ", sawObjectState=" << sawObjectState;
			context.Expect(replacement.has_value(), detail.str());
			if(replacement) context.Expect(*replacement != *oldHandle,
				"Controller re-add must publish a fresh slot generation rather than revive the cached wrapper.");
			const auto stale = SubmitStaleAcquire(*oldHandle);
			context.Expect(!stale.queued && stale.reason == Locomotion::LocomotionReason::StaleHandle,
				"An Acquire sent with the old controller attachment handle must be rejected as stale.");
			context.Expect(locomotion.ReadDiagnostics().liveSlots == 1U,
				"Only the replacement controller attachment may remain live after re-add.");
		});

		registry.Add("Engine.Locomotion.Lifecycle.ComponentRemoveReaddInvalidatesOldWrapper", [](TestContext& context) {
			LocomotionRuntimeGuard runtime;
			ObjectManager& objects = ObjectManager::instance();
			const Object object = MakeLocomotionCharacter(context);
			if(!runtime.Start(context)) return;
			Locomotion::LocomotionSystem& locomotion = Locomotion::LocomotionSystem::Instance();
			locomotion.Initialize();
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			const UUID objectId = objects.getUUID(object);
			const auto oldHandle = locomotion.ResolvePublished(objectId);
			context.Expect(oldHandle.has_value(), "The initial component attachment must be published.");
			if(!oldHandle) return;

			objects.RemoveComponent<CharacterLocomotionComponent>(object);
			locomotion.ObserveProtectedBoundary();
			context.Expect(!locomotion.ResolvePublished(objectId).has_value(),
				"Removing CharacterLocomotionComponent must revoke the old attachment immediately at the protected boundary.");
			context.Expect(objects.AddComponent<CharacterLocomotionComponent>(object) != nullptr,
				"The same object must accept a replacement locomotion component.");
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			const auto replacement = locomotion.ResolvePublished(objectId);
			context.Expect(replacement.has_value() && *replacement != *oldHandle,
				"Re-adding CharacterLocomotionComponent must create a new wrapper identity.");
			const auto stale = SubmitStaleAcquire(*oldHandle);
			context.Expect(!stale.queued && stale.reason == Locomotion::LocomotionReason::StaleHandle
				&& locomotion.ReadDiagnostics().liveSlots == 1U,
				"A cached wrapper must stay stale and cannot create a second live owner after component re-add.");
		});

		registry.Add("Engine.Locomotion.Lifecycle.SceneAndWorldTeardownRevokeBindings", [](TestContext& context) {
			LocomotionRuntimeGuard runtime;
			ObjectManager& objects = ObjectManager::instance();
			const Object object = MakeLocomotionCharacter(context);
			if(!runtime.Start(context)) return;
			Locomotion::LocomotionSystem& locomotion = Locomotion::LocomotionSystem::Instance();
			locomotion.Initialize();
			runtime.PublishCharacters();
			locomotion.ObserveProtectedBoundary();
			const UUID objectId = objects.getUUID(object);
			const auto sceneHandle = locomotion.ResolvePublished(objectId);
			context.Expect(sceneHandle.has_value(), "The scene teardown fixture must begin with a published binding.");
			if(!sceneHandle) return;

			objects.clear();
			locomotion.ObserveProtectedBoundary();
			context.Expect(!locomotion.ResolvePublished(objectId).has_value()
				&& locomotion.ReadDiagnostics().liveSlots == 0U,
				"Scene replacement must clear every live locomotion binding and slot.");
			const auto staleSceneCommand = SubmitStaleAcquire(*sceneHandle);
			context.Expect(!staleSceneCommand.queued
				&& staleSceneCommand.reason == Locomotion::LocomotionReason::StaleHandle,
				"A command copied before scene replacement must not address the cleared attachment.");

			// The valid Physics world may outlive the scene transition; stopping it is
			// a second independent teardown path and must leave no controller handle.
			context.Expect(Physics::PhysicsSystem::Instance().StopWorld(),
				"The fixture must stop its remaining Physics world explicitly.");
			locomotion.ObserveProtectedBoundary();
			context.Expect(locomotion.ReadDiagnostics().liveSlots == 0U
				&& !locomotion.ResolvePublished(objectId).has_value(),
				"World teardown must keep the locomotion authority store empty.");
		});
	}
}
