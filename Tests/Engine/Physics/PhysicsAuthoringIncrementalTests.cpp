#include "../TestHarness.h"

#include "Physics/Assets/PhysicsAssetChangeJournal.h"
#include "Physics/PhysicsAuthoringSnapshot.h"

#include <algorithm>
#include <cstdint>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Physics;

		TerraEngine::UUID Id(std::uint32_t value) {
			TerraEngine::UUID id{};
			id[12] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
			id[13] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
			id[14] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
			id[15] = static_cast<std::uint8_t>(value & 0xFFU);
			return id;
		}

		PhysicsBodyDescriptor Body(std::uint32_t id, std::uint64_t transformRevision = 1U) {
			PhysicsBodyDescriptor body;
			body.objectId = Id(id);
			body.objectGeneration = 1U;
			body.componentGeneration = 1U;
			// Immutable authoring packets always carry both an exact canonical pose
			// and the frame that owns its localized provider representation.
			body.canonicalWorldPose.position.offsetMetres = {static_cast<double>(id), 0.0, 0.0};
			body.frameMembership = {{kPrimaryPhysicsFrameId, 1U}, {{1U}, 1U}, 1U};
			body.sourceTransformRevision = transformRevision;
			return body;
		}

		PhysicsColliderDescriptor Collider(
			std::uint32_t objectId,
			std::uint32_t recordId,
			std::uint64_t recipeHash,
			TerraEngine::UUID colliderAsset = TerraEngine::EMPTY_UUID,
			TerraEngine::UUID materialAsset = TerraEngine::EMPTY_UUID) {
			PhysicsColliderDescriptor collider;
			collider.objectId = Id(objectId);
			collider.componentGeneration = 1U;
			collider.recordId = Id(recordId);
			collider.recordGeneration = 1U;
			collider.dimensions = {1.0F, 1.0F, 1.0F};
			collider.stableRecipeHash = recipeHash;
			collider.cookedColliderAssetId = colliderAsset;
			collider.materialId = materialAsset;
			return collider;
		}

		PhysicsAuthoringSnapshot Snapshot(
			PhysicsGeneration generation,
			std::vector<PhysicsBodyDescriptor> bodies,
			std::vector<PhysicsColliderDescriptor> colliders,
			std::uint64_t hierarchyGeneration = 1U,
			const PhysicsAuthoringSnapshot* previous = nullptr) {
			PhysicsAuthoringData data;
			data.world = {{1U}, 1U};
			data.settings = PhysicsSettings::Defaults();
			data.snapshotGeneration = generation;
			data.hierarchyGeneration = hierarchyGeneration;
			data.bodies = std::move(bodies);
			data.colliders = std::move(colliders);
			auto snapshot = PhysicsAuthoringSnapshot::Create(std::move(data), previous);
			if(!snapshot) throw TestFailure("The incremental authoring fixture must create a valid snapshot.");
			return std::move(*snapshot);
		}
	}

	void RegisterPhysicsAuthoringIncrementalTests(TestRegistry& registry) {
		registry.Add("Physics.AuthoringIncremental.AssetJournalPublishesExplicitBoundedGenerations", [](TestContext& context) {
			PhysicsAssetChangeJournal journal(2U);
			const TerraEngine::UUID collider = Id(100U);
			const TerraEngine::UUID material = Id(101U);
			const PhysicsAssetGeneration first = journal.Publish(
				collider, PhysicsAssetKind::CookedCollider, PhysicsAssetChangeKind::Added);
			const PhysicsAssetGeneration second = journal.Publish(
				material, PhysicsAssetKind::Material, PhysicsAssetChangeKind::Added);
			const PhysicsAssetGeneration third = journal.Publish(
				collider, PhysicsAssetKind::CookedCollider, PhysicsAssetChangeKind::Updated);
			context.Expect(first < second && second < third,
				"Collider and material notifications must advance one explicit monotonic generation.");
			context.ExpectEqual(third, journal.AssetGeneration(collider, PhysicsAssetKind::CookedCollider),
				"Per-asset observations must identify the latest published collider generation.");

			const PhysicsAssetChangeSnapshot current = journal.SnapshotSince(first);
			context.Expect(current.IsValid() && !current.requiresFullRefresh
				&& current.changes.size() == 2U,
				"A consumer inside the retained window must receive a complete coalesced change set.");
			const PhysicsAssetChangeSnapshot stale = journal.SnapshotSince(0U);
			context.Expect(stale.IsValid() && stale.requiresFullRefresh,
				"A consumer older than the bounded journal must receive an explicit full-refresh reason.");
		});

		registry.Add("Physics.AuthoringIncremental.UnchangedDescriptorsRetainStablePresenceAndIndices", [](TestContext& context) {
			const auto first = Snapshot(1U,
				{Body(1U), Body(2U)},
				{Collider(1U, 11U, 0xA1U), Collider(2U, 12U, 0xA2U)});
			const auto second = Snapshot(2U,
				{Body(2U), Body(1U)},
				{Collider(2U, 12U, 0xA2U), Collider(1U, 11U, 0xA1U)},
				1U,
				&first);
			const auto generation = PhysicsAuthoringSnapshot::BuildGeneration(&first, second);
			context.Expect(generation && generation->Unchanged(),
				"Unchanged authored bodies must produce an empty exact-change generation.");
			context.ExpectEqual(static_cast<std::size_t>(2U), second.Data().bodyPresence.size(),
				"The snapshot must retain the compact body-presence set.");
			const auto* firstIndex = first.FindBodyDescriptorIndex(Id(1U));
			const auto* secondIndex = second.FindBodyDescriptorIndex(Id(1U));
			context.Expect(firstIndex != nullptr && secondIndex != nullptr
				&& firstIndex->stableIndex == secondIndex->stableIndex,
				"A retained body must keep its stable descriptor index when vector order changes.");
		});

		registry.Add("Physics.AuthoringIncremental.AssetDependenciesAffectOnlyExactBodiesAndBypassPrimitives", [](TestContext& context) {
			const TerraEngine::UUID cookedAsset = Id(200U);
			const TerraEngine::UUID materialAsset = Id(201U);
			const auto first = Snapshot(1U,
				{Body(1U), Body(2U)},
				{Collider(1U, 11U, 0xB1U, cookedAsset, materialAsset),
					Collider(2U, 12U, 0xB2U)});
			const auto second = Snapshot(2U,
				{Body(1U), Body(2U)},
				{Collider(1U, 11U, 0xB1U, cookedAsset, materialAsset),
					Collider(2U, 12U, 0xB2U)});

			PhysicsAssetChangeJournal journal;
			journal.Publish(cookedAsset, PhysicsAssetKind::CookedCollider, PhysicsAssetChangeKind::Updated);
			journal.Publish(materialAsset, PhysicsAssetKind::Material, PhysicsAssetChangeKind::Updated);
			const auto generation = PhysicsAuthoringSnapshot::BuildGeneration(
				&first, second, journal.SnapshotSince(0U));
			context.Expect(generation && generation->Data().changes.size() == 1U,
				"Only the body indexed by the changed assets may enter preparation.");
			const PhysicsAuthoringChange* changed = generation ? generation->Find(Id(1U)) : nullptr;
			context.Expect(changed != nullptr
				&& HasAnyReason(changed->reasons, PhysicsAuthoringChangeReason::ColliderAsset)
				&& HasAnyReason(changed->reasons, PhysicsAuthoringChangeReason::MaterialAsset),
				"The exact body must retain both collider-asset and material-asset reasons.");
			context.Expect(generation && generation->Find(Id(2U)) == nullptr,
				"A primitive without a derived collider dependency must bypass unrelated asset work.");
		});

		registry.Add("Physics.AuthoringIncremental.EquivalentPrimitiveGeometrySharesWithoutBehaviorAliasing", [](TestContext& context) {
			PhysicsColliderDescriptor firstCollider = Collider(1U, 11U, 0xCAFEU);
			PhysicsColliderDescriptor secondCollider = Collider(2U, 12U, 0xCAFEU);
			firstCollider.materialId = Id(301U);
			secondCollider.materialId = Id(302U);
			firstCollider.collisionLayer = 1U;
			secondCollider.collisionLayer = 2U;
			const auto snapshot = Snapshot(1U,
				{Body(1U), Body(2U)}, {firstCollider, secondCollider});
			context.ExpectEqual(static_cast<std::size_t>(1U), snapshot.Data().primitiveShapeShares.size(),
				"Equivalent immutable primitive geometry must produce one share range.");
			context.ExpectEqual(2U, snapshot.Data().primitiveShapeShares.front().colliderCount,
				"The share range must identify both equivalent primitive collider records.");
			context.Expect(snapshot.Data().colliders[0].materialId != snapshot.Data().colliders[1].materialId
				&& snapshot.Data().colliders[0].collisionLayer != snapshot.Data().colliders[1].collisionLayer,
				"Geometry sharing must leave material and collision behavior on each collider instance.");
		});

		registry.Add("Physics.AuthoringIncremental.ReasonsSeparateAddsRemovalsTransformsAndDependencies", [](TestContext& context) {
			const auto first = Snapshot(1U, {Body(1U)}, {Collider(1U, 11U, 0xD1U)}, 1U);
			const auto second = Snapshot(2U,
				{Body(1U, 2U), Body(2U)},
				{Collider(1U, 11U, 0xD1U), Collider(2U, 12U, 0xD2U)}, 2U);
			const auto changed = PhysicsAuthoringSnapshot::BuildGeneration(&first, second);
			const PhysicsAuthoringChange* moved = changed ? changed->Find(Id(1U)) : nullptr;
			const PhysicsAuthoringChange* added = changed ? changed->Find(Id(2U)) : nullptr;
			context.Expect(moved != nullptr
				&& HasAnyReason(moved->reasons, PhysicsAuthoringChangeReason::Transform)
				&& HasAnyReason(moved->reasons, PhysicsAuthoringChangeReason::HierarchyDependency),
				"A hierarchy-derived pose change must retain both transform and dependency reasons.");
			context.Expect(added != nullptr
				&& HasAnyReason(added->reasons, PhysicsAuthoringChangeReason::Added)
				&& HasAnyReason(added->reasons, PhysicsAuthoringChangeReason::Lifecycle),
				"A new body must retain explicit added and lifecycle reasons.");

			const auto third = Snapshot(3U, {Body(2U)}, {Collider(2U, 12U, 0xD2U)}, 2U);
			const auto removedGeneration = PhysicsAuthoringSnapshot::BuildGeneration(&second, third);
			const PhysicsAuthoringChange* removed = removedGeneration
				? removedGeneration->Find(Id(1U))
				: nullptr;
			context.Expect(removed != nullptr
				&& HasAnyReason(removed->reasons, PhysicsAuthoringChangeReason::Removed)
				&& removed->descriptorIndex == kInvalidPhysicsDescriptorIndex,
				"A removed body must retain its exact identity without a stale live descriptor index.");
		});

		registry.Add("Physics.AuthoringIncremental.UnrelatedSnapshotMetadataDoesNotInvalidateRetainedBodies", [](TestContext& context) {
			const auto first = Snapshot(1U, {Body(1U)}, {Collider(1U, 11U, 0xE1U)}, 1U);
			const auto unrelatedChange = Snapshot(2U, {Body(1U)}, {Collider(1U, 11U, 0xE1U)}, 2U, &first);
			const auto generation = PhysicsAuthoringSnapshot::BuildGeneration(&first, unrelatedChange);
			context.Expect(generation && generation->Unchanged(),
				"A hierarchy publication without a changed physics dependency must retain the prior immutable descriptors.");
			context.Expect(first.FindBodyDescriptorIndex(Id(1U))->stableIndex
				== unrelatedChange.FindBodyDescriptorIndex(Id(1U))->stableIndex,
				"Unrelated scene metadata must not disturb a retained body's stable descriptor identity.");
		});

		registry.Add("Physics.AuthoringIncremental.PairedEndpointsPublishOneConstraintAcrossLifecycle", [](TestContext& context) {
			// A real constraint depends on two bodies. Adding, moving, or removing
			// both in one authored generation must retain one runtime closure record.
			PhysicsAuthoringData data;
			data.world = {{1U}, 1U};
			data.settings = PhysicsSettings::Defaults();
			data.snapshotGeneration = 1U;
			data.bodies = {Body(1U), Body(2U)};
			data.colliders = {Collider(1U, 11U, 0xF1U), Collider(2U, 12U, 0xF2U)};
			PhysicsConstraintDescriptor constraint;
			constraint.ownerObjectId = Id(1U);
			constraint.ownerObjectGeneration = 1U;
			constraint.componentGeneration = 1U;
			constraint.recordId = Id(91U);
			constraint.recordGeneration = 1U;
			constraint.firstObjectId = Id(1U);
			constraint.secondEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
			constraint.secondObjectId = Id(2U);
			data.constraints = {constraint};
			const auto first = PhysicsAuthoringSnapshot::Create(std::move(data));
			context.Expect(first.has_value(), "Paired endpoints must form a valid immutable snapshot.");
			const auto verify = [&](const PhysicsAuthoringSnapshot* previous, const PhysicsAuthoringSnapshot& current, bool removed) {
				const auto generation = PhysicsAuthoringSnapshot::BuildGeneration(previous, current);
				context.Expect(generation.has_value(), "Both changed endpoints must publish an atomically validated generation.");
				context.Expect(generation->Data().constraintRecordIds == std::vector<UUID>{constraint.recordId}
					&& generation->Data().changes.size() == 2U,
					"The constraint union must contain one record while both endpoint changes remain visible.");
				for(const auto& change : generation->Data().changes) {
					context.Expect(HasAnyReason(change.reasons, removed ? PhysicsAuthoringChangeReason::Removed
						: previous ? PhysicsAuthoringChangeReason::Transform : PhysicsAuthoringChangeReason::Added),
						"Closure normalization must preserve endpoint lifecycle and transform reasons.");
				}
			};
			verify(nullptr, *first, false);
			PhysicsAuthoringData movedData = first->Data();
			movedData.snapshotGeneration = 2U;
			for(auto& body : movedData.bodies) ++body.sourceTransformRevision;
			const auto moved = PhysicsAuthoringSnapshot::Create(std::move(movedData), &*first);
			context.Expect(moved.has_value(), "Moving both endpoints must preserve snapshot validity.");
			verify(&*first, *moved, false);
			PhysicsAuthoringData emptyData;
			emptyData.world = first->Data().world;
			emptyData.settings = first->Data().settings;
			emptyData.snapshotGeneration = 3U;
			const auto empty = PhysicsAuthoringSnapshot::Create(std::move(emptyData), &*moved);
			context.Expect(empty.has_value(), "Removing the paired mechanism must produce a valid empty snapshot.");
			verify(&*moved, *empty, true);
		});

		registry.Add("Physics.AuthoringIncremental.TypedClosureIncludesOnlyAffectedConstraintAndCharacterOwners", [](TestContext& context) {
			PhysicsAuthoringData firstData;
			firstData.world = {{1U}, 1U};
			firstData.settings = PhysicsSettings::Defaults();
			firstData.snapshotGeneration = 1U;
			firstData.bodies = {Body(1U), Body(2U)};
			firstData.colliders = {Collider(1U, 11U, 0xF1U), Collider(2U, 12U, 0xF2U)};
			PhysicsConstraintDescriptor constraint;
			constraint.ownerObjectId = Id(1U);
			constraint.ownerObjectGeneration = 1U;
			constraint.componentGeneration = 1U;
			constraint.recordId = Id(91U);
			constraint.recordGeneration = 1U;
			constraint.firstObjectId = Id(1U);
			constraint.secondEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
			constraint.secondObjectId = Id(2U);
			PhysicsConstraintDescriptor gear = constraint;
			gear.recordId = Id(92U);
			gear.kind = PhysicsConstraintKind::Gear;
			gear.firstGearConstraintId = constraint.recordId;
			gear.secondGearConstraintId = constraint.recordId;
			firstData.constraints = {constraint, gear};
			PhysicsCharacterDescriptor character;
			character.objectId = Id(2U);
			character.objectGeneration = 1U;
			character.componentGeneration = 1U;
			// A detached character snapshot requires complete transform/hierarchy
			// evidence even when this test only changes the constraint closure.
			character.objectTransformRevision = 1U;
			character.hierarchyGeneration = 1U;
			firstData.characters = {character};
			auto first = PhysicsAuthoringSnapshot::Create(std::move(firstData));
			context.Expect(first.has_value(), "The closure fixture must create its first immutable snapshot.");

			PhysicsAuthoringData secondData = first->Data();
			secondData.snapshotGeneration = 2U;
			secondData.bodies[0].sourceTransformRevision = 2U;
			auto second = PhysicsAuthoringSnapshot::Create(std::move(secondData), &*first);
			const auto generation = second ? PhysicsAuthoringSnapshot::BuildGeneration(&*first, *second) : std::nullopt;
			context.Expect(generation && generation->Data().constraintRecordIds.size() == 2U
				&& generation->Data().constraintRecordIds[0] == Id(91U)
				&& generation->Data().constraintRecordIds[1] == Id(92U)
				&& generation->Data().characterObjectIds.empty(),
				"Changing one hinge endpoint must include its direct and dependent gear closures without rebuilding an unrelated character.");
		});

		registry.Add("Physics.AuthoringIncremental.RejectsDiscontinuousPublishedSceneGeneration", [](TestContext& context) {
			PhysicsAuthoringGenerationData discontinuous;
			discontinuous.world = {{9U}, 1U};
			discontinuous.generation = 3U;
			discontinuous.snapshotGeneration = 3U;
			discontinuous.sceneGeneration = 7U;
			discontinuous.previousScenePublicationRevision = 40U;
			discontinuous.scenePublicationRevision = 42U;
			discontinuous.settingsHash = 1U;
			context.Expect(!PhysicsAuthoringGeneration::Create(std::move(discontinuous)).has_value(),
				"A skipped scene packet revision must fail closed rather than silently authoring from an incomplete delta.");

			PhysicsAuthoringGenerationData fallback;
			fallback.world = {{9U}, 1U};
			fallback.generation = 3U;
			fallback.snapshotGeneration = 3U;
			fallback.settingsHash = 1U;
			fallback.fallbackReason = PhysicsAuthoringFallbackReason::ScenePacketDiscontinuity;
			context.Expect(PhysicsAuthoringGeneration::Create(std::move(fallback)).has_value(),
				"A reason-tagged full fallback must remain valid when packet continuity cannot be proved.");
		});
	}
}
