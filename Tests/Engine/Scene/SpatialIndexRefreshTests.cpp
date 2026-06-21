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

		TerraEngine::Scene::SpatialObjectRecord MakeRecord(uint8_t idValue, const glm::vec3& minimum, const glm::vec3& maximum, uint64_t revision = 1) {
			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = MakeUuid(idValue);
			record.entity = static_cast<entt::entity>(idValue);
			record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
			record.worldBounds = TerraEngine::Scene::MakeAABB(minimum, maximum, TerraEngine::Scene::SpatialBoundsSource::Authored);
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
				refreshed[static_cast<std::size_t>(item)].worldBounds.min.x += 0.1f;
				refreshed[static_cast<std::size_t>(item)].worldBounds.max.x += 0.1f;
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
	}
}
