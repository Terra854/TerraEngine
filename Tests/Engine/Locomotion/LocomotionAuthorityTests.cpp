// LocomotionAuthorityTests.cpp - command ownership and protected publication.

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
#include "ThreadPool/ThreadPool.h"

#include <cmath>
#include <span>
#include <sstream>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;

		LocomotionHandle MakeIdentity() {
			LocomotionHandle identity{};
			identity.sceneLifetime = 11U;
			identity.objectId[0] = 3U;
			identity.objectGeneration = 2U;
			identity.attachmentGeneration = 4U;
			return identity;
		}

		ResolvedLocomotionBindings MakeBindings() {
			ResolvedLocomotionBindings bindings{};
			bindings.configurationRevision = 1U;
			bindings.maximumSpeed = 5.0;
			bindings.controller.world = {{9U}, 1U};
			bindings.controller.character = {{9U}, 1U, 1U, 1U};
			bindings.controller.objectId = MakeIdentity().objectId;
			bindings.controller.objectGeneration = 2U;
			bindings.controller.componentGeneration = 4U;
			bindings.controllerSupported = true;
			bindings.frame = {Physics::kPrimaryPhysicsFrameId, 1U};
			return bindings;
		}

		LocomotionCommand MakeCommand(CommandKind kind, LocomotionHandle handle,
			std::uint64_t producer, std::uint64_t sequence, std::uint64_t epoch,
			std::uint64_t token = 0U) {
			LocomotionCommand command{};
			command.kind = kind;
			command.handle = handle;
			command.expectedAuthorityEpoch = epoch;
			command.expectedConfigurationRevision = 1U;
			command.ownerToken = token;
			command.producerId = producer;
			command.sequence = sequence;
			command.correlationId = sequence + 1000U;
			return command;
		}

		CommandReceipt SubmitAndCommit(LocomotionCommandStore& store,
			const LocomotionCommand& command, std::uint64_t updateEpoch) {
			(void)store.SubmitBatch(std::span(&command, 1U), updateEpoch);
			auto prepared = store.PrepareNextBatch(updateEpoch, {});
			if(prepared) {
				if(prepared->accepted) (void)store.CommitPreparedBatch(std::move(*prepared));
				else (void)store.RejectPreparedBatch(std::move(*prepared), prepared->reason);
			}
			return store.ReadReceipt(command.handle, command.producerId,
				command.sequence, command.correlationId);
		}

		// This fixture exercises the real protected system/Physics boundary rather
		// than treating the command store's internal commit as gameplay publication.
		class RuntimeGuard final {
		public:
			RuntimeGuard() {
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

			~RuntimeGuard() {
				LocomotionSystem::Instance().Shutdown();
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Start(TestContext& context) {
				auto& objects = ObjectManager::instance();
				const Object object = objects.createObject("Locomotion authority character");
				context.Expect(objects.AddComponent<Transform>(object) != nullptr
					&& objects.AddComponent<CharacterControllerComponent>(object) != nullptr
					&& objects.AddComponent<CharacterLocomotionComponent>(object) != nullptr,
					"The integration fixture needs all three authored components.");
				objectId = objects.getUUID(object);
				auto& physics = Physics::PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The integration fixture must start Physics.");
				LocomotionSystem::Instance().Initialize();
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-authority-physics-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
				LocomotionSystem::Instance().ObserveProtectedBoundary();
				return LocomotionSystem::Instance().ResolvePublished(objectId).has_value();
			}

			UUID objectId = EMPTY_UUID;
		};
	}

	void RegisterLocomotionAuthorityTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Authority.AuthoredInitialModeIsIdlePreference", [](TestContext& context) {
			LocomotionCommandStore store;
			auto bindings = MakeBindings();
			bindings.initialMotionMode = MotionMode::RootMotion;
			const auto handle = store.Bind(MakeIdentity(), bindings).handle;
			context.Expect(store.Read(handle)->authority.state == AuthorityState::Idle
				&& store.Read(handle)->authority.mode == MotionMode::RootMotion,
				"The authored mode is inspectable in Idle without granting RootMotion ownership.");
			const auto acquired = SubmitAndCommit(store,
				MakeCommand(CommandKind::Acquire, handle, 10U, 1U, 0U), 1U);
			context.Expect(acquired.state == ReceiptState::Applied
				&& store.Read(handle)->authority.mode == MotionMode::Velocity,
				"Acquire explicitly selects Velocity even when the authored idle preference is RootMotion.");
		});

		registry.Add("Engine.Locomotion.Authority.CaptureCopiesEligibleWithoutDraining", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(), MakeBindings()).handle;
			const auto acquire = MakeCommand(CommandKind::Acquire, handle, 10U, 1U, 0U);
			context.Expect(store.SubmitBatch(std::span(&acquire, 1U), 3U).queued,
				"A command can be admitted for a later movement epoch.");
			context.Expect(store.SnapshotEligibleCommands(2U).empty()
				&& store.SnapshotEligibleCommands(3U).size() == 1U
				&& store.PendingCount() == 1U,
				"Capture copies only eligible commands and does not consume Publish's queue.");
		});

		registry.Add("Engine.Locomotion.Authority.ExclusiveTransferAndRootModeRollback", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(), MakeBindings()).handle;
			const auto acquired = SubmitAndCommit(store,
				MakeCommand(CommandKind::Acquire, handle, 10U, 1U, 0U), 1U);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"Only protected Acquire grants the first producer an opaque token.");
			const auto competing = SubmitAndCommit(store,
				MakeCommand(CommandKind::Acquire, handle, 20U, 1U, 1U), 2U);
			context.Expect(competing.state == ReceiptState::Rejected
				&& store.Read(handle)->authority.ownerToken == acquired.ownerToken,
				"A second producer cannot implicitly take control.");
			auto unavailableMode = MakeCommand(CommandKind::SetMode, handle,
				10U, 2U, 1U, acquired.ownerToken);
			unavailableMode.mode = MotionMode::RootMotion;
			const auto failedMode = SubmitAndCommit(store, unavailableMode, 2U);
			context.Expect(failedMode.state == ReceiptState::Rejected
				&& failedMode.reason == LocomotionReason::AnimationUnavailable
				&& store.Read(handle)->authority.mode == MotionMode::Velocity
				&& store.Read(handle)->authority.authorityEpoch == 1U,
				"Unavailable RootMotion leaves the previous mode and epoch intact.");
			auto transfer = MakeCommand(CommandKind::Transfer, handle,
				10U, 3U, 1U, acquired.ownerToken);
			transfer.transferProducerId = 20U;
			const auto transferred = SubmitAndCommit(store, transfer, 2U);
			context.Expect(transferred.state == ReceiptState::Applied
				&& transferred.ownerToken != acquired.ownerToken
				&& transferred.authorityEpoch == 2U,
				"Explicit transfer rotates the token and advances authority epoch.");
			auto stale = MakeCommand(CommandKind::SetIntent, handle,
				10U, 4U, 2U, acquired.ownerToken);
			stale.direction = {1.0, 0.0, 0.0};
			stale.requestedSpeed = 2.0;
			context.Expect(SubmitAndCommit(store, stale, 3U).state == ReceiptState::Rejected
				&& store.Read(handle)->authority.ownerToken == transferred.ownerToken,
				"The old owner cannot replay intent after transfer.");
		});

		registry.Add("Engine.Locomotion.Authority.IntentExpiryStopAndResume", [](TestContext& context) {
			LocomotionCommandStore store;
			const auto handle = store.Bind(MakeIdentity(), MakeBindings()).handle;
			const auto acquired = SubmitAndCommit(store,
				MakeCommand(CommandKind::Acquire, handle, 10U, 1U, 0U), 1U);
			auto invalidPersistentPlayer = MakeCommand(CommandKind::SetIntent, handle,
				10U, 6U, 1U, acquired.ownerToken);
			invalidPersistentPlayer.direction = {1.0, 0.0, 0.0};
			invalidPersistentPlayer.requestedSpeed = 1.0;
			invalidPersistentPlayer.lifetime = IntentLifetime::UntilStopped;
			const auto invalidAdmission = store.SubmitBatch(
				std::span(&invalidPersistentPlayer, 1U), 2U);
			context.Expect(!invalidAdmission.queued
				&& invalidAdmission.reason == LocomotionReason::InvalidArgument,
				"Player intent cannot claim Script's persistent UntilStopped lifetime.");
			auto oneUpdate = MakeCommand(CommandKind::SetIntent, handle,
				10U, 2U, 1U, acquired.ownerToken);
			oneUpdate.direction = {1.0, 0.0, 0.0};
			oneUpdate.requestedSpeed = 8.0;
			const auto adjusted = SubmitAndCommit(store, oneUpdate, 2U);
			context.Expect(adjusted.state == ReceiptState::Adjusted
				&& store.Read(handle)->intent->requestedSpeed == 5.0,
				"Over-speed intent is clamped with an Adjusted receipt.");
			store.ExpireOneUpdateIntents(3U);
			context.Expect(!store.Read(handle)->intent.has_value(),
				"OneUpdate intent expires before the following movement epoch.");
			auto script = MakeCommand(CommandKind::SetIntent, handle,
				10U, 3U, 1U, acquired.ownerToken);
			script.intentKind = IntentKind::Script;
			script.direction = {0.0, 0.0, 1.0};
			script.requestedSpeed = 2.0;
			script.lifetime = IntentLifetime::UntilStopped;
			context.Expect(SubmitAndCommit(store, script, 3U).state == ReceiptState::Applied,
				"Script intent can request persistence.");
			store.ExpireOneUpdateIntents(9U);
			context.Expect(store.Read(handle)->intent.has_value(),
				"Persistent script intent survives later movement epochs.");
			const auto stopped = SubmitAndCommit(store,
				MakeCommand(CommandKind::Stop, handle, 10U, 4U, 1U, acquired.ownerToken), 9U);
			context.Expect(stopped.state == ReceiptState::Applied
				&& store.Read(handle)->authority.state == AuthorityState::Stopped
				&& !store.Read(handle)->intent.has_value(),
				"Stop clears persistent motion but retains the owner.");
			auto resume = MakeCommand(CommandKind::SetIntent, handle,
				10U, 5U, 2U, acquired.ownerToken);
			resume.direction = {0.0, 0.0, 1.0};
			resume.requestedSpeed = 1.0;
			context.Expect(SubmitAndCommit(store, resume, 10U).state == ReceiptState::Applied
				&& store.Read(handle)->authority.state == AuthorityState::Controlled,
				"The same owner resumes explicitly after Stop.");
		});

		registry.Add("Engine.Locomotion.Authority.SystemPublishesQueuedAcquireAndSameUpdateIntent", [](TestContext& context) {
			RuntimeGuard runtime;
			if(!runtime.Start(context)) {
				context.Expect(false, "The real controller must publish a locomotion handle.");
				return;
			}
			auto& system = LocomotionSystem::Instance();
			const auto handle = system.ResolvePublished(runtime.objectId);
			context.Expect(handle.has_value(), "The system must resolve the published component.");
			if(!handle) return;
			const auto acquire = MakeCommand(CommandKind::Acquire, *handle, 51U, 1U, 0U);
			const auto queued = system.SubmitBatch(std::span(&acquire, 1U));
			context.Expect(queued.queued && queued.receipts[0].state == ReceiptState::Queued,
				"Producer admission is queued, not Applied before protected publication.");
			system.Update(1.0F / 60.0F);
			const auto applied = system.ReadReceipt(*handle, 51U, 1U, 1001U);
			context.Expect(applied.state == ReceiptState::Applied && applied.ownerToken != 0U,
				"The next protected Update applies Acquire and returns a native token.");
			const auto packet = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
			context.Expect(packet && packet->states && !packet->states->empty(),
				"The integration fixture must expose a real physical controller.");
			if(!packet || !packet->states || packet->states->empty()) return;
			Physics::PhysicsCharacterCommandTarget controller{};
			for(const auto& state : *packet->states) {
				if(state.objectId != runtime.objectId) continue;
				controller.world = state.world;
				controller.character = state.handle;
				controller.objectId = state.objectId;
				controller.objectGeneration = state.objectGeneration;
				controller.componentGeneration = state.componentGeneration;
				break;
			}
			const auto lease = Physics::PhysicsSystem::Instance().ReadCharacterMotionLease(controller);
			context.Expect(lease && lease->state == Physics::CharacterMotionLeaseState::LocomotionOwned,
				"Applied logical Acquire must have its matching physical lease.");
			auto intent = MakeCommand(CommandKind::SetIntent, *handle,
				51U, 2U, applied.authorityEpoch, applied.ownerToken);
			intent.direction = {1.0, 0.0, 0.0};
			intent.requestedSpeed = 2.0;
			context.Expect(system.SubmitBatch(std::span(&intent, 1U)).queued,
				"The new owner's movement intent must queue.");
			system.Update(1.0F / 60.0F);
			const auto intentReceipt = system.ReadReceipt(*handle, 51U, 2U, 1002U);
			const auto observations = system.CopyObservations();
			bool sawMoving = false;
			std::ostringstream detail;
			const auto diagnostics = system.ReadDiagnostics();
			detail << "Same-update intent diagnostic: receiptState=" << static_cast<int>(intentReceipt.state)
				<< ", receiptReason=" << static_cast<int>(intentReceipt.reason)
				<< ", observationCount=" << (observations ? observations->entries.size() : 0U)
				<< ", rejectedTargets=" << diagnostics.rejectedTargets
				<< ", lastTargetFailure=" << diagnostics.lastTargetFailure;
			if(observations) for(const auto& observation : observations->entries)
				if(observation.handle == *handle) {
					detail << ", desiredX=" << observation.desiredVelocity.x
						<< ", bindingReason=" << static_cast<int>(observation.bindingReason)
						<< ", authorityState=" << static_cast<int>(observation.authority.state);
					if(observation.desiredVelocity.x > 1.9) sawMoving = true;
				}
			context.Expect(intentReceipt.state == ReceiptState::Applied && sawMoving, detail.str());
		});
	}
}
