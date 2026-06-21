#include "TestHarness.h"

#include "Scene/SpatialIndex.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::UUID MakeUuid(uint8_t value) {
			TerraEngine::UUID id{};
			id.fill(value);
			return id;
		}

		TerraEngine::Scene::SpatialObjectRecord MakeRecord(
			uint8_t idValue,
			const glm::vec3& minimum,
			const glm::vec3& maximum,
			uint32_t categoryMask = TerraEngine::Scene::SpatialCategoryRenderable,
			bool active = true,
			uint64_t revision = 1) {
			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = MakeUuid(idValue);
			record.entity = static_cast<entt::entity>(idValue);
			record.active = active;
			record.categoryMask = categoryMask;
			record.worldBounds = TerraEngine::Scene::MakeAABB(minimum, maximum, TerraEngine::Scene::SpatialBoundsSource::Authored);
			record.revision = revision;
			return record;
		}

		bool HasHit(const TerraEngine::Scene::SpatialQueryResult& result, const TerraEngine::UUID& objectId) {
			return std::any_of(result.hits.begin(), result.hits.end(), [&objectId](const TerraEngine::Scene::SpatialQueryHit& hit) {
				return hit.objectId == objectId;
			});
		}
	}

	void RegisterSpatialIndexEdgeTests(TestRegistry& registry) {
		registry.Add("Engine.SpatialIndex.FiltersInactiveAndCategoryMasks", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, TerraEngine::Scene::SpatialCategoryRenderable, true),
				MakeRecord(2, {2.0f, -1.0f, -1.0f}, {3.0f, 1.0f, 1.0f}, TerraEngine::Scene::SpatialCategoryRenderable, false),
				MakeRecord(3, {4.0f, -1.0f, -1.0f}, {5.0f, 1.0f, 1.0f}, TerraEngine::Scene::SpatialCategoryDiagnostic, true)
			};
			index.Rebuild(records);

			const TerraEngine::Scene::SpatialAABB queryArea = TerraEngine::Scene::MakeAABB({-2.0f, -2.0f, -2.0f}, {6.0f, 2.0f, 2.0f}, TerraEngine::Scene::SpatialBoundsSource::Authored);
			const TerraEngine::Scene::SpatialQueryResult defaultResult = index.QueryAABB(queryArea);
			context.ExpectEqual(2u, static_cast<unsigned int>(defaultResult.hits.size()), "Default filters should include active objects from all categories.");
			context.Expect(HasHit(defaultResult, MakeUuid(1)) && HasHit(defaultResult, MakeUuid(3)), "Default filters should include active renderable and diagnostic objects.");
			context.Expect(!HasHit(defaultResult, MakeUuid(2)), "Default filters should exclude inactive objects.");

			TerraEngine::Scene::SpatialQueryFilter renderableOnly{};
			renderableOnly.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
			const TerraEngine::Scene::SpatialQueryResult renderableResult = index.QueryAABB(queryArea, renderableOnly);
			context.ExpectEqual(1u, static_cast<unsigned int>(renderableResult.hits.size()), "Renderable-only filters should exclude diagnostic and inactive objects.");
			context.Expect(HasHit(renderableResult, MakeUuid(1)), "Renderable-only filters should include the active renderable object.");

			renderableOnly.includeInactive = true;
			const TerraEngine::Scene::SpatialQueryResult inactiveIncludedResult = index.QueryAABB(queryArea, renderableOnly);
			context.ExpectEqual(2u, static_cast<unsigned int>(inactiveIncludedResult.hits.size()), "Renderable filters should include inactive renderables when requested.");
			context.Expect(HasHit(inactiveIncludedResult, MakeUuid(1)) && HasHit(inactiveIncludedResult, MakeUuid(2)), "Inactive-inclusive renderable filters should include both renderable objects.");
		});

		registry.Add("Engine.SpatialIndex.InvalidQueryInputsReportInvalidInput", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f})
			};
			index.Rebuild(records);

			const TerraEngine::Scene::SpatialQueryResult invalidAabb = index.QueryAABB(TerraEngine::Scene::MakeInvalidBounds(TerraEngine::Scene::SpatialBoundsSource::Authored));
			context.Expect(!invalidAabb.validInput, "Invalid AABB queries should be reported as invalid input.");
			context.Expect(invalidAabb.hits.empty(), "Invalid AABB queries should not return hits.");

			const TerraEngine::Scene::SpatialSphere invalidSphere{{0.0f, 0.0f, 0.0f}, -1.0f};
			const TerraEngine::Scene::SpatialQueryResult invalidSphereResult = index.QuerySphere(invalidSphere);
			context.Expect(!invalidSphereResult.validInput, "Negative-radius sphere queries should be reported as invalid input.");
			context.Expect(invalidSphereResult.hits.empty(), "Invalid sphere queries should not return hits.");

			const TerraEngine::Scene::SpatialRay zeroDirectionRay{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 10.0f};
			const TerraEngine::Scene::SpatialQueryResult invalidRayResult = index.QueryRay(zeroDirectionRay);
			context.Expect(!invalidRayResult.validInput, "Zero-direction ray queries should be reported as invalid input.");
			context.Expect(invalidRayResult.hits.empty(), "Invalid ray queries should not return hits.");
		});

		registry.Add("Engine.SpatialIndex.MarkDirtyForcesIncrementalUpdateOnRefresh", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {4.0f, -1.0f, -1.0f}, {6.0f, 1.0f, 1.0f})
			};
			index.Rebuild(records);

			index.MarkDirty(MakeUuid(1));
			TerraEngine::Scene::SpatialDiagnosticsSnapshot dirtyDiagnostics = index.GetDiagnosticsSnapshot();
			context.ExpectEqual(1u, dirtyDiagnostics.dirtyObjectCount, "MarkDirty should increment the dirty-object diagnostic count.");

			index.Refresh(records);
			const TerraEngine::Scene::SpatialDiagnosticsSnapshot refreshDiagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(refreshDiagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Incremental, "Refreshing a dirty but otherwise unchanged record should use the incremental path.");
			context.ExpectEqual(1u, refreshDiagnostics.updatedObjectCount, "The dirty record should be counted as an updated object.");
			context.ExpectEqual(1u, refreshDiagnostics.unchangedObjectCount, "The clean record should remain unchanged.");
			context.ExpectEqual(0u, refreshDiagnostics.dirtyObjectCount, "Refresh should clear dirty-object diagnostics after processing.");
		});

		registry.Add("Engine.SpatialIndex.ForcedRefreshReportsFullRefreshDiagnostics", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {4.0f, -1.0f, -1.0f}, {6.0f, 1.0f, 1.0f})
			};
			index.Rebuild(records);

			index.Refresh(records, true);
			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Full, "Forced refresh should route through the full rebuild path.");
			context.ExpectEqual(2u, diagnostics.fullRefreshObjectCount, "Full refresh diagnostics should report the number of input records.");
			context.ExpectEqual(2u, diagnostics.indexedObjectCount, "Full refresh should keep all records indexed.");
		});

		registry.Add("Engine.SpatialIndex.DiagnosticsExposeSummaryCountersWithoutPerObjectDetails", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {2.0f, -1.0f, -1.0f}, {3.0f, 1.0f, 1.0f})
			};
			index.Rebuild(records);
			index.Refresh(records);

			const TerraEngine::Scene::SpatialDiagnosticsSnapshot diagnostics = index.GetDiagnosticsSnapshot();
			context.Expect(diagnostics.refreshMode == TerraEngine::Scene::SpatialRefreshMode::Unchanged, "Diagnostics should report unchanged steady-state refreshes.");
			context.ExpectEqual(2u, diagnostics.unchangedObjectCount, "Summary diagnostics should count unchanged records.");
			context.ExpectEqual(0u, diagnostics.incrementalRefreshCount, "Unchanged refreshes should not be counted as incremental work.");
			context.ExpectEqual(0u, diagnostics.fullRefreshCount, "Unchanged refreshes should not be counted as full refresh work.");
		});
	}
}
