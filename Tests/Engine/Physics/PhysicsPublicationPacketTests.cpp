// PhysicsPublicationPacketTests.cpp - changed-only conversion and atomic visibility.

#include "../TestHarness.h"

#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/PhysicsTransactions.h"
#include "Physics/TransformPublicationPacket.h"
#include "Scene/SceneMutationQueue.h"

#include <cmath>
#include <span>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		Physics::PhysicsPose MakePose(float x, float y, float z)
		{
			return {{x, y, z}, {0.0F, 0.0F, 0.0F, 1.0F}};
		}

		Physics::TransformPublicationSourceRecord CapturePublicationRecord(
			ObjectManager& objects,
			const Object& object,
			Physics::PhysicsWorldHandle world,
			Physics::PhysicsSlotIndex slot,
			Physics::PhysicsGeneration bodyGeneration,
			Physics::PhysicsPose pose,
			bool changed)
		{
			const std::optional<Object> parent = objects.getParent(object);
			return Physics::TransformPublicationSourceRecord{
				.objectId = objects.getUUID(object),
				.body = {world.worldId, world.worldGeneration, slot, bodyGeneration},
				.frame = {Physics::kPrimaryPhysicsFrameId, world.worldGeneration},
				.objectLifetimeGeneration = objects.getObjectLifetimeGeneration(object),
				.bodyGeneration = bodyGeneration,
				.componentGeneration = objects.getObjectComponentGeneration(object),
				.expectedTransformRevision = objects.GetObjectTransformRevision(object),
				.expectedHierarchyGeneration = objects.GetHierarchyRevision(),
				.parentObjectId = parent ? objects.getUUID(*parent) : TerraEngine::EMPTY_UUID,
				.expectedParentLifetimeGeneration = parent ? objects.getObjectLifetimeGeneration(*parent) : 0U,
				.expectedParentTransformRevision = parent ? objects.GetObjectTransformRevision(*parent) : 0U,
				// Every provider float pose is explicitly relative to this canonical
				// origin; tests must not imply a global float position authority.
				.canonicalFrameOrigin = {},
				.localPose = pose,
				.worldPoseReceipt = pose,
				.poseChanged = changed};
		}

		Physics::TransformPublicationPacket MakePacket(
			ObjectManager& objects,
			Physics::PhysicsWorldHandle world,
			std::span<const Physics::TransformPublicationSourceRecord> records,
			std::size_t chunkSize,
			std::uint64_t groupId)
		{
			return Physics::PrepareTransformPublicationPacket(
				{world, {Physics::kPrimaryPhysicsFrameId, world.worldGeneration}, 42U, 7U, 11U},
				objects.GetSceneGeneration(),
				objects.GetHierarchyRevision(),
				groupId,
				groupId + 100U,
				records,
				chunkSize,
				Physics::TransformPublicationVisibilityBoundary::AfterPhysics);
		}

		Physics::TransformPublicationSourceRecord CaptureCharacterPublicationRecord(
			ObjectManager& objects,
			const Object& object,
			Physics::PhysicsWorldHandle world,
			Physics::PhysicsSlotIndex slot,
			Physics::PhysicsGeneration characterGeneration,
			Physics::PhysicsPose pose)
		{
			Physics::TransformPublicationSourceRecord record = CapturePublicationRecord(
				objects, object, world, slot, characterGeneration, pose, true);
			// A controller-only object must carry its own provider authority rather
			// than fabricating a rigid body merely to update its visible Transform.
			record.authority = Physics::TransformPublicationAuthority::Character;
			record.body = {};
			record.character = {world.worldId, world.worldGeneration, slot, characterGeneration};
			return record;
		}
	}

	void RegisterPhysicsPublicationPacketTests(TestRegistry& registry)
	{
		registry.Add("Physics.Publication.RigidPosesPreserveAuthoredScale", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue queue;
			const Physics::PhysicsWorldHandle world{{95U}, 1U};
			const Object body = objects.createObject("Scaled Rigid Body");
			const Object character = objects.createObject("Small Imported Character");
			Scene::WorldPose bodyPose;
			bodyPose.scale = {4.0F, 2.0F, 10.0F};
			Scene::WorldPose characterPose;
			characterPose.scale = {0.01F, 0.02F, 0.03F};
			context.Expect(objects.SetCanonicalWorldPose(body, bodyPose) == Scene::WorldCoordinateError::None
				&& objects.SetCanonicalWorldPose(character, characterPose) == Scene::WorldCoordinateError::None,
				"The fixture must author distinct non-unit scales before physics publication.");
			for(std::uint64_t step = 1U; step <= 2U; ++step) {
				const std::vector<Physics::TransformPublicationSourceRecord> sources{
					CapturePublicationRecord(objects, body, world, 0U, 1U, MakePose(static_cast<float>(step), 0, 0), true),
					CaptureCharacterPublicationRecord(objects, character, world, 1U, 1U, MakePose(static_cast<float>(step), 0, 0))};
				queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(MakePacket(objects, world, sources, 1U, step)));
				context.Expect(objects.GetCanonicalWorldPose(character)->position.offsetMetres.x == static_cast<double>(step - 1U),
					"Preparing a scaled character packet must not publish before the declared flush.");
				const auto flushed = queue.Apply(objects, "physics-scaled-pose");
				context.Expect(flushed.publicationGroupResults.size() == 1U && flushed.publicationGroupResults.front().Committed(),
					"Body and character pose chunks must commit atomically at the protected boundary.");
				context.Expect(objects.GetCanonicalWorldPose(body)->scale == bodyPose.scale
					&& objects.GetCanonicalWorldPose(character)->scale == characterPose.scale,
					"Initialization and repeated rigid-pose updates must preserve authored mesh scales.");
			}
			// A stale second target rejects the entire packet, preserving the first
			// target's last visible pose and scale instead of exposing a partial update.
			const std::vector<Physics::TransformPublicationSourceRecord> staleSources{
				CapturePublicationRecord(objects, body, world, 0U, 1U, MakePose(99, 0, 0), true),
				CaptureCharacterPublicationRecord(objects, character, world, 1U, 1U, MakePose(99, 0, 0))};
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(MakePacket(objects, world, staleSources, 1U, 3U)));
			objects.removeObject(character);
			const auto rejected = queue.Apply(objects, "physics-scaled-stale-pose");
			context.Expect(rejected.publicationGroupResults.size() == 1U && !rejected.publicationGroupResults.front().Committed()
				&& objects.GetCanonicalWorldPose(body)->position.offsetMetres.x == 2.0
				&& objects.GetCanonicalWorldPose(body)->scale == bodyPose.scale,
				"A rejected scaled packet must retain both the prior pose and authored scale atomically.");
			objects.clear();
		});

		registry.Add("Physics.Publication.WorkerConversionKeepsOnlyChangedOrRequestedRecords", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Physics::PhysicsWorldHandle world{{91U}, 3U};
			const Object first = objects.createObject("Changed");
			const Object second = objects.createObject("Retained Unchanged");
			const Object third = objects.createObject("Explicit Query State");
			std::vector<Physics::TransformPublicationSourceRecord> records;
			records.push_back(CapturePublicationRecord(objects, first, world, 0U, 5U, MakePose(1.0F, 0.0F, 0.0F), true));
			records.back().boundsChange = Physics::TransformPublicationBoundsChange{
				.previousBounds = Scene::MakeAABB({-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}, Scene::SpatialBoundsSource::Authored),
				.changedBounds = Scene::MakeAABB({0.5F, -0.5F, -0.5F}, {1.5F, 0.5F, 0.5F}, Scene::SpatialBoundsSource::Authored),
				.expectedSpatialRevision = objects.GetObjectSpatialRevision(first)};
			records.push_back(CapturePublicationRecord(objects, second, world, 1U, 5U, MakePose(2.0F, 0.0F, 0.0F), false));
			records.push_back(CapturePublicationRecord(objects, third, world, 2U, 5U, MakePose(3.0F, 0.0F, 0.0F), false));
			records.back().explicitlyRequired = true;

			const Physics::TransformPublicationPacket packet = MakePacket(objects, world, records, 1U, 1U);
			context.ExpectEqual(static_cast<std::size_t>(2), packet.entries.size(), "Unchanged retained snapshot records should not enter transform publication.");
			context.Expect(objects.getUUID(first) == packet.entries[0].objectId, "Changed records should retain deterministic source order.");
			context.Expect(objects.getUUID(third) == packet.entries[1].objectId, "Explicitly requested unchanged state should remain publishable.");
			context.ExpectEqual(static_cast<std::size_t>(2), packet.chunks.size(), "A chunk size of one should bound preparation without changing packet identity.");
			context.Expect(packet.snapshot.tick == 42U && packet.snapshot.storeGeneration == 7U && packet.snapshot.snapshotGeneration == 11U, "Changed publication should retain the complete same-tick snapshot identity.");
			context.Expect(packet.entries.front().boundsChange.has_value() && packet.entries.front().boundsChange->changedBounds.Center().x == 1.0F, "Changed bounds should travel with the typed pose entry for later spatial publication.");
			context.Expect(Physics::ValidateTransformPublicationPacket(packet).accepted, "Worker-converted packet should be structurally valid without a live Object facade.");
			objects.clear();
		});

		registry.Add("Physics.Publication.MultiChunkGroupBecomesVisibleAtOneDeclaredFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue queue;
			const Physics::PhysicsWorldHandle world{{92U}, 4U};
			std::vector<Object> targets{
				objects.createObject("Chunk A"),
				objects.createObject("Chunk B"),
				objects.createObject("Chunk C")};
			std::vector<Physics::TransformPublicationSourceRecord> records;
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = world;
			for (std::size_t index = 0U; index < targets.size(); ++index)
			{
				const auto bodyGeneration = static_cast<Physics::PhysicsGeneration>(8U + index);
				records.push_back(CapturePublicationRecord(objects, targets[index], world, static_cast<Physics::PhysicsSlotIndex>(index), bodyGeneration, MakePose(static_cast<float>(index + 1U), 2.0F, 3.0F), true));
				runtime.bodyHandles.emplace(objects.getUUID(targets[index]), records.back().body);
				runtime.bodyGenerations.emplace(objects.getUUID(targets[index]), bodyGeneration);
			}
			const Physics::TransformPublicationPacket packet = MakePacket(objects, world, records, 2U, 2U);
			context.Expect(Physics::ValidatePhysicsTransformPublication(objects, runtime, packet).accepted, "Matching scene/body generations should pass combined preflight.");
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(packet));
			for (const Object& target : targets)
			{
				context.Expect(std::abs(static_cast<float>(objects.GetComponent<Transform>(target)->worldPose.position.offsetMetres.x)) < 0.001F, "Prepared worker output must remain invisible before the declared flush.");
			}

			const Scene::SceneMutationBatchResult flushed = queue.Apply(objects, "After Physics");
			context.ExpectEqual(static_cast<std::size_t>(1), flushed.publicationGroupResults.size(), "One tick should produce one group receipt regardless of preparation chunks.");
			context.Expect(flushed.publicationGroupResults.front().Committed(), "Every chunk should commit under one tick-wide decision.");
			context.ExpectEqual(static_cast<std::size_t>(2), flushed.publicationGroupResults.front().chunkReceipts.size(), "The accepted group should retain per-chunk receipts.");
			for (std::size_t index = 0U; index < targets.size(); ++index)
			{
				context.Expect(std::abs(static_cast<float>(objects.GetComponent<Transform>(targets[index])->worldPose.position.offsetMetres.x) - static_cast<float>(index + 1U)) < 0.001F, "All chunks should become visible together at the protected boundary.");
			}

			const Physics::TransformPublicationReceipt receipt = Physics::MakeTransformPublicationReceipt(packet, flushed.publicationGroupResults.front());
			context.Expect(receipt.Accepted(), "Only an accepted protected flush should create an acknowledgement receipt.");
			context.ExpectEqual(targets.size(), receipt.acknowledgedEntries.size(), "Acknowledgement should cover every changed pose in the committed tick.");
			objects.clear();
		});

		registry.Add("Physics.Publication.StaleLateChunkRetainsPriorVisiblePacketAtomically", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue queue;
			const Physics::PhysicsWorldHandle world{{93U}, 4U};
			const Object first = objects.createObject("Prior First");
			const Object second = objects.createObject("Prior Second");
			std::vector<Physics::TransformPublicationSourceRecord> firstRecords{
				CapturePublicationRecord(objects, first, world, 0U, 1U, MakePose(1.0F, 0.0F, 0.0F), true),
				CapturePublicationRecord(objects, second, world, 1U, 1U, MakePose(2.0F, 0.0F, 0.0F), true)};
			const Physics::TransformPublicationPacket accepted = MakePacket(objects, world, firstRecords, 2U, 31U);
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(accepted));
			const auto acceptedFlush = queue.Apply(objects, "physics-prior-packet");
			context.Expect(acceptedFlush.publicationGroupResults.size() == 1U
				&& acceptedFlush.publicationGroupResults.front().Committed(),
				"The prior packet must establish visible state before rejection is measured.");

			std::vector<Physics::TransformPublicationSourceRecord> replacement{
				CapturePublicationRecord(objects, first, world, 0U, 1U, MakePose(10.0F, 0.0F, 0.0F), true),
				CapturePublicationRecord(objects, second, world, 1U, 1U, MakePose(20.0F, 0.0F, 0.0F), true)};
			const Physics::TransformPublicationPacket rejected = MakePacket(objects, world, replacement, 1U, 32U);
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(rejected));
			objects.removeObject(second);
			const auto rejectedFlush = queue.Apply(objects, "physics-stale-late-chunk");
			const Transform* visibleFirst = objects.GetComponent<Transform>(first);
			context.Expect(rejectedFlush.publicationGroupResults.size() == 1U
				&& !rejectedFlush.publicationGroupResults.front().Committed()
				&& rejectedFlush.publicationGroupResults.front().appliedWriteCount == 0U,
				"A stale late publication chunk must reject the whole replacement packet before any write becomes visible.");
			context.Expect(visibleFirst != nullptr && std::abs(static_cast<float>(visibleFirst->worldPose.position.offsetMetres.x) - 1.0F) < 0.001F,
				"Rejected publication must retain the last complete visible pose rather than exposing a partial replacement.");
			objects.clear();
		});

		registry.Add("Physics.Publication.ControllerOnlyPoseUsesCharacterAuthorityAndProtectedFlush", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Scene::SceneMutationQueue queue;
			const Physics::PhysicsWorldHandle world{{94U}, 6U};
			const Object controller = objects.createObject("Controller Only Publication");
			const Physics::TransformPublicationSourceRecord source = CaptureCharacterPublicationRecord(
				objects, controller, world, 4U, 8U, MakePose(3.0F, 0.0F, 0.0F));
			const Physics::TransformPublicationPacket packet = MakePacket(objects, world, {&source, 1U}, 1U, 41U);
			Physics::PhysicsTransactionRuntimeState runtime;
			runtime.world = world;
			runtime.characterHandles.emplace(source.objectId, source.character);
			runtime.characterGenerations.emplace(source.objectId, source.character.slotGeneration);
			context.Expect(Physics::ValidatePhysicsTransformPublication(objects, runtime, packet).accepted,
				"A controller-only Transform packet must validate against its character handle and generation.");
			queue.EnqueuePublicationGroup(Physics::BuildTransformPublicationGroup(packet));
			context.Expect(std::abs(static_cast<float>(objects.GetComponent<Transform>(controller)->worldPose.position.offsetMetres.x)) < 0.001F,
				"A prepared controller pose must remain invisible before its declared flush boundary.");
			const Scene::SceneMutationBatchResult flushed = queue.Apply(objects, "After Physics");
			context.Expect(flushed.publicationGroupResults.size() == 1U
				&& flushed.publicationGroupResults.front().Committed()
				&& std::abs(static_cast<float>(objects.GetComponent<Transform>(controller)->worldPose.position.offsetMetres.x) - 3.0F) < 0.001F,
				"The protected flush must publish a controller-only pose atomically.");

			runtime.characterHandles[source.objectId].slotGeneration = 9U;
			const Physics::TransformPublicationValidationResult stale =
				Physics::ValidatePhysicsTransformPublication(objects, runtime, packet);
			context.Expect(!stale.accepted && stale.reason == Physics::TransformPublicationValidationReason::StaleBody
				&& std::abs(static_cast<float>(objects.GetComponent<Transform>(controller)->worldPose.position.offsetMetres.x) - 3.0F) < 0.001F,
				"A stale character generation must reject before mutation and retain the last complete visible controller pose.");
			objects.clear();
		});
	}
}
