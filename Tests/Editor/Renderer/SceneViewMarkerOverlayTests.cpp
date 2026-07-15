#include "../TestHarness.h"
#include "SceneViewOverlayTestFixtures.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"

#include <algorithm>

namespace
{
	TerraEngine::UUID MarkerUuid(std::uint8_t value)
	{
		TerraEngine::UUID id{};
		id.back() = value;
		return id;
	}

	TerraEngine::Editor::SceneViewOverlay::MarkerStaticInput StaticMarker(
		std::uint8_t id,
		std::uint64_t lifetime,
		std::string name = "Marker")
	{
		TerraEngine::Editor::SceneViewOverlay::MarkerStaticInput input;
		input.objectId = MarkerUuid(id);
		input.objectLifetimeGeneration = lifetime;
		input.entityReference = static_cast<entt::entity>(id);
		input.componentGeneration = 1u;
		input.metadataGeneration = 1u;
		input.category = TerraEngine::Editor::SceneViewOverlay::MarkerCategory::Physics;
		input.displayName = std::move(name);
		input.tooltip = "Physics marker";
		return input;
	}

	TerraEngine::Editor::SceneViewOverlay::MarkerPoseRecord PoseMarker(
		std::uint8_t id,
		std::uint64_t lifetime,
		std::uint64_t revision,
		TerraEngine::UUID parent = TerraEngine::EMPTY_UUID)
	{
		TerraEngine::Editor::SceneViewOverlay::MarkerPoseRecord pose;
		pose.objectId = MarkerUuid(id);
		pose.parentObjectId = parent;
		pose.objectLifetimeGeneration = lifetime;
		pose.transformRevision = revision;
		pose.hierarchyGeneration = revision;
		pose.parentWorldGeneration = parent == TerraEngine::EMPTY_UUID ? 0u : revision;
		pose.worldPosition = glm::vec3(static_cast<float>(id), 0.0f, 0.0f);
		return pose;
	}
}

