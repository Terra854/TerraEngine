// TerrainFailureContainmentTests.cpp -- End-to-end cancellation and LKG gates.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Terrain/TerrainDiagnostics.h"
#include "Terrain/TerrainPublication.h"
#include "Terrain/TerrainStreamingSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::shared_ptr<TerraEngine::Terrain::TerrainAssetRoot> MakeFailureRoot()
		{
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1u;
			root->rootContentHash = {31u, 32u};
			root->authoredBounds = {0, 0, 256, 256};
			root->layerStackGeneration = 1u;
			return root;
		}
	}

	void RegisterTerrainFailureContainmentTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainFailureContainment.LifecycleBoundariesCancelBeforeVisibility", [](TestContext& context) {
			const std::array<std::string_view, 5u> boundaries{
				"project close", "scene switch", "play transition", "device loss", "device recovery"};
			bool allContained = true;
			for(std::size_t run = 0u; run < 100u; ++run)
			for(const auto boundary : boundaries)
			{
				auto root = MakeFailureRoot();
				TerraEngine::Terrain::TerrainAssetStore store(root);
				TerraEngine::Terrain::TerrainMutationQueue queue;
				TerraEngine::Terrain::TerrainPublication publication(store);
				TerraEngine::Editor::Terrain::TerrainAuthoringSystem authoring;
				authoring.Configure(queue, publication);
				authoring.Initialize();
				TerraEngine::Terrain::TerrainEditTransaction transaction;
				transaction.transactionId = TerraEngine::GenerateUUID();
				transaction.targetAssetUuid = root->assetUuid;
				transaction.inputRootHash = root->rootContentHash;
				transaction.expectedLayerStackGeneration = root->layerStackGeneration;
				transaction.affectedBounds = root->authoredBounds;
				transaction.budget.maximumAreas = 1u;
				std::string error;
				const bool queued = authoring.EnqueueDetachedCommit(std::move(transaction), error);
				const auto cancelled = authoring.CancelPreparedWork(boundary);
				authoring.PublishAtProtectedBoundary();
				const auto receipts = authoring.DrainReceipts();
				allContained = allContained && queued && cancelled == 1u && receipts.size() == 1u
					&& receipts.front().outcome == TerraEngine::Terrain::TerrainEditOutcome::Cancelled
					&& store.CaptureRoot().publicationGeneration == 1u;
			}
			context.Expect(allContained,
				"Project, scene, play, device-loss, and recovery boundaries must cancel detached work before it can become visible.");
		});

		registry.Add("Editor.TerrainFailureContainment.CorruptDerivedKeepsLastKnownGoodCollision", [](TestContext& context) {
			auto root = MakeFailureRoot();
			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Physics::TerrainHeightFieldRecipe recipe;
			recipe.terrainAssetId = root->assetUuid;
			recipe.authoredGeneration = root->publicationGeneration;
			recipe.authoredHash = root->rootContentHash.low64;
			recipe.heightMillimetres.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(TerraEngine::Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			auto current = TerraEngine::Physics::Terrain::PrepareTerrainCollisionCandidate(
				recipe, root->rootContentHash, root->publicationGeneration);
			TerraEngine::Physics::Terrain::TerrainCollisionSnapshot snapshot;
			const bool published = TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, current, store);
			bool allRejected = true;
			for(std::size_t run = 0u; run < 100u; ++run)
			{
				auto corrupt = current;
				corrupt.recipe.heightMillimetres.clear();
				allRejected = allRejected
					&& !TerraEngine::Physics::Terrain::PublishTerrainCollisionCandidate(snapshot, corrupt, store)
					&& snapshot.currentRecipe.IsComplete();
				std::atomic_bool cancelled = true;
				TerraEngine::Terrain::TerrainResidencyBudget budget{};
				const auto residency = TerraEngine::Terrain::PrepareTerrainResidencyPlan(
					root->publicationGeneration, {}, {}, budget, &cancelled);
				TerraEngine::Renderer::Terrain::TerrainDirectMeshInput renderInput{};
				renderInput.assetId = root->assetUuid;
				renderInput.authoredHash = root->rootContentHash;
				renderInput.authoredGeneration = root->publicationGeneration;
				std::vector<std::int32_t> renderHeights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
				renderInput.heightMillimetres = renderHeights;
				const auto renderer = TerraEngine::Renderer::Terrain::BuildTerrainDirectMesh(
					renderInput, &cancelled);
				allRejected = allRejected && residency.cancelled && renderer.cancelled;
			}
			context.Expect(published && allRejected
				&& snapshot.status == TerraEngine::Physics::Terrain::TerrainCollisionStatus::Failed
				&& snapshot.currentRecipe.IsComplete(),
				"A corrupt derived collision candidate must fail atomically while the complete last-known-good shape remains queryable.");
		});

		registry.Add("Editor.TerrainFailureContainment.MissingLayerSourceIsVisibleAndNonDestructive", [](TestContext& context) {
			TerraEngine::Terrain::TerrainDiagnosticBuildInput input;
			input.snapshot.revision = 1u;
			input.snapshot.layerFallbacks.push_back({7u, TerraEngine::GenerateUUID(),
				TerraEngine::Terrain::TerrainLayerFallbackState::MissingSource,
				"Layer 7 keeps authored weights while its material source is missing."});
			const auto snapshot = TerraEngine::Terrain::BuildTerrainDiagnosticSnapshot(std::move(input));
			const bool reloadAction = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const auto& item) {
				return item.category == TerraEngine::Terrain::TerrainDiagnosticCategory::Source
					&& item.action == TerraEngine::Terrain::TerrainDiagnosticAction::ReloadSource;
			});
			context.Expect(snapshot.layerFallbacks.size() == 1u
				&& snapshot.layerFallbacks.front().stableLayerId == 7u && reloadAction,
				"A missing layer source must retain its stable authored identity and publish a concrete reload action.");
		});
	}
}
