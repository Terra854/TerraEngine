// TerrainResizeWorkflowTests.cpp -- Detached resize preview and job regressions.

#include "TestHarness.h"
#include "TerrainResizeTestFixtures.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainResizeWorkflowTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainResize.ExpansionPreviewMaterializesUnevenTransition", [](TestContext& context) {
			auto mutableRoot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(
				*TerraEngine::Tests::TerrainResizeFixtures::MakeRoot({0, 0, 128, 256}));
			mutableRoot->defaultHeightMm = 0;
			const std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> root = mutableRoot;
			auto request = TerraEngine::Tests::TerrainResizeEditorFixtures::MakeExpansionRequest(root, {0, 0, 384, 256});
			request.sourceHeight = [](TerraEngine::Terrain::TerrainSampleCoordinate sample) -> std::optional<std::int32_t> {
				// Values beyond the old partial edge deliberately simulate hidden padded
				// storage left by earlier authoring. Expansion must never reveal it.
				return sample.x < 128 ? static_cast<std::int32_t>(10'000 + sample.x * 2 + sample.z) : 999'999;
			};
			TerraEngine::Editor::Terrain::TerrainResizeJobRequest::AreaSource origin{};
			origin.descriptor = mutableRoot->directoryShards.front().descriptors.begin()->second;
			origin.heightMm.resize(TerraEngine::Terrain::kTerrainAreaSampleCount, 999'999);
			for(std::uint32_t z = 0u; z < 256u; ++z)
				for(std::uint32_t x = 0u; x < 128u; ++x)
					origin.heightMm[static_cast<std::size_t>(z) * 256u + x]
						= static_cast<std::int32_t>(10'000 + x * 2u + z);
			request.sourceAreas.emplace(TerraEngine::Terrain::TerrainAreaCoordinate{0, 0}, std::move(origin));
			TerraEngine::Editor::Terrain::TerrainResizeJobs jobs;
			std::string error;
			context.Expect(jobs.Start(std::move(request), error), "A valid captured expansion must start detached preparation.");
			std::optional<TerraEngine::Editor::Terrain::TerrainPreparedResize> ready;
			// The shared test process may already have task-system work in flight. Poll
			// with a bounded deadline instead of blocking the authoring boundary.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while(!ready && std::chrono::steady_clock::now() < deadline)
			{
				ready = jobs.TakeReadyPreview();
				if(!ready) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			context.Expect(ready.has_value(), "The detached expansion job must reach a ready preview.");
			context.Expect(ready && ready->preview.Succeeded(), "The ready expansion preview must remain valid after boundary inspection.");
			context.Expect(ready && ready->preview.addedAreaCount == 1u, "The fixture must add exactly one terrain area.");
			context.Expect(ready && !ready->preview.delta.descriptorUpserts.empty() && !ready->preparedBlobs.empty(),
				"Edge Blend must materialize its non-default uneven transition while leaving default-only distant coverage sparse.");
			if(ready)
			{
				const auto descriptor = std::find_if(ready->preview.delta.descriptorUpserts.begin(),
					ready->preview.delta.descriptorUpserts.end(), [](const auto& value) { return value.coordinate == TerraEngine::Terrain::TerrainAreaCoordinate{0, 0}; });
				const auto blob = descriptor == ready->preview.delta.descriptorUpserts.end() || !descriptor->channels.heightHash
					? ready->preparedBlobs.end()
					: std::find_if(ready->preparedBlobs.begin(), ready->preparedBlobs.end(), [&](const auto& value) {
						return value.first == *descriptor->channels.heightHash;
					});
				std::int32_t formerlyHidden = 999'999;
				if(blob != ready->preparedBlobs.end() && blob->second.size() == TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t))
					std::memcpy(&formerlyHidden, blob->second.data() + (64u * 256u + 200u) * sizeof(std::int32_t), sizeof(std::int32_t));
				context.Expect(formerlyHidden != 999'999,
					"Growing a partial boundary area must replace hidden padded samples with policy output.");
			}
			context.Expect(ready && ready->preview.proposalFingerprint.IsValid(),
				"The ready resize proposal must carry a root-bound approval fingerprint.");
		});

		registry.Add("Editor.TerrainResize.StalePreviewAndCancellationStayDetached", [](TestContext& context) {
			auto root = TerraEngine::Tests::TerrainResizeFixtures::MakeRoot();
			auto request = TerraEngine::Tests::TerrainResizeEditorFixtures::MakeExpansionRequest(root, {-256, 0, 256, 256});
			TerraEngine::Editor::Terrain::TerrainResizeJobs jobs;
			std::string error;
			context.Expect(jobs.Start(std::move(request), error), "The cancellable resize fixture must start.");
			jobs.Cancel("test cancellation");
			context.Expect(jobs.Progress().stage == TerraEngine::Editor::Terrain::TerrainResizeStage::Cancelled,
				"Cancellation must remain a detached job state and publish no partial root.");

			TerraEngine::Terrain::TerrainAssetStore store(root);
			TerraEngine::Editor::Terrain::TerrainPreparedResize prepared{};
			prepared.observedRoot = {root, root->publicationGeneration};
			prepared.preview.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Expand;
			prepared.preview.delta.previousBounds = root->authoredBounds;
			prepared.preview.delta.proposedBounds = {0, 0, 512, 256};
			prepared.preview.proposedAreaCount = 2u;
			prepared.preview.proposalFingerprint = {7u, 9u};
			auto candidate = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(*root);
			candidate->publicationGeneration = 2u;
			candidate->rootContentHash = {90u, 91u};
			context.Expect(store.PublishCandidate(root, candidate).Succeeded(), "The fixture must advance the live root.");
			context.Expect(!jobs.BuildTransaction(store, prepared, false, error),
				"A root change after preview must reject the resize transaction atomically.");
		});

		registry.Add("Editor.TerrainResize.CropPreviewCountsExactDiscardedAuthoredContent", [](TestContext& context) {
			auto mutableRoot = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>(
				*TerraEngine::Tests::TerrainResizeFixtures::MakeRoot({0, 0, 512, 256}));
			auto descriptor = TerraEngine::Tests::TerrainResizeFixtures::MakeDescriptor({1, 0}, 3u);
			mutableRoot->directoryShards.front().descriptors.emplace(descriptor.coordinate, descriptor);
			const std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> root = mutableRoot;
			TerraEngine::Editor::Terrain::TerrainResizeJobRequest request{};
			request.observedRoot = {root, root->publicationGeneration};
			request.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Crop;
			request.delta.previousBounds = root->authoredBounds;
			request.delta.proposedBounds = {0, 0, 300, 256};
			TerraEngine::Editor::Terrain::TerrainResizeJobRequest::AreaSource source{};
			source.descriptor = descriptor;
			source.heightMm.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, 42);
			source.holeBits.assign(TerraEngine::Terrain::kTerrainAreaSampleCount / 8u, 0u);
			const std::size_t discardedHole = 60u;
			source.holeBits[discardedHole / 8u] |= static_cast<std::uint8_t>(1u << (discardedHole & 7u));
			const std::array<TerraEngine::Terrain::TerrainWeightContribution, 1u> fallback{{{root->defaultLayerId, 255u}}};
			const std::array<TerraEngine::Terrain::TerrainWeightContribution, 1u> painted{{{9u, 255u}}};
			source.surfaceGroups[1u].assign(TerraEngine::Terrain::kTerrainAreaSampleCount,
				TerraEngine::Terrain::NormalizeTerrainWeights(fallback));
			source.surfaceGroups[1u][70u] = TerraEngine::Terrain::NormalizeTerrainWeights(painted);
			request.sourceAreas.emplace(descriptor.coordinate, std::move(source));
			TerraEngine::Editor::Terrain::TerrainResizeJobs jobs;
			std::string error;
			context.Expect(jobs.Start(std::move(request), error), "A bounded exact crop preview must start.");
			std::optional<TerraEngine::Editor::Terrain::TerrainPreparedResize> ready;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while(!ready && std::chrono::steady_clock::now() < deadline)
			{
				ready = jobs.TakeReadyPreview();
				if(!ready) std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			const std::uint64_t discardedFromPartial = (256u - 44u) * 256u;
			context.Expect(ready && ready->preview.cropLoss.discardedSamples == 212u * 256u
				&& ready->preview.cropLoss.nonDefaultHeightSamples == discardedFromPartial
				&& ready->preview.cropLoss.holeSamples == 1u
				&& ready->preview.cropLoss.paintedSamples == 1u
				&& ready->preview.cropLoss.partialDescriptors == 1u
				&& std::find(ready->preview.delta.forcedRetirements.begin(), ready->preview.delta.forcedRetirements.end(),
					TerraEngine::Terrain::TerrainAreaCoordinate{1, 0}) != ready->preview.delta.forcedRetirements.end(),
				"Crop preview must count discarded authored channels exactly and force retirement of changed partial coverage.");
			if(ready)
			{
				TerraEngine::Terrain::TerrainAssetStore store(root);
				context.Expect(!jobs.BuildTransaction(store, *ready, false, error),
					"A dedicated data-loss acknowledgement must be required after the exact crop summary.");
				context.Expect(jobs.BuildTransaction(store, *ready, true, error).has_value(),
					"The same current fingerprint may build one atomic crop after explicit acknowledgement.");
			}
		});
	}
}
