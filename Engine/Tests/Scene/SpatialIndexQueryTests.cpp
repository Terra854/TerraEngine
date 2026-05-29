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

		TerraEngine::Scene::SpatialObjectRecord MakeRecord(uint8_t idValue, const glm::vec3& minimum, const glm::vec3& maximum, uint64_t revision = 1) {
			TerraEngine::Scene::SpatialObjectRecord record{};
			record.objectId = MakeUuid(idValue);
			record.entity = static_cast<entt::entity>(idValue);
			record.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;
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

	void RegisterSpatialIndexQueryTests(TestRegistry& registry) {
		registry.Add("Engine.SpatialIndex.QueryAABBAndSphereReturnExpectedObjects", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {10.0f, 0.0f, 0.0f}, {12.0f, 2.0f, 2.0f}),
				MakeRecord(3, {0.0f, 10.0f, 0.0f}, {2.0f, 12.0f, 2.0f})
			};

			index.Rebuild(records);

			const TerraEngine::Scene::SpatialQueryResult boxResult = index.QueryAABB(TerraEngine::Scene::MakeAABB({-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}, TerraEngine::Scene::SpatialBoundsSource::Authored));
			context.ExpectEqual(1u, static_cast<unsigned int>(boxResult.hits.size()), "Only the origin object should overlap the AABB query.");
			context.Expect(HasHit(boxResult, MakeUuid(1)), "The AABB query should include the object at the origin.");
			context.Expect(!HasHit(boxResult, MakeUuid(2)), "The AABB query should not include the distant x-axis object.");

			const TerraEngine::Scene::SpatialSphere sphere{{11.0f, 1.0f, 1.0f}, 1.5f};
			const TerraEngine::Scene::SpatialQueryResult sphereResult = index.QuerySphere(sphere);
			context.ExpectEqual(1u, static_cast<unsigned int>(sphereResult.hits.size()), "Only the x-axis object should overlap the sphere query.");
			context.Expect(HasHit(sphereResult, MakeUuid(2)), "The sphere query should include the x-axis object.");
		});

		registry.Add("Engine.SpatialIndex.QueryRaySortsByDistanceAndHonorsMaxResults", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}),
				MakeRecord(2, {10.0f, -1.0f, -1.0f}, {12.0f, 1.0f, 1.0f}),
				MakeRecord(3, {0.0f, 5.0f, 0.0f}, {1.0f, 6.0f, 1.0f})
			};

			index.Rebuild(records);

			TerraEngine::Scene::SpatialQueryFilter filter{};
			filter.sortMode = TerraEngine::Scene::SpatialSortMode::DistanceAscending;
			filter.maxResults = 2;

			const TerraEngine::Scene::SpatialRay ray{{-5.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, 30.0f};
			const TerraEngine::Scene::SpatialQueryResult result = index.QueryRay(ray, filter);

			context.ExpectEqual(2u, static_cast<unsigned int>(result.hits.size()), "The ray query should return the capped number of hits.");
			context.Expect(result.hits[0].objectId == MakeUuid(1), "The closest ray hit should be reported first.");
			context.Expect(result.hits[1].objectId == MakeUuid(2), "The farther ray hit should be reported second.");
		});
	}
}
