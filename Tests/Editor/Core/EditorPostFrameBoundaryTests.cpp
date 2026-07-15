#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Windows/SceneViewMarkerPipeline.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		TerraEngine::UUID BoundaryMarkerId()
		{
			TerraEngine::UUID id{};
			id.back() = 1u;
			return id;
		}

		TerraEngine::Editor::SceneViewOverlay::MarkerViewportQuery MakeBoundaryQuery(
			const TerraEngine::Editor::SceneViewOverlay::MarkerPipeline& pipeline)
		{
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerViewportQuery query;
			query.viewportKey.viewGeneration = 1u;
			query.viewportKey.projectionGeneration = 1u;
			query.viewportKey.viewportRectangle = glm::ivec4(0, 0, 100, 100);
			query.viewportKey.overlayPreferenceGeneration = 1u;
			query.viewportKey.markerSpatialPublicationGeneration = 1u;
			query.viewportKey.markerPosePublicationGeneration = pipeline.PosePublicationGeneration();
			query.viewportSize = glm::vec2(100.0f);
			return query;
		}
	}

	void RegisterEditorPostFrameBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Editor.PostFrameBoundary.IsSchedulerOwnedAndProtected", [](TestContext& context) {
			auto read = [](const std::filesystem::path& path) {
				std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path);
				std::ostringstream source; source << input.rdbuf(); return source.str();
			};
			const std::string editor = read("Editor/TerraEngine-Editor/Core/EditorSystem.cpp");
			const std::string engine = read("Engine/Core/MainEngine.cpp");
			context.Expect(editor.find("declaration.phase = \"PostFrame\"") != std::string::npos, "EditorSystem must declare PostFrame.");
			context.Expect(editor.find("HardBoundaryClassification::Protected") != std::string::npos, "ImGui/Vulkan work must remain protected.");
			context.Expect(engine.find("systemScheduler.RegisterSystem(*editorSystem") != std::string::npos, "The scheduler must own editor frame execution.");
			context.Expect(engine.find("editorSystem->Update(deltaTime)") == std::string::npos, "Manual post-scheduler editor execution must be removed.");
			// An armed capture must use the runtime lifecycle state at the engine
			// boundary instead of relying on editor-window visibility or button state.
			context.Expect(engine.find("const bool playModeFrame = ScriptingSystem::Instance().IsRunning()") != std::string::npos,
				"Engine profiling must derive capture eligibility from active Play Mode.");
			context.Expect(engine.find("profiler.BeginFrame(playModeFrame)") != std::string::npos,
				"The main loop must pass Play Mode eligibility into every armed profiler frame boundary.");
		});

		registry.Add("Editor.PostFrameBoundary.PreparedMutationsDoNotPrematurelyInvalidateSceneViewCache", [](TestContext& context) {
			using namespace TerraEngine::Editor::SceneViewOverlay;
			MarkerPipeline pipeline;
			MarkerStaticInput staticInput;
			staticInput.objectId = BoundaryMarkerId();
			staticInput.objectLifetimeGeneration = 1u;
			staticInput.entityReference = static_cast<entt::entity>(1u);
			staticInput.componentGeneration = 1u;
			staticInput.metadataGeneration = 1u;
			staticInput.category = MarkerCategory::Physics;
			staticInput.displayName = "Boundary marker";
			staticInput.tooltip = staticInput.displayName;
			context.Expect(pipeline.UpsertStaticRecord(staticInput), "The fixture marker must publish before its viewport cache is tested.");
			MarkerPoseRecord pose;
			pose.objectId = staticInput.objectId;
			pose.objectLifetimeGeneration = 1u;
			pose.transformRevision = 1u;
			pose.hierarchyGeneration = 1u;
			pose.worldPosition = glm::vec3(0.0f);
			pose.worldBounds = TerraEngine::Scene::MakeAABB(glm::vec3(-0.1f), glm::vec3(0.1f), TerraEngine::Scene::SpatialBoundsSource::Authored);
			context.Expect(pipeline.UpsertPoseRecord(pose), "The fixture pose must publish before its viewport cache is tested.");

			MarkerViewportQuery query = MakeBoundaryQuery(pipeline);
			TerraEngine::Scene::SpatialQueryHit hit;
			hit.objectId = staticInput.objectId;
			hit.bounds = *pose.worldBounds;
			query.spatialCandidates.hits.push_back(hit);
			const MarkerFrameResult first = pipeline.PrepareViewport(query, {});
			context.Expect(!first.reusedViewportCache, "The first viewport preparation must populate the cache.");

			// A worker has only prepared an exact-change packet here. The editor must
			// keep rendering the immutable prior snapshot until its protected flush.
			const MarkerFrameResult beforeFlush = pipeline.PrepareViewport(query, {});
			context.Expect(beforeFlush.reusedViewportCache, "Prepared but unpublished mutations must not invalidate Scene View cache state.");

			MarkerExactChangeSet publishedChange;
			publishedChange.sceneGeneration = 1u;
			publishedChange.transformChangedObjectIds = {BoundaryMarkerId()};
			const MarkerInvalidationResult invalidated = pipeline.ApplyExactChanges(publishedChange);
			context.ExpectEqual(static_cast<std::size_t>(1u), invalidated.poseRefreshObjectIds.size(), "The declared publication boundary must invalidate only the changed marker pose.");
			context.Expect(pipeline.FindPoseRecord(BoundaryMarkerId()) == nullptr, "The committed change must evict the stale pose before the next immutable snapshot arrives.");
		});
	}
}
