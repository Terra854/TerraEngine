#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"
#include "Scene/WorldCoordinate.h"

#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterUnboundedSceneAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Editor.UnboundedAuthoring.CellLocalEntryNormalizesAtBoundary", [](TestContext& context) {
			Scene::WorldPosition entered;
			entered.cell = {15, -20, 3};
			entered.offsetMetres = {Scene::kWorldCellHalfEdgeMetres, -Scene::kWorldCellHalfEdgeMetres - 0.5, 0.25};
			const Scene::WorldCoordinateResult normalized = Scene::NormalizeWorldPosition(entered);
			context.Expect(normalized.Succeeded(), "Exact inspector cell/local input should normalize before commit.");
			context.ExpectEqual(16LL, normalized.position.cell.x, "Positive local boundary should carry into the adjacent cell.");
			context.ExpectEqual(-21LL, normalized.position.cell.y, "Negative local boundary should carry into the adjacent cell.");
		});

		registry.Add("Editor.UnboundedAuthoring.LocalizedMarkerKeepsCanonicalIdentity", [](TestContext& context) {
			Editor::SceneViewOverlay::MarkerPipeline pipeline;
			Editor::SceneViewOverlay::MarkerStaticInput marker;
			marker.objectId[15] = 42u;
			marker.objectLifetimeGeneration = 1u;
			marker.entityReference = static_cast<entt::entity>(42u);
			marker.componentGeneration = 1u;
			marker.metadataGeneration = 1u;
			marker.category = Editor::SceneViewOverlay::MarkerCategory::Physics;
			marker.displayName = "Distant marker";
			marker.tooltip = marker.displayName;
			context.Expect(pipeline.UpsertStaticRecord(marker), "The marker's stable identity must publish before its localized pose.");
			Editor::SceneViewOverlay::MarkerPoseRecord pose;
			pose.objectId = marker.objectId;
			pose.objectLifetimeGeneration = 1u;
			pose.transformRevision = 1u;
			pose.hierarchyGeneration = 1u;
			pose.canonicalPosition.cell = {15'258'789'062LL, 0, 0};
			pose.canonicalPosition.offsetMetres = {1.25, 0.0, 0.0};
			pose.localPosition = glm::vec3(1.25f, 0.0f, 0.0f);
			pose.localBounds = Scene::MakeFallbackBounds(pose.localPosition, 0.5f);
			context.Expect(pipeline.UpsertPoseRecord(pose), "A localized marker should publish from its immutable canonical identity.");
			const auto* stored = pipeline.FindPoseRecord(pose.objectId);
			context.Expect(stored != nullptr && stored->canonicalPosition == pose.canonicalPosition,
				"Re-localizing a marker must not replace its exact canonical address with a float coordinate.");
		});
	}
}
