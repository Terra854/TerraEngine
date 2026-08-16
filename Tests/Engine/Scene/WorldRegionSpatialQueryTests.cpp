// WorldRegionSpatialQueryTests.cpp - sparse cross-region traversal coverage.

#include "../TestHarness.h"

#include "Scene/SpatialIndex.h"

namespace TerraEngine::Tests
{
	void RegisterWorldRegionSpatialQueryTests(TestRegistry& registry)
	{
		registry.Add("WorldRegionSpatialQuery.VisitsOnlyRequestedRegionsAndDeduplicatesProxy", [](TestContext& context) {
			TerraEngine::Scene::PublishedWorldRegionResidency residency{};
			auto first = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>(); first->key = { 1, 0, 0 };
			TerraEngine::UUID objectId{}; objectId[0] = 9;
			TerraEngine::Scene::SpatialObjectRecord record{}; record.objectId = objectId; record.localBounds = TerraEngine::Scene::MakeAABB(glm::vec3(-1.0f), glm::vec3(1.0f), TerraEngine::Scene::SpatialBoundsSource::Authored);
			first->spatialRecords.push_back(record);
			residency.residentRegions[first->key] = first;
			auto unrequested = std::make_shared<TerraEngine::Scene::WorldRegionSnapshot>(); unrequested->key = { 2, 0, 0 }; residency.residentRegions[unrequested->key] = unrequested;
			residency.coarseProxies.push_back({ objectId, first->key, {}, record.localBounds, 0 });
			const auto result = TerraEngine::Scene::QueryResidentRegionsAABB(residency, { first->key }, TerraEngine::Scene::MakeAABB(glm::vec3(-2.0f), glm::vec3(2.0f), TerraEngine::Scene::SpatialBoundsSource::Authored));
			context.ExpectEqual(1u, result.visitedRegionCount, "Cross-region traversal must inspect only explicit intersected resident keys, never the full directory.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.result.hits.size(), "Owner/cover/proxy duplicates must merge by stable UUID.");
		});
	}
}
