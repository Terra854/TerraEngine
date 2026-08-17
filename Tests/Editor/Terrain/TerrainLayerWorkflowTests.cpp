// TerrainLayerWorkflowTests.cpp -- Stable layer metadata and detached paint workflows.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainSurfaceBrush.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainPublication.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
namespace
{
	TerraEngine::Terrain::TerrainLayerStack MakeStack()
	{
		TerraEngine::Terrain::TerrainLayerStack stack;
		stack.generation = 1u;
		stack.layers = {
			{1u, "Grass"}, {2u, "Rock"}, {3u, "Sand"}, {4u, "Snow"}
		};
		return stack;
	}

	std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> MakeRoot(const TerraEngine::Terrain::TerrainLayerStack& stack)
	{
		auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
		root->assetUuid = TerraEngine::GenerateUUID();
		root->publicationGeneration = 1u;
		root->persistedRootGeneration = 1u;
		root->rootContentHash = {11u, 22u};
		root->authoredBounds = {0, 0, 256, 256};
		root->defaultLayerId = 1u;
		root->layerStackGeneration = stack.generation;
		root->layerStack = std::make_shared<const TerraEngine::Terrain::TerrainLayerStack>(stack);
		return root;
	}
}

	void RegisterTerrainLayerWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainLayers.MetadataOperationsAreStableAndReversible", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack base = MakeStack();
			TerrainLayerStack changed;
			TerrainLayer duplicate = base.layers.front(); duplicate.stableId = 5u; duplicate.name = "Grass Copy";
			const TerrainLayerPatch add{TerrainLayerPatchKind::Add, base.generation, 0u, 0xffffffffu, duplicate};
			context.Expect(ApplyTerrainLayerPatch(base, add, changed).Succeeded() && changed.layers.size() == 5u,
				"Adding a layer must retain existing stable IDs and append the new reusable layer.");
			TerrainLayerStack reordered;
			context.Expect(ApplyTerrainLayerPatch(changed, {TerrainLayerPatchKind::Move, changed.generation, 5u, 1u}, reordered).Succeeded()
				&& reordered.layers[1].stableId == 5u,
				"Reordering must change presentation only and retain the duplicate stable ID.");
			TerrainLayerStack disabled;
			context.Expect(ApplyTerrainLayerPatch(reordered, {TerrainLayerPatchKind::SetEnabled, reordered.generation, 2u, 0xffffffffu, {}, false}, disabled).Succeeded()
				&& !disabled.layers[2].enabled,
				"Enable and disable operations must be copy-on-write metadata changes.");
			TerrainLayer replacement = disabled.layers[0]; replacement.name = "Meadow";
			TerrainLayerStack replaced;
			context.Expect(ApplyTerrainLayerPatch(disabled, {TerrainLayerPatchKind::Replace, disabled.generation, replacement.stableId, 0xffffffffu, replacement}, replaced).Succeeded()
				&& replaced.layers[0].stableId == 1u && replaced.layers[0].name == "Meadow",
				"Replacing metadata must retain the target stable ID.");
			TerrainLayerStack removed;
			context.Expect(ApplyTerrainLayerPatch(replaced, {TerrainLayerPatchKind::Remove, replaced.generation, 5u}, removed).Succeeded()
				&& removed.layers.size() == 4u,
				"Removing a layer must be a bounded metadata operation, not a paint rewrite.");
			TerrainLayerStack restored;
			context.Expect(ApplyTerrainLayerPatch(removed, {TerrainLayerPatchKind::Add, removed.generation, 0u, 1u, duplicate}, restored).Succeeded()
				&& restored.layers[1].stableId == 5u,
				"The inverse add must restore a removed layer at its original presentation index.");
		});

		registry.Add("Editor.TerrainLayers.SurfaceBrushPreservesWeightsAcrossMissingSourceAndFifthContributor", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			using namespace TerraEngine::Editor::Terrain;
			const std::array<TerrainWeightContribution, 4u> starting{{{1u, 63u}, {2u, 64u}, {3u, 64u}, {4u, 64u}}};
			TerrainWeightSample sample = NormalizeTerrainWeights(starting);
			const TerrainWeightSample missingSourceBaseline = sample;
			TerrainSurfaceBrushSettings paint; paint.mode = TerrainSurfaceBrushMode::Paint; paint.layerId = 5u; paint.strength = 255u;
			const auto painted = TerrainSurfaceBrush::ApplySample(sample, paint);
			context.Expect(sample.IsCanonical() && painted.changed && painted.displacedLayerId != 0u,
				"Painting a fifth contributor must produce canonical four-slot weights and warn about the displaced stable layer.");
			TerrainSurfaceBrushSettings erase = paint; erase.mode = TerrainSurfaceBrushMode::Erase;
			const auto erased = TerrainSurfaceBrush::ApplySample(sample, erase);
			TerrainSurfaceBrushSettings fill = paint; fill.mode = TerrainSurfaceBrushMode::Fill; fill.layerId = 2u;
			const auto filled = TerrainSurfaceBrush::ApplySample(sample, fill);
			TerrainSurfaceBrushSettings replace = fill; replace.mode = TerrainSurfaceBrushMode::Replace; replace.layerId = 2u; replace.replacementLayerId = 3u;
			const auto replaced = TerrainSurfaceBrush::ApplySample(sample, replace);
			TerrainSurfaceBrushSettings normalize = replace; normalize.mode = TerrainSurfaceBrushMode::Normalize;
			const auto normalized = TerrainSurfaceBrush::ApplySample(sample, normalize);
			context.Expect(erased.changed && filled.changed && replaced.changed && !normalized.changed
				&& sample.IsCanonical() && sample.slots[0].layerId == 3u,
				"Erase, fill, replace, and normalize must all finish with canonical deterministic weights.");
			TerrainWeightSample masked = missingSourceBaseline;
			paint.useLayerMask = true;
			context.Expect(!TerrainSurfaceBrush::ApplySample(masked, paint, 0u).changed && masked.slots == missingSourceBaseline.slots,
				"A zero layer mask must preserve authored weights even while a source is missing.");
			context.Expect(TerrainSurfaceBrush::PreviewDisplacementMm(77u, -9, 12, 1000) == TerrainSurfaceBrush::PreviewDisplacementMm(77u, -9, 12, 1000),
				"Displacement previews must be deterministic for canonical signed coordinates.");
		});

		registry.Add("Editor.TerrainLayers.MetadataHistoryAndSaveReloadStayAtomic", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack stack = MakeStack();
			TerrainAssetStore store(MakeRoot(stack));
			TerrainPublication publication(store);
			const auto capture = store.CaptureRoot();
			TerrainLayerPatch disable{TerrainLayerPatchKind::SetEnabled, capture.root->layerStackGeneration, 2u, 0xffffffffu, {}, false};
			TerrainLayerPatch enable{TerrainLayerPatchKind::SetEnabled, 0u, 2u, 0xffffffffu, {}, true};
			TerrainEditTransaction transaction;
			transaction.transactionId = TerraEngine::GenerateUUID(); transaction.targetAssetUuid = capture.root->assetUuid;
			transaction.inputRootHash = capture.root->rootContentHash; transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
			transaction.affectedBounds = capture.root->authoredBounds; transaction.layerStackPatch = disable; transaction.inverseLayerStackPatch = enable;
			const auto receipt = publication.Publish(std::move(transaction));
			context.Expect(receipt.outcome == TerrainEditOutcome::Committed && receipt.historyPayload
				&& receipt.historyPayload->forwardLayerStackPatch && receipt.historyPayload->inverseLayerStackPatch,
				"A layer operation must commit as one protected root transaction with an explicit inverse history patch.");
			const std::filesystem::path path = std::filesystem::temp_directory_path() / ("TerraEngineLayerWorkflow_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::error_code error;
			const auto saved = store.PersistAtomically({path});
			TerrainAssetStore reloaded(MakeRoot(stack));
			const auto opened = reloaded.OpenPersistedRoot(path);
			const auto loaded = reloaded.CaptureRoot();
			context.Expect(saved.Succeeded() && opened.Succeeded() && loaded.root->layerStack
				&& loaded.root->layerStack->layers.size() == 4u && !loaded.root->layerStack->layers[1].enabled,
				"Saving and reopening a layer root must retain metadata while leaving authored weight blobs untouched.");
			std::filesystem::remove_all(path, error);
		});
	}
}
