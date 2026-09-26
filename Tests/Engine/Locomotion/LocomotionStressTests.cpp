// LocomotionStressTests.cpp - bounded producer and lifecycle stress regressions.

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;

		constexpr std::size_t kStressSlotCount = 1000U;

		LocomotionHandle MakeStoreIdentity(std::size_t value) {
			LocomotionHandle identity{};
			identity.sceneLifetime = 83U;
			identity.objectId[0] = static_cast<std::uint8_t>(value & 0xFFU);
			identity.objectId[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
			identity.objectId[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
			identity.objectId[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
			identity.objectId[15] = 0xA5U;
			identity.objectGeneration = 1U;
			identity.attachmentGeneration = 1U;
			return identity;
		}

		ResolvedLocomotionBindings MakeStoreBindings(std::size_t value,
			const LocomotionHandle& identity) {
			ResolvedLocomotionBindings bindings{};
			bindings.configurationRevision = 1U;
			bindings.maximumSpeed = 5.0;
			bindings.controller.world = {{83U}, 1U};
			bindings.controller.character = {{83U}, 1U,
				static_cast<std::uint32_t>(value), 1U};
			bindings.controller.objectId = identity.objectId;
			bindings.controller.objectGeneration = identity.objectGeneration;
			bindings.controller.componentGeneration = identity.attachmentGeneration;
			bindings.frame = {Physics::kPrimaryPhysicsFrameId, 1U};
			bindings.controllerSupported = true;
			return bindings;
		}

		LocomotionCommand MakeAcquire(const LocomotionHandle& handle,
			std::uint64_t producer, std::uint64_t sequence = 1U) {
			LocomotionCommand command{};
			command.kind = CommandKind::Acquire;
			command.handle = handle;
			command.expectedConfigurationRevision = 1U;
			command.producerId = producer;
			command.sequence = sequence;
			command.correlationId = producer ^ (sequence << 32U);
			return command;
		}

		struct StoreEntry final {
			LocomotionHandle handle{};
			LocomotionCommand acquire{};
			CommandReceipt acquired{};
		};

		bool CommitAll(LocomotionCommandStore& store, std::uint64_t updateEpoch) {
			while(store.PendingCount() != 0U) {
				auto prepared = store.PrepareNextBatch(updateEpoch, {});
				if(!prepared || !prepared->accepted) return false;
				if(!store.CommitPreparedBatch(std::move(*prepared))) return false;
			}
			return true;
		}

		class LocomotionStressRuntime final {
		public:
			LocomotionStressRuntime() {
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

			~LocomotionStressRuntime() {
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
				object_ = objects.createObject("Locomotion stress character");
				context.Expect(objects.AddComponent<Transform>(object_) != nullptr
					&& objects.AddComponent<CharacterControllerComponent>(object_) != nullptr
					&& objects.AddComponent<CharacterLocomotionComponent>(object_) != nullptr,
					"The real-runtime stress fixture needs Transform, CharacterController, and CharacterLocomotion.");
				objectId_ = objects.getUUID(object_);
				auto& physics = Physics::PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The stress fixture must start a real Physics world.");
				LocomotionSystem::Instance().Initialize();
				PublishCharacters();
				LocomotionSystem::Instance().ObserveProtectedBoundary();
				return LocomotionSystem::Instance().ResolvePublished(objectId_).has_value();
			}

			void PublishCharacters() {
				auto& objects = ObjectManager::instance();
				auto& physics = Physics::PhysicsSystem::Instance();
				// Repeated extraction lets the provider publish a new controller generation
				// after each transactional component remove/add cycle.
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-stress-physics-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
			}

			Object object_{};
			UUID objectId_ = EMPTY_UUID;
		};

		Physics::PhysicsCharacterCommandTarget FindControllerTarget(
			Physics::PhysicsSystem& physics, UUID objectId) {
			Physics::PhysicsCharacterCommandTarget target{};
			const auto packet = physics.LatestCharacterStatePacket();
			if(!packet || !packet->states) return target;
			for(const auto& state : *packet->states) {
				if(state.objectId != objectId) continue;
				target.world = state.world;
				target.character = state.handle;
				target.objectId = state.objectId;
				target.objectGeneration = state.objectGeneration;
				target.componentGeneration = state.componentGeneration;
				break;
			}
			return target;
		}
	}

	void RegisterLocomotionStressTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Stress.ConcurrentAdmissionAcross1_4_8ProducersAnd1000Slots",
			[](TestContext& context) {
				constexpr std::array<std::size_t, 3U> workerCounts{1U, 4U, 8U};
				for(const std::size_t workerCount : workerCounts) {
					LocomotionCommandStore store;
					std::vector<StoreEntry> entries(kStressSlotCount);
					std::vector<CommandBatchAdmission> admissions(kStressSlotCount);
					bool allBound = true;
					for(std::size_t index = 0U; index < kStressSlotCount; ++index) {
						const auto identity = MakeStoreIdentity(index + 1U);
						const auto bound = store.Bind(identity, MakeStoreBindings(index + 1U, identity));
						allBound = allBound && bound.Succeeded();
						entries[index].handle = bound.handle;
						entries[index].acquire = MakeAcquire(bound.handle, 100000U + index);
					}
					context.Expect(allBound && store.LiveCount() == kStressSlotCount,
						"Each stress campaign must bind 1000 distinct valid identities under the store's 4096-slot bound.");
					if(!allBound) return;

					std::vector<std::thread> producers;
					producers.reserve(workerCount);
					for(std::size_t worker = 0U; worker < workerCount; ++worker) {
						producers.emplace_back([&, worker] {
							for(std::size_t index = worker; index < kStressSlotCount; index += workerCount) {
								admissions[index] = store.SubmitBatch(
									std::span(&entries[index].acquire, 1U), 1U);
							}
						});
					}
					for(auto& producer : producers) producer.join();

					std::size_t admittedCount = 0U;
					bool exactQueuedReceipts = true;
					for(const auto& admission : admissions) {
						admittedCount += admission.queued ? 1U : 0U;
						exactQueuedReceipts = exactQueuedReceipts && admission.queued
							&& admission.receipts.size() == 1U
							&& admission.receipts.front().state == ReceiptState::Queued;
					}
					context.Expect(admittedCount == kStressSlotCount && exactQueuedReceipts
						&& store.PendingCount() == kStressSlotCount,
						"Concurrent admission must fail closed with exactly one retained Queued receipt for each of 1000 inputs.");
					context.Expect(CommitAll(store, 1U),
						"All admitted stress commands must pass the ordinary protected store commit path.");

					bool exactAppliedReceipts = true;
					for(auto& entry : entries) {
						entry.acquired = store.ReadReceipt(entry.handle, entry.acquire.producerId,
							entry.acquire.sequence, entry.acquire.correlationId);
						exactAppliedReceipts = exactAppliedReceipts
							&& entry.acquired.state == ReceiptState::Applied
							&& entry.acquired.ownerToken != 0U;
						const auto retry = store.SubmitBatch(std::span(&entry.acquire, 1U), 2U);
						exactAppliedReceipts = exactAppliedReceipts && retry.queued
							&& retry.receipts.size() == 1U
							&& retry.receipts.front().state == ReceiptState::Applied
							&& retry.receipts.front().ownerToken == entry.acquired.ownerToken;
					}
					context.Expect(exactAppliedReceipts && store.PendingCount() == 0U,
						"Every producer must read its exact applied receipt, and exact retries must not enqueue duplicate work.");

					std::vector<CommandBatchAdmission> releases(kStressSlotCount);
					producers.clear();
					for(std::size_t worker = 0U; worker < workerCount; ++worker) {
						producers.emplace_back([&, worker] {
							for(std::size_t index = worker; index < kStressSlotCount; index += workerCount) {
								auto command = entries[index].acquire;
								command.kind = CommandKind::Release;
								command.sequence = 2U;
								command.correlationId += 1U;
								command.expectedAuthorityEpoch = entries[index].acquired.authorityEpoch;
								command.ownerToken = entries[index].acquired.ownerToken;
								releases[index] = store.SubmitBatch(std::span(&command, 1U), 2U);
							}
						});
					}
					for(auto& producer : producers) producer.join();
					std::size_t releaseCount = 0U;
					bool releaseReceiptsQueued = true;
					for(const auto& admission : releases) {
						releaseCount += admission.queued ? 1U : 0U;
						releaseReceiptsQueued = releaseReceiptsQueued && admission.queued
							&& admission.receipts.size() == 1U
							&& admission.receipts.front().state == ReceiptState::Queued;
					}
					context.Expect(releaseCount == kStressSlotCount && releaseReceiptsQueued,
						"Concurrent teardown admission must retain one Queued Release receipt for every live slot.");
					context.Expect(CommitAll(store, 2U),
						"Every admitted Release must commit through the same protected transaction path.");

					bool idleAfterRelease = true;
					for(auto& entry : entries) {
						const auto released = store.ReadReceipt(entry.handle, entry.acquire.producerId,
							2U, entry.acquire.correlationId + 1U);
						const auto record = store.Read(entry.handle);
						idleAfterRelease = idleAfterRelease && released.state == ReceiptState::Applied
							&& record && record->authority.state == AuthorityState::Idle
							&& record->authority.ownerToken == 0U && !record->intent.has_value();
					}
					context.Expect(idleAfterRelease,
						"After Release, no stress slot may retain an active owner token or movement intent.");

					bool slotsTornDown = true;
					for(const auto& entry : entries)
						slotsTornDown = store.ReleaseSlot(entry.handle) && slotsTornDown;
					context.Expect(slotsTornDown && store.LiveCount() == 0U && store.PendingCount() == 0U,
						"Releasing every attachment must leave no live or queued store state.");

					const auto replacementIdentity = MakeStoreIdentity(1U);
					const auto replacement = store.Bind(replacementIdentity,
						MakeStoreBindings(1U, replacementIdentity));
					const auto staleAdmission = store.SubmitBatch(
						std::span(&entries.front().acquire, 1U), 3U);
					context.Expect(replacement.Succeeded() && replacement.handle != entries.front().handle
						&& !staleAdmission.queued && staleAdmission.reason == LocomotionReason::StaleHandle,
						"Slot reuse must increment generation so an old typed command cannot address the replacement identity.");
				}
			});

		registry.Add("Engine.Locomotion.Stress.RepeatedPhysicsReleaseAndRebindRejectsOldGenerations",
			[](TestContext& context) {
				LocomotionStressRuntime runtime;
				if(!runtime.Start(context)) {
					context.Expect(false, "The live Physics fixture must publish its first locomotion identity.");
					return;
				}
				auto& objects = ObjectManager::instance();
				auto& locomotion = LocomotionSystem::Instance();
				auto& physics = Physics::PhysicsSystem::Instance();
				constexpr std::uint64_t kFirstProducer = 7000U;
				constexpr std::size_t kLifecycleCycles = 12U;
				bool everyCycleClean = true;
				for(std::size_t cycle = 0U; cycle < kLifecycleCycles; ++cycle) {
					const auto handle = locomotion.ResolvePublished(runtime.objectId_);
					if(!handle) {
						everyCycleClean = false;
						break;
					}
					const std::uint64_t producer = kFirstProducer + cycle;
					auto acquire = MakeAcquire(*handle, producer);
					const auto admission = locomotion.SubmitBatch(std::span(&acquire, 1U));
					locomotion.Update(physics.FixedTimeStep());
					const auto acquired = locomotion.ReadReceipt(*handle, producer, 1U,
						acquire.correlationId);
					if(!admission.queued || acquired.state != ReceiptState::Applied
						|| acquired.ownerToken == 0U) {
						everyCycleClean = false;
						break;
					}

					auto release = acquire;
					release.kind = CommandKind::Release;
					release.sequence = 2U;
					release.correlationId += 1U;
					release.expectedAuthorityEpoch = acquired.authorityEpoch;
					release.ownerToken = acquired.ownerToken;
					const auto releaseAdmission = locomotion.SubmitBatch(std::span(&release, 1U));
					locomotion.Update(physics.FixedTimeStep());
					const auto released = locomotion.ReadReceipt(*handle, producer, 2U,
						release.correlationId);
					const auto observations = locomotion.CopyObservations();
					bool clearedObservation = false;
					if(observations) for(const auto& observation : observations->entries)
						if(observation.handle == *handle) {
							clearedObservation = observation.authority.state == AuthorityState::Idle
								&& observation.authority.ownerToken == 0U
								&& observation.desiredVelocity.x == 0.0
								&& observation.desiredVelocity.y == 0.0
								&& observation.desiredVelocity.z == 0.0;
							break;
						}
					const auto target = FindControllerTarget(physics, runtime.objectId_);
					const auto lease = physics.ReadCharacterMotionLease(target);
					const bool targetCurrent = target.objectId == runtime.objectId_
						&& target.objectGeneration != 0U && target.componentGeneration != 0U
						&& target.character.slotGeneration != 0U;
					const bool noPhysicalOwner = targetCurrent && (!lease
						|| lease->state == Physics::CharacterMotionLeaseState::Unleased);
					everyCycleClean = everyCycleClean && releaseAdmission.queued
						&& released.state == ReceiptState::Applied
						&& clearedObservation
						&& noPhysicalOwner;
					if(!everyCycleClean) break;

					objects.RemoveComponent<CharacterLocomotionComponent>(runtime.object_);
					locomotion.ObserveProtectedBoundary();
					const auto stale = locomotion.SubmitBatch(std::span(&acquire, 1U));
					const bool removedCleanly = stale.reason == LocomotionReason::StaleHandle
						&& !stale.queued
						&& !locomotion.ResolvePublished(runtime.objectId_).has_value();
					context.Expect(removedCleanly,
						"After each detach, the prior generation must be stale and absent before a replacement can bind.");
					if(!removedCleanly) {
						everyCycleClean = false;
						break;
					}
					context.Expect(objects.AddComponent<CharacterLocomotionComponent>(runtime.object_) != nullptr,
						"Each lifecycle cycle must accept a fresh locomotion attachment.");
					runtime.PublishCharacters();
					locomotion.ObserveProtectedBoundary();
					const auto replacement = locomotion.ResolvePublished(runtime.objectId_);
					if(!replacement || *replacement == *handle) {
						everyCycleClean = false;
						break;
					}
				}

				context.Expect(everyCycleClean,
					"Twelve real Physics acquire/release and transactional attachment cycles must leave every prior generation stale and every released lease unowned.");
				context.Expect(locomotion.ReadDiagnostics().liveSlots == 1U,
					"Repeated teardown must retain only the final live replacement attachment.");
			});
	}
}
