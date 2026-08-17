// TerrainSculptWorkflowTests.cpp -- Detached preview, picking, and transaction checks.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSession.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainPicking.h"
#include "Editor/TerraEngine-Editor/Windows/TerrainWindow.h"

#include "Terrain/TerrainPublication.h"

#include <limits>
#include <set>

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

		registry.Add("Editor.TerrainSculpt.SelectionRestoresAfterStrokeRelease", [](TestContext& context) {
			context.Expect(TerrainWindow::ShouldCaptureSceneSelection(true, true)
				&& !TerrainWindow::ShouldCaptureSceneSelection(true, false)
				&& !TerrainWindow::ShouldCaptureSceneSelection(false, true),
				"Terrain capture must take priority only during an active stroke so ordinary Scene View selection restores on release.");
		});
	}
}
