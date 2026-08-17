// TerrainProceduralJobTests.cpp -- Detached bounded procedural job contracts.

#include "TestHarness.h"

#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "Terrain/TerrainPublication.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <thread>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		TerraEngine::Terrain::TerrainRootCapture MakeCapture()
		{
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1u; root->rootContentHash = {1u, 2u};
			return {root, root->publicationGeneration};
		}

		TerraEngine::Editor::Terrain::TerrainProceduralJobRequest MakeRequest()
		{
			using namespace TerraEngine::Editor::Terrain;
			TerrainProceduralJobRequest request; request.observedRoot = MakeCapture();
			request.observedArea.coordinate = {0, 0}; request.observedArea.editGeneration = 1u; request.observedArea.authoredContentHash = {3u, 4u};
			request.heightMm.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, 10);
			request.selectionGrid.storageOrigin = {0, 0}; request.selectionGrid.storageWidth = 256u; request.selectionGrid.storageHeight = 256u;
			request.selectionGrid.evaluationBounds = {0, 0, 256, 256}; request.selectionCriteria.maximumEvaluatedSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
			request.kernel = [](std::span<const std::int32_t> source, std::span<std::int32_t> out, std::span<const std::uint8_t> mask, const std::atomic_bool&) {
				for(std::size_t index = 0u; index < source.size(); ++index) out[index] = source[index] + (mask[index] == 0u ? 0 : 1);
				return TerraEngine::Terrain::TerrainResultCode::Ok;
			};
			return request;
		}
	}

	void RegisterTerrainProceduralJobTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainProcedural.EstimateUsesCompleteAreaBatches", [](TestContext& context) {
			auto request = MakeRequest();
			TerraEngine::Editor::Terrain::TerrainProceduralJobArea second;
			second.observedArea = request.observedArea; second.observedArea.coordinate = {1, 0};
			second.heightMm = request.heightMm; second.selectionGrid = request.selectionGrid; second.selectionGrid.storageOrigin = {256, 0}; second.selectionGrid.evaluationBounds = {256, 0, 512, 256};
			request.additionalAreas.push_back(std::move(second)); request.maximumAreas = 2u;
			const auto estimate = TerraEngine::Editor::Terrain::TerrainProceduralJobs{}.Estimate(request);
			context.Expect(estimate.areas == 2u && estimate.kernelBatches == 2u
				&& estimate.samples == 2u * TerraEngine::Terrain::kTerrainAreaSampleCount,
				"Estimates must expose complete-area batches; erosion and spline work are never incorrectly sample-sliced.");
		});

		registry.Add("Editor.TerrainProcedural.CancellationCoalescingHasNoPublishablePreview", [](TestContext& context) {
			ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(1u);
			auto request = MakeRequest(); std::atomic_bool entered = false;
			request.kernel = [&entered](std::span<const std::int32_t>, std::span<std::int32_t>, std::span<const std::uint8_t>, const std::atomic_bool& cancelled) {
				entered.store(true, std::memory_order_release);
				while(!cancelled.load(std::memory_order_acquire)) std::this_thread::yield();
				return TerraEngine::Terrain::TerrainResultCode::Cancelled;
			};
			TerraEngine::Editor::Terrain::TerrainProceduralJobs jobs; std::string error;
			const bool started = jobs.Start(std::move(request), error);
			for(std::size_t wait = 0u; wait < 500u && !entered.load(std::memory_order_acquire); ++wait) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const auto before = std::chrono::steady_clock::now(); const auto progress = jobs.Progress(); jobs.Cancel("test cancellation");
			const auto responsive = std::chrono::steady_clock::now() - before;
			std::optional<TerraEngine::Editor::Terrain::TerrainProceduralPreview> preview;
			for(std::size_t wait = 0u; wait < 500u && !preview; ++wait) { preview = jobs.TakeReadyPreview(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			ThreadPool::instance().shutdown();
			context.Expect(started && entered.load(std::memory_order_acquire) && responsive < std::chrono::milliseconds(100)
				&& !preview && (progress.stage == TerraEngine::Editor::Terrain::TerrainProceduralStage::RunningKernel || progress.stage == TerraEngine::Editor::Terrain::TerrainProceduralStage::Queued),
				"Cancellation must remain camera-responsive and a cancelled worker must never expose a partial preview for publication.");
		});

		registry.Add("Editor.TerrainProcedural.ExactMultiAreaPreviewPublishesOnlyAtProtectedBoundary", [](TestContext& context) {
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 7);
			std::vector<std::uint8_t> bytes(heights.size() * sizeof(std::int32_t)); std::memcpy(bytes.data(), heights.data(), bytes.size());
			const auto hash = TerraEngine::Terrain::HashTerrainBytes(bytes);
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 1u; root->rootContentHash = {9u, 10u}; root->authoredBounds = {0, 0, 256, 256}; root->layerStackGeneration = 1u;
			TerraEngine::Terrain::TerrainAreaDescriptor descriptor; descriptor.coordinate = {0, 0}; descriptor.editGeneration = 1u; descriptor.authoredContentHash = hash; descriptor.channels.heightHash = hash;
			TerraEngine::Terrain::TerrainDirectoryShard shard; shard.areaBounds = root->authoredBounds; shard.identity = {11u, 12u}; shard.descriptors.emplace(descriptor.coordinate, descriptor); root->directoryShards.push_back(std::move(shard));
			TerraEngine::Terrain::TerrainAssetStore store(root); store.Blobs().PutVerified(hash, bytes);
			TerraEngine::Editor::Terrain::TerrainProceduralPreview preview;
			preview.observedRoot = store.CaptureRoot(); preview.observedArea = descriptor; preview.beforeHeightMm = heights; preview.afterHeightMm = heights; preview.afterHeightMm[0] += 5; preview.historyGroup = "Terrain procedural test group";
			TerraEngine::Editor::Terrain::TerrainProceduralJobs jobs; std::string error;
			auto transaction = jobs.BuildTransaction(store, preview, error);
			TerraEngine::Terrain::TerrainMutationQueue queue; TerraEngine::Terrain::TerrainPublication publication(store);
			TerraEngine::Editor::Terrain::TerrainAuthoringSystem authoring; authoring.Configure(queue, publication); authoring.Initialize();
			const auto generationBefore = store.CaptureRoot().publicationGeneration;
			const bool queued = transaction && authoring.EnqueueProceduralCommit(std::move(*transaction), error);
			const bool detached = store.CaptureRoot().publicationGeneration == generationBefore;
			authoring.PublishAtProtectedBoundary(); const auto receipts = authoring.DrainReceipts();
			context.Expect(queued && detached && receipts.size() == 1u && receipts.front().outcome == TerraEngine::Terrain::TerrainEditOutcome::Committed
				&& store.CaptureRoot().publicationGeneration > generationBefore,
				"An exact procedural transaction must remain detached until the protected boundary, then publish as one receipt.");
		});
	}
}
