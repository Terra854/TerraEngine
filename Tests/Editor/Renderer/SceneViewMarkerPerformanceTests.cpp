// SceneViewMarkerPerformanceTests.cpp - Large-scene marker reduction contracts.

#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine::Editor::SceneViewOverlay;

		TerraEngine::UUID MakeMarkerId(std::uint16_t value)
		{
			TerraEngine::UUID id{};
			id[14] = static_cast<std::uint8_t>(value >> 8u);
			id[15] = static_cast<std::uint8_t>(value & 0xffu);
			return id;
		}

		void AddMarker(
			MarkerPipeline& pipeline,
			std::uint16_t id,
			MarkerCategory category,
			const glm::vec3& position,
			bool selected = false,
			float boundsRadius = 0.1f,
			std::uint64_t revision = 1u)
		{
			MarkerStaticInput stable;
			stable.objectId = MakeMarkerId(id);
			stable.objectLifetimeGeneration = 1u;
			stable.entityReference = static_cast<entt::entity>(id);
			stable.componentGeneration = 1u;
			stable.metadataGeneration = 1u;
			stable.category = category;
			stable.displayName = "Marker " + std::to_string(id);
			stable.tooltip = stable.displayName;
			stable.selected = selected;
			(void)pipeline.UpsertStaticRecord(stable);

			MarkerPoseRecord pose;
			pose.objectId = stable.objectId;
			pose.objectLifetimeGeneration = 1u;
			pose.transformRevision = revision;
			pose.hierarchyGeneration = 1u;
			pose.worldPosition = position;
			pose.worldBounds = TerraEngine::Scene::MakeAABB(
				position - glm::vec3(boundsRadius),
				position + glm::vec3(boundsRadius),
				TerraEngine::Scene::SpatialBoundsSource::Authored);
			(void)pipeline.UpsertPoseRecord(pose);
		}

		MarkerViewportQuery MakeQuery(const MarkerPipeline& pipeline, glm::vec2 viewportSize = glm::vec2(100.0f))
		{
			MarkerViewportQuery query;
			query.viewportKey.viewGeneration = 1u;
			query.viewportKey.projectionGeneration = 1u;
			query.viewportKey.viewportRectangle = glm::ivec4(0, 0, static_cast<int>(viewportSize.x), static_cast<int>(viewportSize.y));
			query.viewportKey.overlayPreferenceGeneration = 1u;
			query.viewportKey.markerSpatialPublicationGeneration = 1u;
			query.viewportKey.markerPosePublicationGeneration = pipeline.PosePublicationGeneration();
			query.viewportSize = viewportSize;
			return query;
		}

		void AppendAllHits(const MarkerPipeline& pipeline, MarkerViewportQuery& query, std::uint16_t first, std::uint16_t last)
		{
			for (std::uint16_t id = first; id <= last; ++id)
			{
				const MarkerPoseRecord* pose = pipeline.FindPoseRecord(MakeMarkerId(id));
				if (pose == nullptr)
				{
					continue;
				}
				TerraEngine::Scene::SpatialQueryHit hit;
				hit.objectId = pose->objectId;
				hit.bounds = pose->worldBounds.value_or(TerraEngine::Scene::SpatialAABB{});
				query.spatialCandidates.hits.push_back(hit);
			}
		}

		std::size_t PresentedCount(const MarkerFrameResult& result)
		{
			std::size_t count = 0u;
			for (const MarkerPresentationBatch& batch : result.presentationBatches)
			{
				count += batch.items.size();
			}
			return count;
		}
	}

	void RegisterSceneViewMarkerPerformanceTests(TestRegistry& registry)
	{
		registry.Add("SceneViewOverlay.Markers.RejectBeforeInteractionAndEmission", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(0.0f), false, 0.1f);
			AddMarker(pipeline, 2u, MarkerCategory::Physics, glm::vec3(2.0f, 0.0f, 0.0f), false, 0.1f);
			AddMarker(pipeline, 3u, MarkerCategory::Physics, glm::vec3(0.5f, 0.0f, 5.0f), false, 0.1f);
			AddMarker(pipeline, 4u, MarkerCategory::Physics, glm::vec3(-0.5f, 0.0f, 0.0f), false, 0.001f);
			MarkerViewportQuery query = MakeQuery(pipeline);
			AppendAllHits(pipeline, query, 1u, 4u);
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			preference.maximumMarkerDistance = 2.0f;
			preference.minimumProjectedSizePixels = 1.0f;

			const MarkerFrameResult result = pipeline.PrepareViewport(query, preference);
			context.ExpectEqual(static_cast<std::size_t>(1u), result.screenIndex.entries.size(), "Only the marker inside the exact view, distance, and projected-size bounds should reach interaction work.");
			context.Expect(result.candidates.frustumRejectedCount >= 1u, "Off-frustum markers must be counted before interaction work.");
			context.ExpectEqual(1u, result.candidates.distanceRejectedCount, "Excessive-distance markers must be rejected before projection output is retained.");
			context.ExpectEqual(1u, result.candidates.sizeRejectedCount, "Sub-pixel markers must be rejected before hit testing and drawing.");
		});

		registry.Add("SceneViewOverlay.Markers.DenseGroupsConsolidateButReservedMarkersStayIndividual", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddMarker(pipeline, 1u, MarkerCategory::SelectedObject, glm::vec3(-0.8f, 0.0f, 0.0f), true);
			for (std::uint16_t id = 2u; id <= 9u; ++id)
			{
				// Ten-pixel steps form one connected dense neighborhood while
				// leaving the last member outside the representative's hit radius.
				const float x = -0.175f + static_cast<float>(id - 2u) * 0.05f;
				AddMarker(pipeline, id, MarkerCategory::Physics, glm::vec3(x, 0.0f, 0.0f));
			}
			MarkerViewportQuery query = MakeQuery(pipeline, glm::vec2(400.0f));
			AppendAllHits(pipeline, query, 1u, 9u);
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			preference.densityThreshold = 4u;
			preference.densityRadiusPixels = 18.0f;

			const MarkerFrameResult clustered = pipeline.PrepareViewport(query, preference);
			context.ExpectEqual(static_cast<std::size_t>(2u), PresentedCount(clustered), "Eight dense low-priority physics markers should consolidate beside one individually addressable selection.");
			bool foundCluster = false;
			bool foundReserved = false;
			for (const MarkerPresentationBatch& batch : clustered.presentationBatches)
			{
				for (const MarkerPresentationItem& item : batch.items)
				{
					foundCluster = foundCluster || item.clusterMemberCount == 8u;
					foundReserved = foundReserved || (item.reserved && item.objectId == MakeMarkerId(1u));
				}
			}
			context.Expect(foundCluster, "The consolidated presentation item must retain all expandable member identities.");
			context.Expect(foundReserved, "The selected marker must remain an individual hit-test target in a dense group.");
			const MarkerHitTestResult clusteredHit = pipeline.HitTest(clustered.screenIndex, glm::vec2(235.0f, 200.0f), 4.0f);
			context.Expect(clusteredHit.objectId.has_value() && *clusteredHit.objectId == MakeMarkerId(2u), "A hidden cluster member must resolve to the visible deterministic cluster representative.");
		});

		registry.Add("SceneViewOverlay.Markers.BudgetAndPresentationBatchesAreBounded", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddMarker(pipeline, 1u, MarkerCategory::SelectedObject, glm::vec3(-0.95f, -0.95f, 0.0f), true);
			AddMarker(pipeline, 2u, MarkerCategory::Camera, glm::vec3(-0.90f, -0.95f, 0.0f));
			AddMarker(pipeline, 3u, MarkerCategory::PointLight, glm::vec3(-0.85f, -0.95f, 0.0f));
			for (std::uint16_t id = 4u; id <= 73u; ++id)
			{
				const std::uint16_t offset = id - 4u;
				const float x = -0.9f + static_cast<float>(offset % 10u) * 0.18f;
				const float y = -0.75f + static_cast<float>(offset / 10u) * 0.24f;
				AddMarker(pipeline, id, MarkerCategory::Physics, glm::vec3(x, y, 0.0f));
			}
			MarkerViewportQuery query = MakeQuery(pipeline, glm::vec2(2000.0f));
			AppendAllHits(pipeline, query, 1u, 73u);
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			preference.visibleMarkerBudget = 64u;
			preference.markerPresentationBatchSize = 32u;
			preference.densityRadiusPixels = 18.0f;

			const MarkerFrameResult result = pipeline.PrepareViewport(query, preference);
			context.ExpectEqual(static_cast<std::size_t>(67u), result.screenIndex.entries.size(), "The general budget must retain 64 general markers plus all three reserved markers.");
			context.ExpectEqual(6u, result.candidates.budgetRejectedCount, "Only general markers beyond the configured budget should be rejected.");
			context.ExpectEqual(static_cast<std::size_t>(3u), result.presentationBatches.size(), "Sixty-seven presentation items should be emitted in bounded 32-item batches.");
			context.Expect(std::all_of(result.presentationBatches.begin(), result.presentationBatches.end(), [](const MarkerPresentationBatch& batch) { return batch.items.size() <= 32u; }), "No marker presentation batch may exceed its configured bound.");
		});

		registry.Add("SceneViewOverlay.Markers.NavigationRevealsClusterMembersAndCacheKeysAreExact", [](TestContext& context) {
			MarkerPipeline pipeline;
			for (std::uint16_t id = 1u; id <= 4u; ++id)
			{
				AddMarker(pipeline, id, MarkerCategory::EmptyObject, glm::vec3(0.0f));
			}
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			preference.densityThreshold = 4u;
			MarkerViewportQuery firstQuery = MakeQuery(pipeline, glm::vec2(400.0f));
			AppendAllHits(pipeline, firstQuery, 1u, 4u);
			const MarkerFrameResult clustered = pipeline.PrepareViewport(firstQuery, preference);
			context.ExpectEqual(static_cast<std::size_t>(1u), PresentedCount(clustered), "The initial dense neighborhood should present one cluster.");

			MarkerViewportQuery pointerMoved = firstQuery;
			pointerMoved.viewportKey.pointerCell = MarkerPointerCell{2, 3};
			const MarkerFrameResult cached = pipeline.PrepareViewport(pointerMoved, preference);
			context.Expect(cached.reusedViewportCache, "Pointer-cell movement must reuse the unchanged projected screen index.");

			const float positions[] = {-0.75f, -0.25f, 0.25f, 0.75f};
			for (std::uint16_t id = 1u; id <= 4u; ++id)
			{
				AddMarker(pipeline, id, MarkerCategory::EmptyObject, glm::vec3(positions[id - 1u], 0.0f, 0.0f), false, 0.1f, 2u);
			}
			MarkerViewportQuery navigated = MakeQuery(pipeline, glm::vec2(400.0f));
			navigated.viewportKey.viewGeneration = 2u;
			AppendAllHits(pipeline, navigated, 1u, 4u);
			const MarkerFrameResult revealed = pipeline.PrepareViewport(navigated, preference);
			context.Expect(!revealed.reusedViewportCache, "Changed pose and view generations must invalidate the previous neighborhoods.");
			context.ExpectEqual(static_cast<std::size_t>(4u), PresentedCount(revealed), "After navigation separates the markers, each object must become individually presentable again.");
		});

		registry.Add("SceneViewOverlay.Markers.SpatialPublicationInvalidatesEmptyViewportCache", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(0.0f));
			SceneOverlayPreference preference;
			MarkerViewportQuery emptyQuery = MakeQuery(pipeline);
			const MarkerFrameResult empty = pipeline.PrepareViewport(emptyQuery, preference);
			context.ExpectEqual(static_cast<std::size_t>(0u), PresentedCount(empty), "A viewport queried before spatial publication should initially remain empty.");

			MarkerViewportQuery publishedQuery = emptyQuery;
			publishedQuery.viewportKey.markerSpatialPublicationGeneration = 2u;
			AppendAllHits(pipeline, publishedQuery, 1u, 1u);
			const MarkerFrameResult published = pipeline.PrepareViewport(publishedQuery, preference);
			context.Expect(!published.reusedViewportCache, "A newer immutable spatial publication must invalidate an earlier empty viewport cache.");
			context.ExpectEqual(static_cast<std::size_t>(1u), PresentedCount(published), "The newly published spatial candidate should reach presentation without camera movement.");
		});

		registry.Add("SceneViewOverlay.Markers.ScreenIndexHitTestAndTelemetryAreDeterministic", [](TestContext& context) {
			MarkerPipeline pipeline;
			AddMarker(pipeline, 1u, MarkerCategory::Physics, glm::vec3(0.0f));
			AddMarker(pipeline, 2u, MarkerCategory::Camera, glm::vec3(0.0f));
			MarkerViewportQuery query = MakeQuery(pipeline);
			AppendAllHits(pipeline, query, 1u, 2u);
			SceneOverlayPreference preference;
			preference.largeSceneMode = LargeSceneMode::On;
			const MarkerFrameResult frame = pipeline.PrepareViewport(query, preference);
			const MarkerHitTestResult hit = pipeline.HitTest(frame.screenIndex, glm::vec2(50.0f), 12.0f);
			context.Expect(hit.objectId.has_value() && *hit.objectId == MakeMarkerId(2u), "The reusable screen index must select the higher-priority camera deterministically.");
			context.Expect(hit.telemetry.stageName == "SceneView.HitTest" && hit.telemetry.duration.availability == TerraEngine::Profiling::TelemetryAvailability::Available, "Hit-test cost must publish typed Scene View telemetry.");

			const std::vector<std::string> requiredStages = {
				"SceneView.MarkerCache",
				"SceneView.CandidateQuery",
				"SceneView.Projection",
				"SceneView.Density",
				"SceneView.Emission"
			};
			for (const std::string& stage : requiredStages)
			{
				context.Expect(std::any_of(frame.stageTelemetry.begin(), frame.stageTelemetry.end(), [&](const auto& record) { return record.stageName == stage; }), "Every marker preparation stage must remain visible to the profiler.");
			}
		});
	}
}
