#include "TestHarness.h"

#include "Scene/SpatialIndex.h"

#include <algorithm>
#include <cstdint>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
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
		registry.Add("Engine.SpatialIndex.SceneViewPerspectiveFrustumReturnsVisibleMarkerBounds", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			std::vector<TerraEngine::Scene::SpatialObjectRecord> records{
				MakeRecord(1, {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}),
				MakeRecord(2, {-0.5f, 2.5f, -4.5f}, {0.5f, 3.5f, -3.5f}),
				MakeRecord(3, {-0.5f, 4.5f, 11.0f}, {0.5f, 5.5f, 12.0f})
			};
			index.Rebuild(records);

			// Match the installed editor's default Scene View camera and Vulkan
			// zero-to-one depth projection. This catches a frustum extraction change
			// that would otherwise silently reject every marker in a real viewport.
			const glm::vec3 cameraPosition(0.0f, 5.0f, 10.0f);
			const glm::vec3 forward(0.0f, -0.3428978f, -0.9393727f);
			const glm::mat4 view = glm::lookAt(cameraPosition, cameraPosition + forward, glm::vec3(0.0f, 0.9393727f, -0.3428978f));
			const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
			const TerraEngine::Scene::SpatialQueryResult result = index.QueryFrustum(TerraEngine::Scene::ExtractFrustum(projection * view));

			context.Expect(HasHit(result, MakeUuid(1)), "The Scene View frustum should contain a marker at the world origin.");
			context.Expect(HasHit(result, MakeUuid(2)), "The Scene View frustum should contain a nearby elevated physics marker.");
			context.Expect(!HasHit(result, MakeUuid(3)), "A marker behind the camera should be rejected by the Scene View frustum.");
		});

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

		registry.Add("Engine.SpatialIndex.QueryUsesLatestIncrementalRefreshSnapshot", [](TestContext& context) {
			TerraEngine::Scene::SpatialIndex index;
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> initialRecords{
				MakeRecord(1, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f})
			};
			const std::vector<TerraEngine::Scene::SpatialObjectRecord> movedRecords{
				MakeRecord(1, {0.25f, -1.0f, -1.0f}, {1.25f, 1.0f, 1.0f}, 2)
			};

			index.Rebuild(initialRecords);
			index.Refresh(movedRecords);

			const TerraEngine::Scene::SpatialQueryResult oldArea = index.QueryAABB(
				TerraEngine::Scene::MakeAABB({-2.0f, -2.0f, -2.0f}, {-1.25f, 2.0f, 2.0f}, TerraEngine::Scene::SpatialBoundsSource::Authored));
			const TerraEngine::Scene::SpatialQueryResult newArea = index.QueryAABB(
				TerraEngine::Scene::MakeAABB({0.2f, -2.0f, -2.0f}, {1.5f, 2.0f, 2.0f}, TerraEngine::Scene::SpatialBoundsSource::Authored));

			context.Expect(!HasHit(oldArea, MakeUuid(1)), "Queries should not return the stale pre-refresh bounds.");
			context.Expect(HasHit(newArea, MakeUuid(1)), "Queries should return the latest moved bounds after incremental refresh.");
		});

		registry.Add("Engine.SpatialIndex.EditorSelectableSnapshotIncludesMarkerBearingNonRenderables", [](TestContext& context) {
			TerraEngine::Scene::SpatialObjectRecord physicsOnly = MakeRecord(4, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});
			physicsOnly.categoryMask = TerraEngine::Scene::SpatialCategoryEditorSelectable;
			physicsOnly.active = false;
			physicsOnly.worldBounds = TerraEngine::Scene::MakeFallbackBounds(glm::vec3(0.0f), 0.5f);
			TerraEngine::Scene::SpatialObjectRecord rendererOnly = MakeRecord(5, {-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f});
			rendererOnly.categoryMask = TerraEngine::Scene::SpatialCategoryRenderable;

			TerraEngine::Scene::EditorSelectableSpatialSnapshotPublisher publisher;
			context.Expect(publisher.Publish({physicsOnly, rendererOnly}, 7u), "A valid editor-selectable generation should publish atomically.");
			const std::shared_ptr<const TerraEngine::Scene::EditorSelectableSpatialSnapshot> snapshot = publisher.Latest();
			context.Expect(snapshot != nullptr && snapshot->IsValid(), "Readers should receive a valid immutable spatial snapshot.");
			context.ExpectEqual(1u, static_cast<unsigned int>(snapshot->RecordCount()), "Renderer-only records must not leak into the editor-selectable snapshot.");

			TerraEngine::Scene::SpatialQueryFilter filter{};
			filter.includeInactive = true;
			filter.categoryMask = TerraEngine::Scene::SpatialCategoryEditorSelectable;
			const TerraEngine::Scene::SpatialQueryResult result = snapshot->QueryAABB(
				TerraEngine::Scene::MakeAABB({-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}, TerraEngine::Scene::SpatialBoundsSource::Authored), filter);
			const TerraEngine::Scene::SpatialQueryResult sphereResult = snapshot->QuerySphere({glm::vec3(0.0f), 1000.0f}, filter);
			context.Expect(HasHit(result, MakeUuid(4)), "Inactive physics-only and other non-renderable marker objects must remain queryable to editor authoring surfaces.");
			context.Expect(HasHit(sphereResult, MakeUuid(4)), "Conservative Scene View fallback queries should retain inactive marker bounds.");
			context.Expect(!HasHit(result, MakeUuid(5)), "A renderer-only record is not an editor marker candidate.");
			context.Expect(!publisher.Publish({physicsOnly}, 6u), "A stale generation must not replace the latest immutable snapshot.");
			context.ExpectEqual(7u, static_cast<unsigned int>(publisher.Latest()->Generation()), "Rejected publication must leave the previous generation visible.");
		});
	}
}
