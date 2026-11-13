// ProceduralPreviewCancellationTests.cpp -- Cancel after worker completion, before publication.
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <thread>

namespace TerraEngine::EditorTests
{
	void RegisterProceduralPreviewCancellationTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainProcedural.ReadyPreviewCancelledBeforeTakeIsRejected", [](TestContext& context) {
			using namespace TerraEngine::Editor::Terrain;
			ThreadPool::instance().shutdown();
			ThreadPool::instance().initialize(1u);
			TerrainProceduralJobs jobs;
			// Cancellation precedes pool shutdown during assertion unwinding too.
			struct Cleanup
			{
				TerrainProceduralJobs& jobs;
				~Cleanup() { jobs.Cancel("test cleanup"); ThreadPool::instance().shutdown(); }
			} cleanup{jobs};
			auto root = std::make_shared<TerraEngine::Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 1u;
			root->rootContentHash = {1u, 2u};
			TerrainProceduralJobRequest request;
			request.observedRoot = {root, root->publicationGeneration};
			request.observedArea.coordinate = {0, 0};
			request.observedArea.editGeneration = 1u;
			request.observedArea.authoredContentHash = {3u, 4u};
			request.heightMm.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, 10);
			request.selectionGrid.storageOrigin = {0, 0};
			request.selectionGrid.storageWidth = request.selectionGrid.storageHeight = 256u;
			request.selectionGrid.evaluationBounds = {0, 0, 256, 256};
			request.selectionCriteria.maximumEvaluatedSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
			request.kernel = [](auto source, auto output, auto, const std::atomic_bool&) {
				for(std::size_t index = 0; index < source.size(); ++index) output[index] = source[index] + 1;
				return TerraEngine::Terrain::TerrainResultCode::Ok;
			};
			std::string error;
			context.Expect(jobs.Start(std::move(request), error), "The cancellation fixture must start a real procedural job.");
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while(jobs.Progress().stage != TerrainProceduralStage::ReadyPreview && std::chrono::steady_clock::now() < deadline)
				std::this_thread::yield();
			context.Expect(jobs.Progress().stage == TerrainProceduralStage::ReadyPreview,
				"The bounded watchdog must observe a completed detached preview before cancellation.");
			// With one worker, this barrier cannot run until the preview's promise is
			// settled. It closes the ReadyPreview/producers-return race without taking it.
			auto barrier = ThreadPool::instance().createTask([] {});
			auto completed = barrier->getFuture();
			ThreadPool::instance().enqueue(barrier);
			context.Expect(completed.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
				"The worker must settle its preview before the cancel-before-take experiment.");
			completed.get();
			jobs.Cancel("cancel a ready preview before take");
			const auto preview = jobs.TakeReadyPreview();
			context.Expect(!preview && jobs.Progress().stage == TerrainProceduralStage::Cancelled,
				"Cancellation after worker completion must reject the preview instead of publishing stale ready data.");
		});
	}
}