namespace TerraEngine::EditorTests
{
	void RegisterSceneViewMarkerOverlayTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Markers.ProjectAndCountCategories", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers;
			markers.push_back({ "camera-a", TerraEngine::EMPTY_UUID, entt::null, "Camera", TerraEngine::Editor::SceneViewOverlay::MarkerCategory::Camera, glm::vec3(0.0f, 0.0f, -2.0f) });
			markers.push_back({ "light-a", TerraEngine::EMPTY_UUID, entt::null, "Key", TerraEngine::Editor::SceneViewOverlay::MarkerCategory::PointLight, glm::vec3(0.25f, 0.0f, -2.0f) });
			for (auto& marker : markers)
			{
				const bool projected = TerraEngine::Editor::SceneViewOverlay::ProjectMarkerToScreen(marker, glm::mat4(1.0f), glm::vec2(0.0f), glm::vec2(100.0f));
				context.Expect(projected, marker.markerId + " should project into the viewport.");
			}
			auto snapshot = TerraEngine::Editor::SceneViewOverlay::CountMarkersIntoSnapshot({}, markers);
			context.ExpectEqual(1u, snapshot.cameraMarkerCount, "Visible camera marker should be counted.");
			context.ExpectEqual(1u, snapshot.pointLightMarkerCount, "Visible point light marker should be counted.");
		});

		registry.Add("SceneViewOverlay.Markers.StableMetadataIsIndependentFromPoseInvalidation", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerPipeline pipeline;
			context.Expect(pipeline.UpsertStaticRecord(StaticMarker(1u, 1u, "Shared Name")), "The first stable marker record should publish.");
			context.Expect(pipeline.UpsertStaticRecord(StaticMarker(2u, 1u, "Shared Name")), "The second stable marker record should publish.");
			context.Expect(pipeline.UpsertPoseRecord(PoseMarker(1u, 1u, 1u)), "The initial pose should publish.");
			const MarkerStringId stableName = pipeline.FindStaticRecord(MarkerUuid(1u))->displayNameId;
			context.ExpectEqual(stableName, pipeline.FindStaticRecord(MarkerUuid(2u))->displayNameId, "Equal labels should reuse one interned string identity.");
			const std::uint64_t staticGeneration = pipeline.StaticPublicationGeneration();
			MarkerPoseRecord moved = PoseMarker(1u, 1u, 2u);
			moved.worldPosition = glm::vec3(8.0f, 0.0f, 0.0f);
			context.Expect(pipeline.UpsertPoseRecord(moved), "A newer exact transform revision should update only the pose record.");
			context.ExpectEqual(staticGeneration, pipeline.StaticPublicationGeneration(), "Pose-only movement must retain marker identity, label, tooltip, and category.");
			context.ExpectEqual(stableName, pipeline.FindStaticRecord(MarkerUuid(1u))->displayNameId, "Moving an object must not reintern its stable label.");
		});

		registry.Add("SceneViewOverlay.Markers.ExactHierarchyDeletionAndRecreationInvalidateOnlyAffectedRecords", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerPipeline pipeline;
			for (std::uint8_t id = 1u; id <= 4u; ++id)
			{
				context.Expect(pipeline.UpsertStaticRecord(StaticMarker(id, 1u)), "Stable marker setup should publish.");
			}
			context.Expect(pipeline.UpsertPoseRecord(PoseMarker(1u, 1u, 1u)), "Root pose should publish.");
			context.Expect(pipeline.UpsertPoseRecord(PoseMarker(2u, 1u, 1u, MarkerUuid(1u))), "Child pose should publish.");
			context.Expect(pipeline.UpsertPoseRecord(PoseMarker(3u, 1u, 1u, MarkerUuid(2u))), "Grandchild pose should publish.");
			context.Expect(pipeline.UpsertPoseRecord(PoseMarker(4u, 1u, 1u)), "Unrelated pose should publish.");

			MarkerExactChangeSet reparent;
			reparent.sceneGeneration = 1u;
			reparent.hierarchyChangedObjectIds = { MarkerUuid(1u) };
			const MarkerInvalidationResult hierarchyResult = pipeline.ApplyExactChanges(reparent);
			context.ExpectEqual(static_cast<std::size_t>(3u), hierarchyResult.poseRefreshObjectIds.size(), "A reparented root must invalidate only itself and its cached descendants.");
			context.Expect(pipeline.FindPoseRecord(MarkerUuid(4u)) != nullptr, "An unrelated pose must remain cached after exact hierarchy invalidation.");
			context.Expect(pipeline.FindStaticRecord(MarkerUuid(2u)) != nullptr, "Hierarchy invalidation must retain stable marker metadata.");

			MarkerExactChangeSet removed;
			removed.sceneGeneration = 1u;
			removed.removedObjectIds = { MarkerUuid(4u) };
			const MarkerInvalidationResult removalResult = pipeline.ApplyExactChanges(removed);
			context.ExpectEqual(static_cast<std::size_t>(1u), removalResult.removedObjectIds.size(), "Deletion should publish one exact removed marker identity.");
			context.Expect(pipeline.FindStaticRecord(MarkerUuid(4u)) == nullptr && pipeline.FindPoseRecord(MarkerUuid(4u)) == nullptr, "Deletion must evict both stable and pose records atomically from the marker cache.");
			context.Expect(pipeline.UpsertStaticRecord(StaticMarker(4u, 2u, "Recreated")), "A recreated object with a newer lifetime must be accepted.");
			context.Expect(!pipeline.UpsertStaticRecord(StaticMarker(4u, 1u, "Stale")), "A stale pre-deletion lifetime must not overwrite a recreated marker.");
		});

		registry.Add("SceneViewOverlay.Markers.ViewportKeysSeparateProjectionFromPointerCells", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerViewportKey first;
			first.viewGeneration = 1u;
			first.projectionGeneration = 2u;
			first.viewportRectangle = glm::ivec4(0, 0, 1920, 1080);
			first.overlayPreferenceGeneration = 3u;
			first.markerPosePublicationGeneration = 4u;
			first.pointerCell = MarkerPointerCell{1, 2};
			MarkerViewportKey pointerMoved = first;
			pointerMoved.pointerCell = MarkerPointerCell{2, 2};
			context.Expect(first.SameProjectionInputs(pointerMoved), "Pointer movement should reuse unchanged projected neighborhoods.");
			context.Expect(!(first == pointerMoved), "Full hit-test keys must still identify a changed pointer cell.");
			MarkerViewportKey cameraMoved = first;
			++cameraMoved.viewGeneration;
			context.Expect(!first.SameProjectionInputs(cameraMoved), "A camera generation change must invalidate projected marker state.");
		});
	}
}
