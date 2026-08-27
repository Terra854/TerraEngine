// TerrainLayerWorkflowTests.cpp -- Stable layer metadata and detached paint workflows.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainSurfaceBrush.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainLayerStack.h"
#include "Terrain/TerrainPublication.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
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

	TerraEngine::Terrain::TerrainLayerStack MakeMaximumLayerStack()
	{
		// The persisted root may keep sixty-four definitions, while only the first
		// sixteen are enabled and therefore eligible for complete runtime publication.
		TerraEngine::Terrain::TerrainLayerStack stack;
		stack.generation = 7u;
		stack.layers.reserve(TerraEngine::Terrain::Format::kTerrainMaximumLayers);
		for(std::uint16_t stableId = 1u;
			stableId <= TerraEngine::Terrain::Format::kTerrainMaximumLayers;
			++stableId)
		{
			TerraEngine::Terrain::TerrainLayer layer;
			layer.stableId = stableId;
			layer.name = "Layer " + std::to_string(stableId);
			layer.enabled = stableId <= TerraEngine::Terrain::Format::kTerrainMaximumEnabledLayers;
			stack.layers.push_back(std::move(layer));
		}
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

	std::string ReadTerrainEditorSource(const std::filesystem::path& relativePath)
	{
		// Source-contract checks keep the editor interaction requirement executable
		// before the mapping-preview API exists, while ordinary value tests below
		// continue to cover the transaction data without constructing ImGui state.
		std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath, std::ios::binary);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}
}

	void RegisterTerrainLayerWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainLayers.MaximumDefinitionAndEnabledCountsAreAccepted", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack stack = MakeMaximumLayerStack();
			const auto validation = ValidateTerrainLayerStack(stack);
			const std::size_t enabledCount = static_cast<std::size_t>(std::count_if(
				stack.layers.begin(), stack.layers.end(),
				[](const TerrainLayer& layer) { return layer.enabled; }));
			context.Expect(validation.Succeeded()
				&& stack.layers.size() == Format::kTerrainMaximumLayers
				&& enabledCount == Format::kTerrainMaximumEnabledLayers,
				"A TERRAROOT1 layer stack must accept all sixty-four definitions with the complete sixteen-layer enabled set.");
		});

		registry.Add("Editor.TerrainLayers.EnabledLimitRejectionsAreAtomic", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack maximum = MakeMaximumLayerStack();
			TerrainAssetStore maximumStore(MakeRoot(maximum));
			const auto maximumBefore = maximumStore.CaptureRoot();
			std::shared_ptr<const TerrainAssetRoot> rejectedCandidate;
			const TerrainLayerPatch enableSeventeenth{
				TerrainLayerPatchKind::SetEnabled, maximum.generation,
				static_cast<std::uint16_t>(Format::kTerrainMaximumEnabledLayers + 1u),
				0xffffffffu, {}, true};
			const auto seventeenth = maximumStore.BuildLayerStackCandidate(
				*maximumBefore.root, enableSeventeenth, {}, rejectedCandidate);
			const auto maximumAfter = maximumStore.CaptureRoot();
			context.Expect(!seventeenth.Succeeded() && !rejectedCandidate
				&& maximumAfter.root == maximumBefore.root
				&& maximumAfter.publicationGeneration == maximumBefore.publicationGeneration,
				"Enabling a seventeenth layer must reject the whole candidate and leave the published immutable root active.");

			TerrainLayerStack oneEnabled = MakeStack();
			for(std::size_t index = 1u; index < oneEnabled.layers.size(); ++index)
				oneEnabled.layers[index].enabled = false;
			TerrainAssetStore oneEnabledStore(MakeRoot(oneEnabled));
			const auto oneEnabledBefore = oneEnabledStore.CaptureRoot();
			const TerrainLayerPatch disableFinal{
				TerrainLayerPatchKind::SetEnabled, oneEnabled.generation,
				oneEnabled.layers.front().stableId, 0xffffffffu, {}, false};
			const auto finalDisable = oneEnabledStore.BuildLayerStackCandidate(
				*oneEnabledBefore.root, disableFinal, {}, rejectedCandidate);
			const auto oneEnabledAfter = oneEnabledStore.CaptureRoot();
			context.Expect(!finalDisable.Succeeded() && !rejectedCandidate
				&& oneEnabledAfter.root == oneEnabledBefore.root
				&& oneEnabledAfter.root->layerStack->layers.front().enabled,
				"Disabling the final enabled layer must reject atomically without publishing a stack that cannot render.");
		});

		registry.Add("Editor.TerrainLayers.ReorderPreservesStablePaintIdentity", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			TerrainLayerStack base = MakeStack();
			for(auto& layer : base.layers) layer.materialAssetUuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID paintedMaterial = base.layers[2].materialAssetUuid;
			TerrainLayerStack reordered;
			const auto moved = ApplyTerrainLayerPatch(base,
				{TerrainLayerPatchKind::Move, base.generation, 3u, 0u}, reordered);
			const auto stableLayer = std::find_if(reordered.layers.begin(), reordered.layers.end(),
				[](const TerrainLayer& layer) { return layer.stableId == 3u; });
			TerrainWeightSample painted{};
			painted.slots[0] = {3u, 255u};
			const auto resolved = ResolveTerrainWeightSample(painted, reordered);
			context.Expect(moved.Succeeded() && stableLayer == reordered.layers.begin()
				&& stableLayer->materialAssetUuid == paintedMaterial
				&& resolved.contributorCount == 1u
				&& resolved.contributors[0].stableLayerId == 3u,
				"Presentation reordering must preserve the stable material identity used by existing painted control samples.");
		});

		registry.Add("Editor.TerrainLayers.InvalidIdentityNameAndMappingAreRejected", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack base = MakeStack();
			TerrainLayerStack duplicateId = base;
			duplicateId.layers[1].stableId = duplicateId.layers[0].stableId;
			TerrainLayerStack duplicateNormalizedName = base;
			duplicateNormalizedName.layers[1].name = "gRaSs";
			TerrainLayerStack invalidMapping = base;
			invalidMapping.layers[2].mapping.mode = static_cast<TerrainLayerMappingMode>(255u);
			invalidMapping.layers[2].mapping.tileSizeXMetres = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!ValidateTerrainLayerStack(duplicateId).Succeeded()
				&& !ValidateTerrainLayerStack(duplicateNormalizedName).Succeeded(),
				"Duplicate stable IDs and case-normalized names must reject before changing presentation order or paint identity.");
			context.Expect(!ValidateTerrainLayerStack(invalidMapping).Succeeded(),
				"Unknown mapping enums and non-finite physical mapping values must reject before material publication.");
		});

		registry.Add("Editor.TerrainLayers.ValidationFeedbackNamesTheRecoveryAction", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			TerrainLayerStack maximum = MakeMaximumLayerStack();
			TerrainLayerStack ignored;
			const auto seventeenth = ApplyTerrainLayerPatch(maximum,
				{TerrainLayerPatchKind::SetEnabled, maximum.generation, 17u, 0xffffffffu, {}, true}, ignored);
			TerrainLayerStack duplicateName = MakeStack();
			duplicateName.layers[1].name = "grass";
			const auto identity = ValidateTerrainLayerStack(duplicateName);
			TerrainLayerStack invalidMapping = MakeStack();
			invalidMapping.layers[0].mapping.tileSizeXMetres = 0.0;
			const auto mapping = ValidateTerrainLayerStack(invalidMapping);
			const std::string window = ReadTerrainEditorSource(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			context.Expect(seventeenth.message.find("sixteen") != std::string::npos
				&& seventeenth.message.find("enabled") != std::string::npos
				&& identity.message.find("unique") != std::string::npos
				&& mapping.message.find("mapping") != std::string::npos,
				"Rejected layer edits must explain the enabled limit, identity collision, or physical mapping problem to the artist.");
			context.Expect(window.find("outError = result.message;") != std::string::npos
				&& window.find("status = std::move(error);") != std::string::npos,
				"The Layers inspector must forward validation feedback into its visible status instead of silently dropping a rejected edit.");
		});

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

		registry.Add("Editor.TerrainLayers.SurfaceBrushStampIsBoundedAndSpacingAware", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			using namespace TerraEngine::Editor::Terrain;
			std::vector<TerrainWeightSample> source(64u, NormalizeTerrainWeights(
				std::array<TerrainWeightContribution, 1u>{{{1u, 255u}}}));
			std::vector<TerrainWeightSample> painted(source.size());
			TerrainSurfaceBrushSettings settings;
			settings.mode = TerrainSurfaceBrushMode::Paint;
			settings.layerId = 2u;
			settings.strength = 255u;
			settings.radiusSamples = 2.0F;
			settings.spacingSamples = 1.5F;
			const auto result = TerrainSurfaceBrush::ApplyStamp(
				source, painted, 8u, 8u, settings, {3.0F, 3.0F});
			context.Expect(result.changed && painted[3u * 8u + 3u].slots != source[3u * 8u + 3u].slots
				&& painted.front().slots == source.front().slots,
				"A Scene View surface stamp must change its centre while preserving samples outside the circular footprint.");
			context.Expect(!TerrainSurfaceBrush::IsStrokeSampleDue({3.0F, 3.0F}, {4.0F, 3.0F}, settings.spacingSamples)
				&& TerrainSurfaceBrush::IsStrokeSampleDue({3.0F, 3.0F}, {5.0F, 3.0F}, settings.spacingSamples),
				"Surface stroke spacing must suppress dense pointer samples and accept the next due stamp deterministically.");
		});

		registry.Add("Editor.TerrainLayers.PaintSelectionAlwaysResolvesToAnEnabledStableLayer", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			auto stack = MakeStack();
			stack.layers[0].enabled = false;
			context.Expect(TerrainSurfaceBrush::ResolveEnabledLayerId(stack, 3u) == 3u,
				"A valid selected paint layer must retain its stable ID.");
			context.Expect(TerrainSurfaceBrush::ResolveEnabledLayerId(stack, 1u) == 2u
				&& TerrainSurfaceBrush::ResolveEnabledLayerId(stack, 0u) == 2u,
				"A disabled or unset paint layer must fall back to the first enabled stable ID.");
			for(auto& layer : stack.layers) layer.enabled = false;
			context.Expect(TerrainSurfaceBrush::ResolveEnabledLayerId(stack, 3u) == 0u,
				"A stack with no enabled layers must explicitly report that painting is unavailable.");
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

		registry.Add("Editor.TerrainLayers.MappingPreviewApplyCancelStaysDetached", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			const TerrainLayerStack committed = MakeStack();
			TerrainLayer replacement = committed.layers.front();
			replacement.mapping.tileSizeXMetres = 6.0;
			replacement.mapping.tileSizeZMetres = 9.0;
			replacement.mapping.offsetXMetres = -2.5;
			replacement.mapping.offsetZMetres = 13.0;
			replacement.mapping.rotationRadians = 0.75;

			TerrainLayerPatch forward;
			forward.kind = TerrainLayerPatchKind::Replace;
			forward.expectedGeneration = committed.generation;
			forward.targetStableId = replacement.stableId;
			forward.layer = replacement;
			TerrainLayerStack preview;
			const auto staged = ApplyTerrainLayerPatch(committed, forward, preview);
			context.Expect(staged.Succeeded()
				&& committed.layers.front().mapping.tileSizeXMetres == 4.0
				&& preview.layers.front().mapping.tileSizeXMetres == 6.0,
				"A mapping drag must change a detached complete layer value without mutating the committed stack.");

			// Cancel is intentionally just disposal of the detached candidate. The
			// authoritative root must remain byte-for-byte unchanged until Apply queues
			// the typed replacement at the protected publication boundary.
			TerrainAssetStore cancelledStore(MakeRoot(committed));
			const auto cancelledBefore = cancelledStore.CaptureRoot();
			preview = committed;
			const auto cancelledAfter = cancelledStore.CaptureRoot();
			context.Expect(preview.layers.front().mapping.tileSizeXMetres == 4.0
				&& cancelledAfter.root->rootContentHash == cancelledBefore.root->rootContentHash
				&& cancelledAfter.publicationGeneration == cancelledBefore.publicationGeneration,
				"Cancel must discard the mapping candidate without publishing a root generation.");

			TerrainAssetStore appliedStore(MakeRoot(committed));
			TerrainPublication publication(appliedStore);
			const auto beforeApply = appliedStore.CaptureRoot();
			TerrainLayerPatch inverse = forward;
			inverse.expectedGeneration = 0u;
			inverse.layer = committed.layers.front();
			TerrainEditTransaction transaction;
			transaction.transactionId = TerraEngine::GenerateUUID();
			transaction.origin = TerrainEditOrigin::VisibleEditor;
			transaction.targetAssetUuid = beforeApply.root->assetUuid;
			transaction.historyGroup = "Terrain mapping edit";
			transaction.inputRootHash = beforeApply.root->rootContentHash;
			transaction.expectedLayerStackGeneration = beforeApply.root->layerStackGeneration;
			transaction.affectedBounds = beforeApply.root->authoredBounds;
			transaction.layerStackPatch = forward;
			transaction.inverseLayerStackPatch = inverse;
			const auto receipt = publication.Publish(std::move(transaction));
			const auto published = appliedStore.CaptureRoot();
			context.Expect(receipt.outcome == TerrainEditOutcome::Committed
				&& receipt.historyPayload && receipt.historyPayload->forwardLayerStackPatch
				&& receipt.historyPayload->inverseLayerStackPatch
				&& published.root->layerStack->layers.front().mapping.tileSizeZMetres == 9.0,
				"Apply must publish one mapping replacement and return the exact forward/inverse receipt needed for undo.");
		});

		registry.Add("Editor.TerrainLayers.MappingInspectorCommitsOnApplyOrEditDeactivation", [](TestContext& context) {
			const std::string window = ReadTerrainEditorSource(
				"Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp");
			const std::size_t stageStart = window.find("bool TerrainWindow::StageLayerPatch");
			const std::size_t applyStart = window.find("bool TerrainWindow::ApplyLayerPreview", stageStart);
			const std::string stage = stageStart != std::string::npos && applyStart != std::string::npos
				? window.substr(stageStart, applyStart - stageStart) : std::string{};
			context.Expect(!stage.empty()
				&& stage.find("previewLayerStack") != std::string::npos
				&& stage.find("EnqueueDetachedCommit") == std::string::npos
				&& stage.find("ScheduleRenderProxyBuild") == std::string::npos,
				"Mapping widgets must stage only detached layer state; preview changes cannot publish or rebuild geometry.");
			context.Expect(window.find("ImGui::IsItemDeactivatedAfterEdit()") != std::string::npos
				&& window.find("ApplyLayerPreview") != std::string::npos
				&& window.find("CancelLayerPreview") != std::string::npos,
				"The mapping inspector must offer Apply/Cancel and commit a completed drag exactly once when its item deactivates.");
		});
	}
}
