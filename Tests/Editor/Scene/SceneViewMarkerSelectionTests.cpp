#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewOverlayMarkers.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine::Editor::SceneViewOverlay;

		// Use a stable value-derived UUID so the test can model deletion and
		// recreation without relying on allocator or entity-slot reuse.
		TerraEngine::UUID MakeSelectionMarkerId(std::uint16_t value)
		{
			TerraEngine::UUID id{};
			id[14] = static_cast<std::uint8_t>(value >> 8u);
			id[15] = static_cast<std::uint8_t>(value & 0xffu);
			return id;
		}

		void AddSelectionMarker(
			MarkerPipeline& pipeline,
			std::uint16_t value,
			MarkerCategory category,
			const glm::vec3& position,
			bool selected = false,
			std::uint64_t lifetime = 1u,
			std::uint64_t componentGeneration = 1u)
		{
			MarkerStaticInput staticInput;
			staticInput.objectId = MakeSelectionMarkerId(value);
			staticInput.objectLifetimeGeneration = lifetime;
			staticInput.entityReference = static_cast<entt::entity>(value);
			staticInput.componentGeneration = componentGeneration;
			staticInput.metadataGeneration = componentGeneration;
			staticInput.category = category;
			staticInput.displayName = "Selection marker " + std::to_string(value);
			staticInput.tooltip = staticInput.displayName;
			staticInput.selected = selected;
			(void)pipeline.UpsertStaticRecord(staticInput);

			MarkerPoseRecord pose;
			pose.objectId = staticInput.objectId;
			pose.objectLifetimeGeneration = lifetime;
			pose.transformRevision = componentGeneration;
			pose.hierarchyGeneration = componentGeneration;
			pose.worldPosition = position;
			pose.worldBounds = TerraEngine::Scene::MakeAABB(
				position - glm::vec3(0.1f), position + glm::vec3(0.1f),
				TerraEngine::Scene::SpatialBoundsSource::Authored);
			(void)pipeline.UpsertPoseRecord(pose);
		}

		MarkerViewportQuery MakeSelectionQuery(const MarkerPipeline& pipeline)
		{
			MarkerViewportQuery query;
			query.viewportKey.viewGeneration = 1u;
			query.viewportKey.projectionGeneration = 1u;
			query.viewportKey.viewportRectangle = glm::ivec4(0, 0, 1000, 1000);
			query.viewportKey.overlayPreferenceGeneration = 1u;
			query.viewportKey.markerSpatialPublicationGeneration = 1u;
			query.viewportKey.markerPosePublicationGeneration = pipeline.PosePublicationGeneration();
			query.viewportSize = glm::vec2(1000.0f);
			return query;
		}

		void AddSelectionCandidate(const MarkerPipeline& pipeline, MarkerViewportQuery& query, std::uint16_t value)
		{
			const MarkerPoseRecord* pose = pipeline.FindPoseRecord(MakeSelectionMarkerId(value));
			if (pose == nullptr)
			{
				return;
			}
			TerraEngine::Scene::SpatialQueryHit hit;
			hit.objectId = pose->objectId;
			hit.bounds = pose->worldBounds.value_or(TerraEngine::Scene::SpatialAABB{});
			query.spatialCandidates.hits.push_back(std::move(hit));
		}

		bool ContainsPresentedId(const MarkerFrameResult& result, const TerraEngine::UUID& id)
		{
			return std::any_of(result.presentationBatches.begin(), result.presentationBatches.end(), [&id](const MarkerPresentationBatch& batch) {
				return std::any_of(batch.items.begin(), batch.items.end(), [&id](const MarkerPresentationItem& item) {
					return item.objectId == id || item.hitTestRepresentative == id;
				});
			});
		}
	}

	void RegisterSceneViewMarkerSelectionTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Markers.SelectedCameraAndLightAreBudgetExempt", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			context.Expect(IsMarkerReserved(MarkerCategory::SelectedObject, false), "Selected-object markers must be reserved before the general budget.");
			context.Expect(IsMarkerReserved(MarkerCategory::Camera, false), "Camera markers must be reserved before the general budget.");
			context.Expect(IsMarkerReserved(MarkerCategory::PointLight, false), "Light markers must be reserved before the general budget.");
			context.Expect(!IsMarkerReserved(MarkerCategory::Physics, false), "Unselected physics markers must use the configurable general budget.");
		});

		registry.Add("SceneViewOverlay.Markers.HitTestIsDeterministic", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers(2u);
			markers[0].markerId = "z-light";
			markers[0].category = TerraEngine::Editor::SceneViewOverlay::MarkerCategory::PointLight;
			markers[0].screenPosition = glm::vec2(50.0f);
			markers[0].visible = true;
			markers[0].visualPriority = TerraEngine::Editor::SceneViewOverlay::MarkerCategoryPriority(markers[0].category, false);
			markers[1].markerId = "a-camera";
			markers[1].category = TerraEngine::Editor::SceneViewOverlay::MarkerCategory::Camera;
			markers[1].screenPosition = glm::vec2(50.0f);
			markers[1].visible = true;
			markers[1].visualPriority = TerraEngine::Editor::SceneViewOverlay::MarkerCategoryPriority(markers[1].category, false);

			const auto hit = TerraEngine::Editor::SceneViewOverlay::HitTestMarkers(markers, glm::vec2(50.0f), 12.0f);
			context.Expect(hit.has_value(), "Overlapping marker hit should return a candidate.");
			context.ExpectEqual(std::string("a-camera"), markers[*hit].markerId, "Higher-priority camera marker should win deterministic overlap selection.");
		});

		registry.Add("SceneViewOverlay.Markers.SpatialOverlapGroupsRemainConnectedAndReset", [](TestContext& context) {
			std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> markers(4u);
			markers[0].screenPosition = glm::vec2(0.0f, 0.0f);
			markers[1].screenPosition = glm::vec2(12.0f, 0.0f);
			markers[2].screenPosition = glm::vec2(24.0f, 0.0f);
			markers[3].screenPosition = glm::vec2(96.0f, 0.0f);
			for (TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker& marker : markers)
			{
				marker.visible = true;
			}

			TerraEngine::Editor::SceneViewOverlay::ResolveMarkerOverlapGroups(markers, 18.0f);
			context.Expect(markers[0].overlapGroup != 0U, "Visible markers must receive an overlap group.");
			context.Expect(markers[0].overlapGroup == markers[1].overlapGroup && markers[1].overlapGroup == markers[2].overlapGroup, "A chain of overlapping markers must resolve as one connected group.");
			context.Expect(markers[3].overlapGroup != markers[0].overlapGroup, "Distant markers must retain an independent overlap group.");

			markers[2].visible = false;
			TerraEngine::Editor::SceneViewOverlay::ResolveMarkerOverlapGroups(markers, 18.0f);
			context.Expect(markers[2].overlapGroup == 0U, "Reused marker snapshots must clear overlap data when a marker becomes hidden.");
		});

		registry.Add("SceneViewOverlay.Markers.ExtremeDensityKeepsReservedObjectsDiscoverable", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddSelectionMarker(pipeline, 1u, MarkerCategory::SelectedObject, glm::vec3(-0.9f, 0.0f, 0.0f), true);
			AddSelectionMarker(pipeline, 2u, MarkerCategory::Camera, glm::vec3(-0.7f, 0.0f, 0.0f));
			AddSelectionMarker(pipeline, 3u, MarkerCategory::PointLight, glm::vec3(-0.5f, 0.0f, 0.0f));
			for (std::uint16_t value = 4u; value < 4100u; ++value)
			{
				// Every ordinary physics marker projects into one dense region. This
				// models the worst editor interaction case without creating live objects.
				AddSelectionMarker(pipeline, value, MarkerCategory::Physics, glm::vec3(0.0f));
			}

			MarkerViewportQuery query = MakeSelectionQuery(pipeline);
			for (std::uint16_t value = 1u; value < 4100u; ++value)
			{
				AddSelectionCandidate(pipeline, query, value);
			}
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			preference.visibleMarkerBudget = 8u;
			preference.densityThreshold = 4u;
			const MarkerFrameResult frame = pipeline.PrepareViewport(query, preference);

			context.Expect(ContainsPresentedId(frame, MakeSelectionMarkerId(1u)), "Extreme density must not hide the selected object.");
			context.Expect(ContainsPresentedId(frame, MakeSelectionMarkerId(2u)), "Extreme density must not hide a camera marker.");
			context.Expect(ContainsPresentedId(frame, MakeSelectionMarkerId(3u)), "Extreme density must not hide a light marker.");
			context.Expect(frame.candidates.budgetRejectedCount > 0u, "The general marker budget should still constrain the dense non-reserved set.");
		});

		registry.Add("SceneViewOverlay.Markers.OffscreenDeletionRecreationAndCategoryChangesStayGenerationSafe", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddSelectionMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(3.0f, 0.0f, 0.0f));
			MarkerViewportQuery offscreenQuery = MakeSelectionQuery(pipeline);
			AddSelectionCandidate(pipeline, offscreenQuery, 1u);
			const MarkerFrameResult offscreen = pipeline.PrepareViewport(offscreenQuery, {});
			context.Expect(offscreen.screenIndex.entries.empty(), "An off-screen marker must be rejected before interaction and presentation work.");
			context.Expect(offscreen.candidates.frustumRejectedCount == 1u, "Off-screen rejection must retain an exact diagnostic count.");

			MarkerExactChangeSet deletion;
			deletion.sceneGeneration = 1u;
			deletion.removedObjectIds = {MakeSelectionMarkerId(1u)};
			const MarkerInvalidationResult deleted = pipeline.ApplyExactChanges(deletion);
			context.ExpectEqual(static_cast<std::size_t>(1u), deleted.removedObjectIds.size(), "Deletion must publish one exact marker identity.");
			context.Expect(pipeline.FindStaticRecord(MakeSelectionMarkerId(1u)) == nullptr, "Deletion must remove the stale marker record before recreation.");

			AddSelectionMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(0.0f), false, 2u, 1u);
			context.Expect(pipeline.FindStaticRecord(MakeSelectionMarkerId(1u))->objectLifetimeGeneration == 2u, "Recreation must replace the deleted identity with its newer lifetime generation.");
			AddSelectionMarker(pipeline, 1u, MarkerCategory::Camera, glm::vec3(0.0f), false, 2u, 2u);
			const MarkerStaticRecord* changed = pipeline.FindStaticRecord(MakeSelectionMarkerId(1u));
			context.Expect(changed != nullptr && changed->category == MarkerCategory::Camera, "A component-generation category change must replace physics metadata without retaining the old category.");
			AddSelectionMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(0.0f), false, 1u, 99u);
			context.Expect(pipeline.FindStaticRecord(MakeSelectionMarkerId(1u))->category == MarkerCategory::Camera, "A stale pre-recreation update must never overwrite a newer marker lifetime.");
		});
	}
}
