// TerrainResizeJobs.h -- Cancellable detached preview jobs for terrain resizing.

#pragma once

#include "Terrain/TerrainEditTransaction.h"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <utility>

namespace TerraEngine::Editor::Terrain
{
	enum class TerrainResizeStage : std::uint8_t
	{
		Idle, Validating, InspectingBoundary, InspectingDiscardedContent,
		PreparingAreas, PreparingDerived, ReadyForConfirmation,
		AwaitingProtectedPublish, Committed, Cancelled, Stale, Failed
	};

	struct TerrainResizeProgress
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerrainResizeStage stage = TerrainResizeStage::Idle;
		float fraction = 0.0F;
		std::size_t completedAreas = 0u;
		std::size_t totalAreas = 0u;
		bool cancellationAvailable = false;
		std::string message;
	};

	struct TerrainResizeJobRequest
	{
		struct AreaSource
		{
			TerraEngine::Terrain::TerrainAreaDescriptor descriptor{};
			std::vector<std::int32_t> heightMm;
			std::vector<std::uint8_t> holeBits;
			std::map<std::uint16_t, std::vector<TerraEngine::Terrain::TerrainWeightSample>> surfaceGroups;
		};
		TerraEngine::Terrain::TerrainRootCapture observedRoot{};
		TerraEngine::Terrain::TerrainResizeDelta delta{};
		std::size_t maximumAreas = 64u;
		TerraEngine::Terrain::TerrainResizeBudget budget{};
		bool holePolicyExplicit = false;
		// The callback closes over detached, immutable area snapshots. Worker jobs
		// never consult the live asset store while evaluating uneven authored edges.
		TerraEngine::Terrain::TerrainHeightSampler sourceHeight;
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, AreaSource> sourceAreas;
	};

	struct TerrainPreparedResize
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainRootCapture observedRoot{};
		TerraEngine::Terrain::TerrainResizeRequest request{};
		TerraEngine::Terrain::TerrainResizePreview preview{};
		TerraEngine::Terrain::Generation readyGeneration = 0u;
		std::vector<std::pair<TerraEngine::Terrain::ContentHash, std::vector<std::uint8_t>>> preparedBlobs;
	};

	class TerrainResizeJobs
	{
	public:
		[[nodiscard]] bool Start(TerrainResizeJobRequest request, std::string& outError);
		void Cancel(std::string_view reason);
		[[nodiscard]] TerrainResizeProgress Progress() const;
		[[nodiscard]] std::optional<TerrainPreparedResize> TakeReadyPreview();
		[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainEditTransaction> BuildTransaction(
			TerraEngine::Terrain::TerrainAssetStore& store, const TerrainPreparedResize& prepared,
			bool dataLossConfirmed, std::string& outError);

	private:
		struct Completed { TerrainResizeProgress progress; std::optional<TerrainPreparedResize> prepared; };
		struct WorkerState { mutable std::mutex mutex; TerrainResizeProgress progress; };
		mutable std::mutex mutex;
		TerrainResizeProgress progress{};
		std::shared_ptr<std::atomic_bool> cancellation;
		std::shared_ptr<WorkerState> runningState;
		std::future<Completed> future;
	};
}
