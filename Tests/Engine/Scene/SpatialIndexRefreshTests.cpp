#include "TestHarness.h"

#include "Scene/SpatialIndex.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeUuid(uint8_t value) {
			TerraEngine::UUID id{};
			id.fill(value);
			return id;
		}

		TerraEngine::Scene::SpatialObjectRecord MakeRecord(uint8_t idValue, const glm::vec3& minimum, const glm::vec3& maximum, uint64_t revision = 1, bool isStatic = false) {
			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = MakeUuid(idValue);
			record.entity = static_cast<entt::entity>(idValue);
			record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
			record.isStatic = isStatic;
			record.localBounds = TerraEngine::Scene::MakeAABB(minimum, maximum, TerraEngine::Scene::SpatialBoundsSource::Authored);
			record.revision = revision;
			return record;
		}
	}

	void RegisterSpatialIndexRefreshTests(TestRegistry& registry) {
		registry.Add("Engine.SpatialIndex.RefreshReportsUnchangedCollections", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {4.0f, -1.0f, -1.0f}, {6.0f, 1.0f, 1.0f})
			};

			index.Rebuild(records);
			index.Refresh(records);

			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Unchanged, "Refreshing identical records should report an unchanged refresh.");
			context.ExpectEqual(2u, diagnostics.unchangedObjectCount, "Both records should be counted as unchanged.");
			context.ExpectEqual(2u, diagnostics.indexedObjectCount, "The index should continue tracking both records.");
		});

		registry.Add("Engine.SpatialIndex.RefreshTracksIncrementalAddsUpdatesAndRemoves", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> initialRecords{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {8.0f, -1.0f, -1.0f}, {10.0f, 1.0f, 1.0f})
			};
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> refreshedRecords{
				MakeRecord(1, {0.0f, -1.0f, -1.0f}, {2.0f, 1.0f, 1.0f}, 2),
				MakeRecord(3, {4.0f, -1.0f, -1.0f}, {5.0f, 1.0f, 1.0f})
			};

			index.Rebuild(initialRecords);
			index.Refresh(refreshedRecords);

			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Incremental, "A contained add/update/remove refresh should stay incremental.");
			context.ExpectEqual(1u, diagnostics.addedObjectCount, "One object should be added during the incremental refresh.");
			context.ExpectEqual(1u, diagnostics.updatedObjectCount, "One object should be updated during the incremental refresh.");
			context.ExpectEqual(1u, diagnostics.removedObjectCount, "One object should be removed during the incremental refresh.");

			const TerraEngine::Scene::SpatialRay ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 20.0f};
			const TerraEngine::Scene::SpatialQueryResult result = index.QueryRay(ray);
			context.ExpectEqual(2u, static_cast<unsigned int>(result.hits.size()), "The refreshed index should expose the updated and added objects.");
		});

		registry.Add("Engine.SpatialIndex.RefreshFallsBackWhenDirtyRecordLeavesRootBounds", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> initialRecords{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f})
			};
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> movedRecords{
				MakeRecord(1, {100.0f, -1.0f, -1.0f}, {102.0f, 1.0f, 1.0f}, 2)
			};

			index.Rebuild(initialRecords);
			index.MarkDirty(MakeUuid(1));
			index.Refresh(movedRecords);

			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Full, "Moving outside root loose bounds should request a safe full refresh.");
			context.ExpectEqual(1u, diagnostics.fullRefreshCount, "Full refresh diagnostics should identify the fallback refresh.");
		});

		registry.Add("Engine.SpatialIndex.RefreshHandlesTenThousandEntriesWithOnePercentDirty", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			std::vector<TerraEngine::Scene::SpatialObjectRecord> records;
			records.reserve(10000);
			for (int item = 0; item < 10000; ++item)
			{
				const float x = static_cast<float>(item % 100);
				const float z = static_cast<float>(item / 100);
				records.push_back(MakeRecord(static_cast<uint8_t>((item % 250) + 1), {x, 0.0f, z}, {x + 0.25f, 0.25f, z + 0.25f}));
				records.back().objectId[0] = static_cast<std::uint8_t>(item / 250);
				records.back().entity = static_cast<entt::entity>(item + 1);
			}
			std::vector<TerraEngine::Scene::SpatialObjectRecord> refreshed = records;
			for (int item = 0; item < 100; ++item)
			{
				refreshed[static_cast<std::size_t>(item)].localBounds.min.x += 0.1f;
				refreshed[static_cast<std::size_t>(item)].localBounds.max.x += 0.1f;
				refreshed[static_cast<std::size_t>(item)].revision = 2;
				index.MarkDirty(refreshed[static_cast<std::size_t>(item)].objectId);
			}

			index.Rebuild(records);
			index.Refresh(refreshed);

			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Incremental, "Contained 1% dirty movement should stay incremental.");
			context.ExpectEqual(100u, diagnostics.updatedObjectCount, "Exactly the dirty 1% should be updated.");
			context.ExpectEqual(9900u, diagnostics.unchangedObjectCount, "The mostly static scene should keep unchanged entries out of update work.");
		});

		registry.Add("Engine.SpatialIndex.BoundsBatchUpdatesOnlyTheAffectedPartitionDeterministically", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-2.0f, -1.0f, -1.0f}, {0.0f, 1.0f, 1.0f}, 1, true),
				MakeRecord(2, {2.0f, -1.0f, -1.0f}, {4.0f, 1.0f, 1.0f}, 1, false),
				MakeRecord(3, {6.0f, -1.0f, -1.0f}, {8.0f, 1.0f, 1.0f}, 1, false)
			};
			index.Rebuild(records);
			const TerraEngine::Scene::SpatialPartitionRevisionSnapshot before = index.GetPartitionRevisionSnapshot();

			TerraEngine::Scene::SpatialBoundsPublicationBatch batch;
			batch.sceneGeneration = 7;
			batch.publicationGeneration = 11;
			batch.changes.push_back({TerraEngine::Scene::SpatialBoundsChangeKind::Update, MakeRecord(3, {7.0f, -1.0f, -1.0f}, {9.0f, 1.0f, 1.0f}, 2, false)});
			batch.changes.push_back({TerraEngine::Scene::SpatialBoundsChangeKind::Update, MakeRecord(2, {3.0f, -1.0f, -1.0f}, {5.0f, 1.0f, 1.0f}, 2, false)});

			context.Expect(index.ApplyBoundsPublicationBatch(batch), "A generation-valid movable changed-bounds batch should commit at the protected boundary.");
			const TerraEngine::Scene::SpatialPartitionRevisionSnapshot after = index.GetPartitionRevisionSnapshot();
			const std::vector<TerraEngine::UUID> order = index.GetLastBoundsPublicationOrder();
			context.ExpectEqual(before.staticRevision, after.staticRevision, "Movable changes must preserve the unrelated static partition cache.");
			context.Expect(after.movableRevision > before.movableRevision, "Movable changed bounds should advance only the movable partition.");
			context.ExpectEqual(static_cast<std::size_t>(2), order.size(), "The exact batch should retain both changed identities.");
			context.Expect(order[0] == MakeUuid(2) && order[1] == MakeUuid(3), "Publication order should be canonical by stable object identity regardless of input order.");
			context.ExpectEqual(3u, static_cast<unsigned int>(index.QueryRay({{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 20.0f}).hits.size()), "The existing query API should merge static and movable partition hits.");
		});

		registry.Add("Engine.SpatialIndex.BoundsBatchClassificationChangeTouchesBothPartitionsAndRejectsAtomically", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			index.Rebuild({
				MakeRecord(1, {-2.0f, -1.0f, -1.0f}, {0.0f, 1.0f, 1.0f}, 1, true),
				MakeRecord(2, {2.0f, -1.0f, -1.0f}, {4.0f, 1.0f, 1.0f}, 1, false)
			});
			const TerraEngine::Scene::SpatialPartitionRevisionSnapshot beforeClassChange = index.GetPartitionRevisionSnapshot();
			TerraEngine::Scene::SpatialBoundsPublicationBatch classChange;
			classChange.sceneGeneration = 4;
			classChange.publicationGeneration = 8;
			classChange.changes.push_back({TerraEngine::Scene::SpatialBoundsChangeKind::Update, MakeRecord(2, {3.0f, -1.0f, -1.0f}, {5.0f, 1.0f, 1.0f}, 2, true)});
			context.Expect(index.ApplyBoundsPublicationBatch(classChange), "A movable-to-static classification change should commit as one publication.");
			const TerraEngine::Scene::SpatialPartitionRevisionSnapshot afterClassChange = index.GetPartitionRevisionSnapshot();
			context.Expect(afterClassChange.staticRevision > beforeClassChange.staticRevision, "Classification change should update the destination static partition.");
			context.Expect(afterClassChange.movableRevision > beforeClassChange.movableRevision, "Classification change should update the source movable partition.");

			TerraEngine::Scene::SpatialBoundsPublicationBatch duplicate;
			duplicate.sceneGeneration = 4;
			duplicate.publicationGeneration = 9;
			duplicate.changes.push_back({TerraEngine::Scene::SpatialBoundsChangeKind::Update, MakeRecord(1, glm::vec3{-1.0f}, glm::vec3{1.0f}, 2, true)});
			duplicate.changes.push_back({TerraEngine::Scene::SpatialBoundsChangeKind::Remove, MakeRecord(1, glm::vec3{-1.0f}, glm::vec3{1.0f}, 2, true)});
			context.Expect(!index.ApplyBoundsPublicationBatch(duplicate), "Duplicate targets should reject the entire changed-bounds batch.");
			const TerraEngine::Scene::SpatialPartitionRevisionSnapshot afterReject = index.GetPartitionRevisionSnapshot();
			context.ExpectEqual(afterClassChange.staticRevision, afterReject.staticRevision, "Rejected work must leave the static partition untouched.");
			context.ExpectEqual(afterClassChange.movableRevision, afterReject.movableRevision, "Rejected work must leave the movable partition untouched.");
		});
	}
}
