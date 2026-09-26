// LocomotionTraceTests.cpp - bounded, copied Physics character-motion evidence.

#include "../TestHarness.h"

#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Locomotion/LocomotionCommands.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/CharacterMotionTrace.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Locomotion;
		using namespace TerraEngine::Physics;

		class FixedValidity final : public CharacterMotionValidity {
		public:
			explicit FixedValidity(CharacterMotionValidityReason value) : reason(value) {}
			CharacterMotionValidityReason Validate() const noexcept override { return reason; }
			CharacterMotionValidityReason reason;
		};

		class CharacterMotionTraceRuntime final {
		public:
			CharacterMotionTraceRuntime() {
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

			~CharacterMotionTraceRuntime() {
				auto& physics = PhysicsSystem::Instance();
				(void)physics.ConfigureCharacterMotionTrace(false);
				LocomotionSystem::Instance().Shutdown();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Start(TestContext& context) {
				auto& objects = ObjectManager::instance();
				for(std::size_t index = 0U; index < objects_.size(); ++index) {
					objects_[index] = objects.createObject("Motion trace character");
					uuids_[index] = objects.getUUID(objects_[index]);
					context.Expect(objects.AddComponent<Transform>(objects_[index]) != nullptr
						&& objects.AddComponent<CharacterControllerComponent>(objects_[index]) != nullptr
						&& objects.AddComponent<CharacterLocomotionComponent>(objects_[index]) != nullptr,
						"The trace fixture requires two real controller/locomotion attachments.");
				}
				auto& physics = PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The trace fixture must start a real Physics world.");
				LocomotionSystem::Instance().Initialize();
				PublishCharacters();
				LocomotionSystem::Instance().ObserveProtectedBoundary();
				return LocomotionSystem::Instance().ResolvePublished(uuids_[0]).has_value()
					&& LocomotionSystem::Instance().ResolvePublished(uuids_[1]).has_value();
			}

			void PublishCharacters() {
				auto& objects = ObjectManager::instance();
				auto& physics = PhysicsSystem::Instance();
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-trace-physics-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
			}

			std::array<Object, 2U> objects_{};
			std::array<UUID, 2U> uuids_{};
		};

		PhysicsCharacterCommandTarget FindControllerTarget(PhysicsSystem& physics, UUID objectId) {
			PhysicsCharacterCommandTarget target{};
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

		const CharacterMotionTraceRecord* FindTraceRecord(
			const CharacterMotionTraceSnapshot& snapshot, std::uint64_t sequence) {
			for(const auto& record : snapshot.records)
				if(record.sequence == sequence) return &record;
			return nullptr;
		}
	}

	void RegisterLocomotionTraceTests(TestRegistry& registry) {
		registry.Add("Engine.Locomotion.Stress.TerrainPublicationCopiesOnlyAcceptedWorldRegions",
			[](TestContext& context) {
				CharacterMotionTraceRuntime runtime;
				if(!runtime.Start(context)) return;
				auto& physics = PhysicsSystem::Instance();
				TerrainCollisionReplacementPacket packet;
				packet.id = "locomotion-terrain-publication-evidence";
				packet.sourceRootHash = {12U, 13U};
				packet.sourcePublicationGeneration = 3U;
				packet.recipe.terrainAssetId = GenerateUUID();
				packet.recipe.authoredGeneration = 3U;
				packet.recipe.authoredHash = 77U;
				packet.recipe.heightMillimetres.assign(TerrainHeightFieldRecipe::kSampleCount, 0);
				packet.recipe.holeBits.assign(TerrainHeightFieldRecipe::kHoleByteCount, 0U);
				context.Expect(physics.CopyTerrainCollisionPublications().empty(),
					"No terrain publication may be inferred from a scene object or uncommitted candidate.");
				const auto committed = physics.CommitTerrainCollisionReplacement(packet);
				const auto before = physics.CopyTerrainCollisionPublications();
				context.Expect(committed.committed && before.size() == 1U
					&& before.front().body == committed.activeBody && before.front().world.IsValid()
					&& before.front().sourceRootHash == packet.sourceRootHash
					&& before.front().sourcePublicationGeneration == 3U,
					"Copied terrain evidence must name the accepted provider body, world and exact source root.");
				packet.sourcePublicationGeneration = 4U;
				packet.cancelled = true;
				const auto rejected = physics.CommitTerrainCollisionReplacement(packet);
				const auto after = physics.CopyTerrainCollisionPublications();
				context.Expect(!rejected.committed && after.size() == 1U
					&& after.front().body == before.front().body && after.front().sourcePublicationGeneration == 3U,
					"Rejected terrain replacement must leave copied accepted-publication proof unchanged.");
				context.Expect(physics.RemoveTerrainCollisions(packet.recipe.terrainAssetId)
					&& physics.CopyTerrainCollisionPublications().empty() && before.size() == 1U,
					"Removal clears live publication identity without modifying earlier detached snapshots.");
			});
		registry.Add("Engine.Locomotion.Stress.TraceDefaultsDisabledAndBoundsPreserveCapture",
			[](TestContext& context) {
				auto& physics = PhysicsSystem::Instance();
				const auto defaults = physics.CopyCharacterMotionTrace(0U);
				context.Expect(!defaults.enabled && defaults.records.empty()
					&& defaults.reservedBytes == 0U,
					"Character-motion tracing must be disabled by default and reserve no ring storage.");
				context.Expect(!physics.ConfigureCharacterMotionTrace(true, 0U)
					&& !physics.ConfigureCharacterMotionTrace(true, 65537U),
					"Trace capacity zero and values above the 65536-record maximum must be rejected.");
				const auto unchanged = physics.CopyCharacterMotionTrace(0U);
				context.Expect(!unchanged.enabled && unchanged.records.empty()
					&& unchanged.reservedBytes == 0U,
					"Invalid enable bounds must preserve the disabled capture state.");
			});

		registry.Add("Engine.Locomotion.Stress.TraceCapturesAcceptedBatchTargetAndCursorLoss",
			[](TestContext& context) {
				CharacterMotionTraceRuntime runtime;
				if(!runtime.Start(context)) {
					context.Expect(false, "Two real Physics character identities must be published before tracing.");
					return;
				}
				auto& physics = PhysicsSystem::Instance();
				auto& locomotion = LocomotionSystem::Instance();
				constexpr std::size_t kRingCapacity = 4U;
				context.Expect(physics.ConfigureCharacterMotionTrace(true, kRingCapacity),
					"A bounded four-record capture must be enabled for the ingress test.");
				const auto preCapture = physics.CopyCharacterMotionTrace(0U);
				const std::uint64_t traceBase = preCapture.latestSequence;
				const auto initialStorage = physics.ReadCharacterMotionStorage();
				context.Expect(initialStorage.traceReservedBytes
					== kRingCapacity * sizeof(CharacterMotionTraceRecord),
					"Storage diagnostics must report the configured bounded ring reservation.");

				std::array<PhysicsCharacterCommandTarget, 2U> controllers{};
				std::array<CharacterMotionLeaseChange, 2U> leaseChanges{};
				for(std::size_t index = 0U; index < leaseChanges.size(); ++index) {
					const auto resolved = locomotion.ResolvePublishedSnapshot(runtime.uuids_[index]);
					context.Expect(resolved.has_value(), "Both fixture objects must have copied binding resolutions.");
					if(!resolved) return;
					controllers[index] = FindControllerTarget(physics, runtime.uuids_[index]);
					const auto currentLease = physics.ReadCharacterMotionLease(controllers[index]);
					context.Expect(currentLease.has_value()
						&& currentLease->state == CharacterMotionLeaseState::Unleased,
						"The real provider must supply an unowned lease before tracing the accepted change.");
					if(!currentLease) return;
					auto& change = leaseChanges[index];
					change.expected = *currentLease;
					change.next = *currentLease;
					change.next.owner.sceneLifetime = resolved->handle.sceneLifetime;
					change.next.owner.objectId = resolved->handle.objectId;
					change.next.owner.objectGeneration = resolved->handle.objectGeneration;
					change.next.owner.attachmentGeneration = resolved->handle.attachmentGeneration;
					change.next.owner.slotIndex = resolved->handle.slotIndex;
					change.next.owner.slotGeneration = resolved->handle.slotGeneration;
					change.next.state = CharacterMotionLeaseState::LocomotionOwned;
					change.next.mode = CharacterMotionMode::Velocity;
					++change.next.authorityEpoch;
					++change.next.motionEpoch;
					++change.next.publicationEpoch;
				}
			const auto leasePublished = physics.PublishCharacterMotionLeases(
				std::span<const CharacterMotionLeaseChange>(leaseChanges));
			context.Expect(leasePublished.accepted,
				"The real Physics system must atomically accept both generation-qualified lease changes.");

			const auto acquiredTrace = physics.CopyCharacterMotionTrace(traceBase);
			context.Expect(acquiredTrace.enabled && acquiredTrace.records.size() == 2U,
				"The accepted two-character lease publication must copy two records into the trace.");
			if(acquiredTrace.records.size() != 2U) return;
			const auto& firstLeaseRecord = acquiredTrace.records[0U];
			const auto& secondLeaseRecord = acquiredTrace.records[1U];
			context.Expect(firstLeaseRecord.sequence < secondLeaseRecord.sequence
				&& firstLeaseRecord.batch == secondLeaseRecord.batch
				&& firstLeaseRecord.batchIndex == 0U && secondLeaseRecord.batchIndex == 1U
				&& firstLeaseRecord.batchCount == 2U && secondLeaseRecord.batchCount == 2U
				&& firstLeaseRecord.precedingTick == secondLeaseRecord.precedingTick
				&& std::holds_alternative<CharacterMotionLeaseChange>(firstLeaseRecord.payload)
				&& std::holds_alternative<CharacterMotionLeaseChange>(secondLeaseRecord.payload),
				"Copied lease records must retain their ordered shared batch metadata and preceding Physics tick.");
			const auto* firstChange = std::get_if<CharacterMotionLeaseChange>(&firstLeaseRecord.payload);
			const auto* secondChange = std::get_if<CharacterMotionLeaseChange>(&secondLeaseRecord.payload);
			context.Expect(firstChange && secondChange
				&& firstChange->expected.state == CharacterMotionLeaseState::Unleased
				&& firstChange->next.state == CharacterMotionLeaseState::LocomotionOwned
				&& secondChange->expected.state == CharacterMotionLeaseState::Unleased
				&& secondChange->next.state == CharacterMotionLeaseState::LocomotionOwned
				&& firstChange->next.owner.IsValid() && secondChange->next.owner.IsValid(),
				"Lease payloads must contain copied before/after owner state, not merely a record kind.");

			const auto controller = controllers[0U];
			const auto lease = physics.ReadCharacterMotionLease(controller);
			context.Expect(controller.objectId == runtime.uuids_[0U] && lease.has_value()
				&& lease->state == CharacterMotionLeaseState::LocomotionOwned,
				"Target capture must use the current real controller and accepted exclusive lease.");
			if(!lease) return;
			CharacterMotionTarget target{};
			target.controller = controller;
			target.owner = lease->owner;
			target.authorityEpoch = lease->authorityEpoch;
			target.motionEpoch = lease->motionEpoch;
			target.locomotionUpdateEpoch = 2U;
			target.mode = lease->mode;
			target.frame = {kPrimaryPhysicsFrameId, controller.world.worldGeneration};
			target.planarVelocity = {1.0F, 0.0F, 0.0F};
			target.acceleration = 20.0F;
			target.braking = 25.0F;
			auto validity = std::make_shared<FixedValidity>(CharacterMotionValidityReason::Valid);
			std::weak_ptr<const CharacterMotionValidity> validityLifetime = validity;
			target.validityGuard = validity;
			const auto targetPublished = physics.PublishCharacterMotionTarget(target);
			context.Expect(targetPublished.Succeeded(),
				"A current target with a valid guard must be accepted by real protected Physics ingress.");
			context.Expect(physics.ReadCharacterMotionStorage().guardedTargets == 1U,
				"While active, the physical target may retain its guard for boundary-time validation.");
			validity.reset();
			target.validityGuard.reset();

			auto rejectedTarget = target;
			rejectedTarget.validityGuard = std::make_shared<FixedValidity>(
				CharacterMotionValidityReason::StaleEvidence);
			const auto beforeRejected = physics.CopyCharacterMotionTrace(traceBase);
			const auto rejected = physics.PublishCharacterMotionTarget(rejectedTarget);
			const auto afterRejected = physics.CopyCharacterMotionTrace(traceBase);
			context.Expect(!rejected.Succeeded()
				&& afterRejected.latestSequence == beforeRejected.latestSequence
				&& afterRejected.records.size() == beforeRejected.records.size(),
				"A stale validity guard must reject target ingress before capture, without advancing the trace sequence.");

			const auto afterTarget = physics.CopyCharacterMotionTrace(traceBase);
			context.Expect(afterTarget.records.size() == 3U
				&& afterTarget.records.back().sequence > secondLeaseRecord.sequence
				&& afterTarget.records.back().precedingTick >= secondLeaseRecord.precedingTick,
				"The accepted target must append after the lease batch at a nondecreasing Physics tick.");
			const CharacterMotionTraceRecord* targetRecord = afterTarget.records.empty()
				? nullptr : &afterTarget.records.back();
			const auto* copiedTarget = targetRecord
				? std::get_if<CharacterMotionTarget>(&targetRecord->payload) : nullptr;
			context.Expect(copiedTarget != nullptr && targetRecord->hadValidityGuard
				&& copiedTarget->validityGuard == nullptr
				&& copiedTarget->controller == controller
				&& copiedTarget->owner == lease->owner
				&& copiedTarget->planarVelocity.x == 1.0F,
				"The trace must record that a guard existed while stripping its shared lifetime from the copied target.");

			// Clearing the accepted physical target releases Physics' retained guard.
			// The trace copy is intentionally still readable after that release.
			auto clearTarget = target;
			clearTarget.clearImmediately = true;
			clearTarget.locomotionUpdateEpoch = 3U;
			clearTarget.planarVelocity = {};
			clearTarget.hasFacing = false;
			context.Expect(physics.PublishCharacterMotionTarget(clearTarget).Succeeded(),
				"An immediate clear target must be accepted for the current lease.");
			context.Expect(validityLifetime.expired()
				&& physics.ReadCharacterMotionStorage().guardedTargets == 0U,
				"A trace record must not keep the evidence validity object alive after Physics releases the target.");
			// One more accepted record than the ring capacity makes this a genuine
			// overwrite test even when a fresh Physics trace sequence begins at one.
			clearTarget.locomotionUpdateEpoch = 4U;
			context.Expect(physics.PublishCharacterMotionTarget(clearTarget).Succeeded(),
				"A later current-lease clear must add one record beyond the ring capacity.");
			const auto afterClear = physics.CopyCharacterMotionTrace(0U);
			context.Expect(afterClear.records.size() == kRingCapacity,
				"The ring must expose only its configured number of newest records.");
			const std::uint64_t cursorBeforeOldest = afterClear.records.empty()
				? 0U : afterClear.records.front().sequence - 1U;
			const auto cursorAtOne = physics.CopyCharacterMotionTrace(cursorBeforeOldest);
			context.Expect(afterClear.lostRecords && !cursorAtOne.lostRecords,
				"CopySince must report an overwritten cursor while still allowing a cursor immediately before the oldest retained sequence.");
			const auto* retainedTargetRecord = FindTraceRecord(afterClear, targetRecord ? targetRecord->sequence : 0U);
			const auto* retainedTarget = retainedTargetRecord
				? std::get_if<CharacterMotionTarget>(&retainedTargetRecord->payload) : nullptr;
			context.Expect(retainedTargetRecord && retainedTargetRecord->hadValidityGuard
				&& retainedTarget && retainedTarget->validityGuard == nullptr,
				"The ring must retain the copied target evidence without retaining its validity guard.");

			const std::uint64_t previousCursor = afterClear.latestSequence;
			context.Expect(!physics.ConfigureCharacterMotionTrace(true, 0U)
				&& !physics.ConfigureCharacterMotionTrace(true, 65537U),
				"Invalid capacity changes must fail without replacing a live bounded capture.");
			const auto preserved = physics.CopyCharacterMotionTrace(0U);
			context.Expect(preserved.enabled && preserved.latestSequence == previousCursor
				&& preserved.records.size() == afterClear.records.size(),
				"Rejected capacity bounds must preserve the current ring records, enabled state, and monotonic cursor.");

			context.Expect(physics.ConfigureCharacterMotionTrace(true, 2U),
				"A valid reconfiguration must atomically replace the ring capacity.");
			const auto reconfigured = physics.CopyCharacterMotionTrace(previousCursor);
			context.Expect(reconfigured.enabled && reconfigured.latestSequence == previousCursor
				&& reconfigured.records.empty()
				&& reconfigured.reservedBytes == 2U * sizeof(CharacterMotionTraceRecord),
				"Reconfiguration clears retained records and storage to the new capacity but never rewinds sequence numbers.");
			clearTarget.locomotionUpdateEpoch = 5U;
			context.Expect(physics.PublishCharacterMotionTarget(clearTarget).Succeeded(),
				"The same current lease must publish a target after trace reconfiguration.");
			const auto afterReconfigureRecord = physics.CopyCharacterMotionTrace(previousCursor);
			context.Expect(afterReconfigureRecord.records.size() == 1U
				&& afterReconfigureRecord.records.front().sequence == previousCursor + 1U,
				"New records must continue from the previous global sequence after reconfiguration.");
			const auto afterReconfigureLoss = physics.CopyCharacterMotionTrace(0U);
			context.Expect(afterReconfigureLoss.lostRecords,
				"A cursor from before ring reconfiguration must explicitly report that prior records were discarded.");
			context.Expect(physics.ConfigureCharacterMotionTrace(false),
				"The validation trace must be explicitly disableable after the bounded test.");
			const auto disabled = physics.CopyCharacterMotionTrace(previousCursor + 1U);
			context.Expect(!disabled.enabled && disabled.records.empty() && disabled.reservedBytes == 0U,
				"Disabling trace capture must release ring storage and return no records.");
			const auto storage = physics.ReadCharacterMotionStorage();
			context.Expect(storage.traceReservedBytes == 0U,
				"Physics storage diagnostics must report zero trace reservation after disable.");
			});
	}
}
