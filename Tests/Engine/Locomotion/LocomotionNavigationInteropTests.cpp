// LocomotionNavigationInteropTests.cpp - execute the character-command ABI path
// used by managed route following against real Physics-owned characters.

#include "../TestHarness.h"

#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptGlue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;

		constexpr float kFixedDeltaTime = 1.0F / 60.0F;

		NativePhysicsUuid ToNativeUuid(const UUID& value) {
			NativePhysicsUuid result{};
			std::memcpy(&result.first, value.data(), sizeof(result.first));
			std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
			return result;
		}

		NativePhysicsHandle ToNativeHandle(PhysicsCharacterHandle value) {
			return {{value.worldId.value, value.worldGeneration, 0U}, value.slotIndex, value.slotGeneration};
		}

		const PhysicsCharacterState* FindCharacter(const PhysicsCharacterStatePacket& packet, UUID objectId) {
			if(!packet.states) return nullptr;
			const auto found = std::find_if(packet.states->begin(), packet.states->end(),
				[objectId](const PhysicsCharacterState& state) { return state.objectId == objectId; });
			return found == packet.states->end() ? nullptr : &*found;
		}

		NativePhysicsCharacterCommandRequest MakeMoveRequest(
			const PhysicsCharacterState& state, std::uint32_t bindingGeneration,
			float velocityX, std::uint64_t sequence, std::uint64_t correlation) {
			NativePhysicsCharacterCommandRequest request{};
			request.header = {kNativePhysicsAbiVersion, sizeof(request)};
			request.character = ToNativeHandle(state.handle);
			request.identity.objectId = ToNativeUuid(state.objectId);
			request.identity.objectGeneration = static_cast<std::uint32_t>(state.objectGeneration);
			request.identity.componentGeneration = static_cast<std::uint32_t>(state.componentGeneration);
			request.identity.bindingGeneration = bindingGeneration;
			request.kind = NativePhysicsCharacterCommandKind::Move;
			request.desiredVelocity = {velocityX, 0.0F, 0.0F};
			request.producerId = 71U;
			request.producerSequence = sequence;
			request.correlationId = correlation;
			return request;
		}

		class InteropRuntime final {
		public:
			InteropRuntime() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				auto& physics = PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
			}

			~InteropRuntime() {
				LocomotionSystem::Instance().Shutdown();
				auto& physics = PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Start(TestContext& context) {
				auto& objects = ObjectManager::instance();
				const Object owned = objects.createObject("ABI leased character");
				const Object unowned = objects.createObject("ABI unleased character");
				ownedId = objects.getUUID(owned);
				unownedId = objects.getUUID(unowned);
				Transform* ownedTransform = objects.AddComponent<Transform>(owned);
				Transform* unownedTransform = objects.AddComponent<Transform>(unowned);
				if(unownedTransform != nullptr) unownedTransform->worldPose.position.offsetMetres.x = 5.0;
				context.Expect(ownedTransform != nullptr && unownedTransform != nullptr
					&& objects.AddComponent<CharacterControllerComponent>(owned) != nullptr
					&& objects.AddComponent<CharacterLocomotionComponent>(owned) != nullptr
					&& objects.AddComponent<Transform>(unowned) != nullptr
					&& objects.AddComponent<CharacterControllerComponent>(unowned) != nullptr,
					"The fixture needs one leased controller and one ordinary unleased controller.");
				auto& physics = PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The fixture Physics world must start.");
				LocomotionSystem::Instance().Initialize();
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-navigation-interop-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
				LocomotionSystem::Instance().ObserveProtectedBoundary();
				return LocomotionSystem::Instance().ResolvePublished(ownedId).has_value();
			}

			UUID ownedId = EMPTY_UUID;
			UUID unownedId = EMPTY_UUID;
		};
	}

	void RegisterLocomotionNavigationInteropTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.NavigationInterop.NativeCharacterBridgeRespectsLease", [](TestContext& context) {
			InteropRuntime runtime;
			if(!runtime.Start(context)) {
				context.Expect(false, "The fixture must publish the leased character handle.");
				return;
			}

			auto& objects = ObjectManager::instance();
			auto& locomotion = LocomotionSystem::Instance();
			const auto handle = locomotion.ResolvePublished(runtime.ownedId);
			const auto ownedObject = objects.getObject(runtime.ownedId);
			const auto unleasedObject = objects.getObject(runtime.unownedId);
			context.Expect(handle.has_value() && ownedObject.has_value() && unleasedObject.has_value(),
				"Both fixture objects must remain published before bridge submissions.");
			if(!handle || !ownedObject || !unleasedObject) return;

			LocomotionCommand acquire{};
			acquire.kind = CommandKind::Acquire;
			acquire.handle = *handle;
			acquire.expectedConfigurationRevision = 1U;
			acquire.producerId = 70U;
			acquire.sequence = 1U;
			acquire.correlationId = 7001U;
			context.Expect(locomotion.SubmitBatch(std::span(&acquire, 1U)).queued,
				"Locomotion ownership must be admitted through the public typed command lane.");
			locomotion.Update(kFixedDeltaTime);
			const auto acquired = locomotion.ReadReceipt(*handle, acquire.producerId,
				acquire.sequence, acquire.correlationId);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The protected Locomotion phase must publish a native owner token.");
			if(acquired.state != ReceiptState::Applied) return;

			auto packet = PhysicsSystem::Instance().LatestCharacterStatePacket();
			context.Expect(packet && packet->states, "Physics must publish both controller identities.");
			if(!packet || !packet->states) return;
			const PhysicsCharacterState* ownedBefore = FindCharacter(*packet, runtime.ownedId);
			const PhysicsCharacterState* unownedBefore = FindCharacter(*packet, runtime.unownedId);
			context.Expect(ownedBefore && unownedBefore, "Both characters must appear in the copied Physics packet.");
			if(!ownedBefore || !unownedBefore) return;
			const PhysicsCharacterState ownedState = *ownedBefore;
			const PhysicsCharacterState unownedState = *unownedBefore;
			const std::uint32_t bindingGeneration = static_cast<std::uint32_t>(
				objects.getObjectBindingGeneration(*unleasedObject));

			NativePhysicsCommandReceipt leasedReceipt{};
			leasedReceipt.header = {kNativePhysicsAbiVersion, sizeof(leasedReceipt)};
			auto leasedMove = MakeMoveRequest(ownedState,
				static_cast<std::uint32_t>(objects.getObjectBindingGeneration(*ownedObject)),
				6.0F, 1U, 6001U);
			const NativePhysicsStatus leasedStatus = Native_Physics_SubmitCharacterCommand(&leasedMove, &leasedReceipt);

			NativePhysicsCommandReceipt unleasedReceipt{};
			unleasedReceipt.header = {kNativePhysicsAbiVersion, sizeof(unleasedReceipt)};
			// Producer ordering is global per native producer, not per character;
			// give the second fixture command the next monotonic sequence.
			auto unleasedMove = MakeMoveRequest(unownedState, bindingGeneration, 3.0F, 2U, 6002U);
			const NativePhysicsStatus unleasedStatus = Native_Physics_SubmitCharacterCommand(&unleasedMove, &unleasedReceipt);
			context.Expect(leasedStatus == NativePhysicsStatus::Accepted
				&& leasedReceipt.commandId != 0U,
				"The bridge may queue a valid legacy command; ownership rejection occurs at the protected Physics boundary.");
			context.Expect(unleasedStatus == NativePhysicsStatus::Accepted
				&& unleasedReceipt.commandId != 0U,
				"The same native bridge remains usable for an unleased character (status="
					+ std::to_string(static_cast<std::uint32_t>(unleasedStatus)) + ", diagnostic="
					+ std::string(unleasedReceipt.diagnostic) + ").");
			if(leasedReceipt.commandId == 0U || unleasedReceipt.commandId == 0U) return;

			auto& physics = PhysicsSystem::Instance();
			(void)objects.PublishSceneChangePacket();
			physics.Update(physics.FixedTimeStep());
			const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects,
				"locomotion-navigation-interop-command-flush");
			physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			const auto results = physics.LatestCommandResults();
			bool leasedRejected = false;
			bool unleasedApplied = false;
			if(results && results->results) for(const auto& result : *results->results) {
				if(result.commandId == leasedReceipt.commandId)
					leasedRejected = result.status == PhysicsResultStatus::Conflict;
				if(result.commandId == unleasedReceipt.commandId)
					unleasedApplied = result.status == PhysicsResultStatus::Success;
			}
			context.Expect(leasedRejected,
				"The native ScriptGlue command must be rejected when Locomotion owns the controller.");
			context.Expect(unleasedApplied,
				"The unleased command should still reach the provider and apply successfully.");

			packet = physics.LatestCharacterStatePacket();
			const PhysicsCharacterState* ownedAfter = packet ? FindCharacter(*packet, runtime.ownedId) : nullptr;
			const PhysicsCharacterState* unownedAfter = packet ? FindCharacter(*packet, runtime.unownedId) : nullptr;
			context.Expect(ownedAfter && std::fabs(ownedAfter->pose.position.x - ownedState.pose.position.x) < 0.01F,
				"A rejected managed-follower-style command must not contribute physical movement.");
			context.Expect(unownedAfter && unownedAfter->pose.position.x > unownedState.pose.position.x + 0.01F,
				"The accepted unleased bridge command must produce measurable physical movement.");
		});
	}
}
