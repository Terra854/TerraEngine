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
	}
}
