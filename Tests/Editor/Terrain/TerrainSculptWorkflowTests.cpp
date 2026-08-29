// TerrainSculptWorkflowTests.cpp -- Detached preview, picking, and transaction checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSession.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainPicking.h"
#include "Editor/TerraEngine-Editor/Windows/TerrainWindow.h"

#include "Terrain/TerrainPublication.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>

#include <glm/ext/matrix_clip_space.hpp>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainSculptWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainSculpt.DetachedPreviewRejectsUnavailableArea", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(); root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1U; root->rootContentHash = {1U, 2U}; root->layerStackGeneration = 1U;
			TerraEngine::Editor::Terrain::TerrainAuthoringSession session; std::string error;
			context.Expect(session.Begin({root, 1U}, error), "A committed root must begin a detached session.");
			std::vector<std::int32_t> delta(TerraEngine::Terrain::kTerrainAreaSampleCount, 1);
			context.Expect(!session.AddHeightDelta({0, 0}, delta, error), "An unavailable area must reject without modifying a preview.");
			session.SetResidentAreas({{0, 0}}); context.Expect(session.AddHeightDelta({0, 0}, delta, error) && session.IsDirty(), "A resident area must receive its detached preview delta.");
			session.Cancel(); context.Expect(!session.IsActive() && !session.IsDirty(), "Cancel must discard only detached authoring state.");
		});

		registry.Add("Editor.TerrainSculpt.CanonicalPickAndGpuQualification", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			const auto hit = TerrainPicking::PickHeightfield({{4.5, 10.0, 3.5}, {0.0, -1.0, 0.0}}, [](std::int64_t, std::int64_t) { return std::optional<double>{2.0}; }, 20.0);
			const TerraEngine::UUID id = TerraEngine::GenerateUUID();
			context.Expect(hit.has_value() && hit->sample.x == 4 && hit->sample.z == 3
				&& TerrainPicking::QualifyGpuIdentity({id, 9U}, id, 9U) && !TerrainPicking::QualifyGpuIdentity({id, 8U}, id, 9U),
				"CPU canonical picks and GPU identity generations must agree before a brush accepts a target.");
		});

		registry.Add("Editor.TerrainSculpt.PointerRayTracksViewportAndEntersFinitePatch", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			const glm::mat4 view(1.0F);
			const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0F), 1.0F, 0.1F, 1000.0F);
			const auto centre = TerrainPicking::BuildViewportRay(view, projection, 0.5, 0.5, false);
			const auto right = TerrainPicking::BuildViewportRay(view, projection, 0.9, 0.5, false);
			context.Expect(centre && right && std::abs(centre->direction.x) < 0.0001
				&& right->direction.x > 0.1,
				"Terrain pointer rays must follow the Scene View cursor instead of always using camera forward.");

			const auto entered = TerrainPicking::PickHeightfield(
				{{-5.0, 10.0, 4.0}, {1.0, -0.5, 0.0}},
				[](std::int64_t x, std::int64_t z) -> std::optional<double> {
					if(x < 0 || x >= 16 || z < 0 || z >= 16) return std::nullopt;
					return 2.0;
				}, 40.0, 0.25);
			context.Expect(entered.has_value() && entered->sample.x >= 0,
				"A terrain ray must march into a finite resident patch when the camera begins outside it.");
		});

		registry.Add("Editor.TerrainSculpt.PreviewOverflowRejectsAtomically", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1U;
			root->rootContentHash = {1U, 2U};
			root->layerStackGeneration = 1U;
			TerraEngine::Editor::Terrain::TerrainAuthoringSession session;
			std::string error;
			context.Expect(session.Begin({root, 1U}, error), "A committed root must begin a detached session.");
			session.SetResidentAreas({{0, 0}});

			std::vector<std::int32_t> maximum(TerraEngine::Terrain::kTerrainAreaSampleCount,
				std::numeric_limits<std::int32_t>::max());
			context.Expect(session.AddHeightDelta({0, 0}, maximum, error),
				"The largest representable preview value must be accepted.");
			std::vector<std::int32_t> overflow(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
			overflow.back() = 1;
			context.Expect(!session.AddHeightDelta({0, 0}, overflow, error),
				"An overflowing stroke must reject before any sample is changed.");
			std::vector<std::int32_t> negativeOne(TerraEngine::Terrain::kTerrainAreaSampleCount, -1);
			context.Expect(session.AddHeightDelta({0, 0}, negativeOne, error),
				"The rejected stroke must leave every prior preview sample unchanged.");
		});

		registry.Add("Editor.TerrainSculpt.FourAreaCornerBuildsOneGroupedCommit", [](TestContext& context) {
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(); root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 5U; root->rootContentHash = {5U, 6U}; root->layerStackGeneration = 3U;
			TerraEngine::Editor::Terrain::TerrainAuthoringSession session; std::string error;
			context.Expect(session.Begin({root, 5U}, error), "A committed root must begin the four-area edit.");
			const std::set<TerraEngine::Terrain::TerrainAreaCoordinate> corner = {{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
			session.SetResidentAreas(corner); std::vector<std::int32_t> delta(TerraEngine::Terrain::kTerrainAreaSampleCount, 1);
			for(const auto& area : corner)
				context.Expect(session.AddHeightDelta(area, delta, error), "Each resident corner area must accept its preview delta.");
			std::vector<TerraEngine::Terrain::TerrainPatch> patches;
			for (const auto& area : corner) { TerraEngine::Terrain::TerrainPatch patch; patch.replacement.coordinate = area; patches.push_back(patch); }
			const auto transaction = session.BuildCommitRequest(std::move(patches), "Four-area seam stroke", error);
			context.Expect(transaction.has_value() && transaction->readSet.size() == 4U && transaction->patches.size() == 4U
				&& transaction->targetAssetUuid == root->assetUuid,
				"A corner stroke must use one detached transaction covering all four affected areas.");
		});

		registry.Add("Editor.TerrainSculpt.ProtectedQueuePublishesOneApplyReceipt", [](TestContext& context) {
			const TerraEngine::UUID assetId = TerraEngine::GenerateUUID();
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(); root->assetUuid = assetId; root->publicationGeneration = 1U; root->rootContentHash = {10U, 11U}; root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1U;
			TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = {12U, 13U};
			TerraEngine::Terrain::TerrainAreaDescriptor before; before.coordinate = {0, 0}; before.editGeneration = 1U; before.authoredContentHash = {14U, 15U}; shard.descriptors.emplace(before.coordinate, before); root->directoryShards.push_back(std::move(shard));
			TerraEngine::Terrain::TerrainAssetStore store(root); TerraEngine::Terrain::TerrainMutationQueue queue; TerraEngine::Terrain::TerrainPublication publication(store);
			const std::vector<std::uint8_t> preparedBytes{1U, 2U, 3U};
			const TerraEngine::Terrain::ContentHash prepared = TerraEngine::Terrain::HashTerrainBytes(preparedBytes);
			store.Blobs().PutVerified(prepared, preparedBytes);
			TerraEngine::Terrain::TerrainPatch patch; patch.replacement = before; patch.replacement.editGeneration = 2U; patch.replacement.authoredContentHash = prepared; patch.preparedBlobHash = prepared;
			TerraEngine::Terrain::TerrainEditTransaction transaction; transaction.transactionId = TerraEngine::GenerateUUID(); transaction.targetAssetUuid = assetId; transaction.inputRootHash = root->rootContentHash; transaction.expectedLayerStackGeneration = 1U; transaction.affectedBounds = root->authoredBounds; transaction.budget.maximumAreas = 1U; transaction.readSet = {{before.coordinate, before.editGeneration, before.authoredContentHash}}; transaction.patches = {patch};
			context.Expect(queue.Enqueue(std::move(transaction)).Succeeded(), "The bounded mutation queue must accept the prepared Apply transaction.");
			const auto receipts = publication.PublishQueued(queue);
			context.Expect(receipts.size() == 1U && receipts.front().outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed && store.CaptureRoot().publicationGeneration == 2U,
				"Apply must enqueue a typed terrain transaction and expose one committed receipt only after the protected publication boundary.");
		});

		registry.Add("Editor.TerrainSculpt.SelectedTargetOwnsSelection", [](TestContext& context) {
			context.Expect(TerrainWindow::ShouldCaptureSceneSelection(true, true)
				&& !TerrainWindow::ShouldCaptureSceneSelection(true, false)
				&& !TerrainWindow::ShouldCaptureSceneSelection(false, true),
				"An enabled terrain tool must own Scene View clicks only while its exact terrain target remains selected.");

			const std::filesystem::path sceneViewPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp";
			std::ifstream sceneViewFile(sceneViewPath, std::ios::binary);
			const std::string sceneViewSource(
				(std::istreambuf_iterator<char>(sceneViewFile)),
				std::istreambuf_iterator<char>());
			context.Expect(sceneViewSource.find(
				"terrainTools->IsEditingTargetSelected(selectedObjectId)") != std::string::npos
				&& sceneViewSource.find("terrainTools->HandleEditingTargetDeselected()") != std::string::npos,
				"Scene View must compare the selected object with Terrain Tools' durable target and cancel a detached stroke after deselection.");
			const std::filesystem::path editorPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp";
			std::ifstream editorFile(editorPath, std::ios::binary);
			const std::string editorSource(
				(std::istreambuf_iterator<char>(editorFile)),
				std::istreambuf_iterator<char>());
			context.Expect(editorSource.find(
				"terrainTools->SynchronizeEditingSelection(selectedTerrainObjectId)") != std::string::npos,
				"The selected Terrain component must activate authoring even when the Inspector window is hidden.");
		});

		registry.Add("Editor.TerrainSculpt.DetachedBrushPublishesRealtimeRenderPreview", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp";
			std::ifstream sourceFile(sourcePath, std::ios::binary);
			const std::string source(
				(std::istreambuf_iterator<char>(sourceFile)),
				std::istreambuf_iterator<char>());
			const auto methodBody = [&source](std::string_view declaration, std::string_view nextDeclaration) {
				const std::size_t begin = source.find(declaration);
				const std::size_t end = begin == std::string::npos
					? std::string::npos : source.find(nextDeclaration, begin + declaration.size());
				return begin != std::string::npos && end != std::string::npos
					? source.substr(begin, end - begin) : std::string();
			};

			const std::string schedule = methodBody(
				"void TerrainWindow::ScheduleRenderProxyBuild()",
				"void TerrainWindow::QueueRenderProxySceneInstance()");
			const std::string publish = methodBody(
				"void TerrainWindow::PollRenderProxyBuild()",
				"void TerrainWindow::SynchronizeTerrainMaterial");
			const std::string stamp = methodBody(
				"void TerrainWindow::ApplyBrushAt",
				"bool TerrainWindow::QueuePreviewForPublication()");
			const std::string pointer = methodBody(
				"void TerrainWindow::HandleSceneViewPointer",
				"void TerrainWindow::ApplyHoleBrushAt");
			context.Expect(schedule.find("authoringSession.IsDirty() || holePreviewActive") != std::string::npos
				&& schedule.find("previewAreaHeightMm") != std::string::npos
				&& schedule.find("previewAreaHoleMasks") != std::string::npos,
				"The render worker must consume detached all-area height and hole buffers while a brush preview is active.");
			context.Expect(schedule.find("terrainRenderProxyCancellation->store") == std::string::npos,
				"Continuous pointer samples must coalesce behind one worker without cancelling every intermediate preview before it can appear.");
			context.Expect(schedule.find("capture.root->rootContentHash") != std::string::npos
				&& schedule.find("capture.publicationGeneration") != std::string::npos
				&& publish.find("completed.authoredHash == capture.root->rootContentHash") != std::string::npos
				&& publish.find("completed.authoredGeneration == currentRootGeneration") != std::string::npos,
				"Whole-terrain proxies must be accepted against the immutable root identity rather than one selected area descriptor.");
			context.Expect(stamp.find("previewHeightMm = std::move(after)") != std::string::npos
				&& stamp.find("ScheduleRenderProxyBuild();") != std::string::npos,
				"Every accepted height stamp must request a detached render-proxy refresh during the drag.");
			context.Expect(pointer.find("if(released && brushStrokeActive)") != std::string::npos
				&& pointer.find("QueuePreviewForPublication()") != std::string::npos,
				"Mouse release must remain the single durable transaction boundary for the complete stroke.");
		});

		registry.Add("Editor.TerrainSculpt.HistoryReplayRefreshesEveryAuthoredChannel", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp";
			std::ifstream sourceFile(sourcePath, std::ios::binary);
			const std::string source(
				(std::istreambuf_iterator<char>(sourceFile)),
				std::istreambuf_iterator<char>());
			const std::size_t synchronizationBegin = source.find(
				"bool TerrainWindow::SynchronizeCommittedAuthoringStateFromPublishedRoot()");
			const std::size_t synchronizationEnd = synchronizationBegin == std::string::npos
				? std::string::npos : source.find("bool TerrainWindow::StageLayerPatch", synchronizationBegin);
			const std::string synchronization = synchronizationBegin != std::string::npos && synchronizationEnd != std::string::npos
				? source.substr(synchronizationBegin, synchronizationEnd - synchronizationBegin) : std::string{};
			context.Expect(synchronization.find("capture.root->layerStack") != std::string::npos
				&& synchronization.find("descriptor.channels.heightHash") != std::string::npos
				&& synchronization.find("descriptor.channels.holeHash") != std::string::npos
				&& synchronization.find("descriptor.channels.weightGroupHashes.find(1u)") != std::string::npos
				&& synchronization.find("DecodeTerrainWeightSample") != std::string::npos,
				"History replay must rebuild height, holes, surface weights, and layer metadata from the published root.");
			context.Expect(source.find("if(wasHistoryReplay)") != std::string::npos
				&& source.find("(void)SynchronizeCommittedAuthoringStateFromPublishedRoot();") != std::string::npos,
				"A committed terrain undo or redo receipt must refresh every detached editor mirror before the next edit.");
		});

		registry.Add("Editor.TerrainSculpt.BrushFootprintProjectsActiveWorldRadius", [](TestContext& context) {
			const auto readSource = [](const std::filesystem::path& relativePath) {
				std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
				return std::string(
					(std::istreambuf_iterator<char>(input)),
					std::istreambuf_iterator<char>());
			};
			const std::string terrainWindow = readSource(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			const std::string sceneView = readSource(
				"Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp");

			context.Expect(terrainWindow.find("EffectiveRadiusCells(holeBrushSettings.radiusCells)") != std::string::npos
				&& terrainWindow.find(": brushSettings.radiusSamples") != std::string::npos,
				"The visible footprint must use the clamped hole radius in hole mode and the height radius in height mode.");
			context.Expect(terrainWindow.find("std::max(0.001F, radiusUnits) * authoredSampleSpacingMetres") != std::string::npos
				&& terrainWindow.find("worldCenter.position") != std::string::npos,
				"Terrain Tools must expose a canonical centre and metre radius instead of a screen-pixel approximation.");
			context.Expect(sceneView.find("LocalizeForCamera(footprint.center)") != std::string::npos
				&& sceneView.find("footprint.radiusMetres") != std::string::npos
				&& sceneView.find("proj * view") != std::string::npos,
				"Scene View must project the brush footprint through the active camera so zoom preserves terrain coverage.");
			context.Expect(sceneView.find("AddCircle(mouse, terrainTools->BrushSettings().radiusSamples") == std::string::npos,
				"Scene View must not interpret terrain samples as fixed screen pixels.");
		});
	}
}
