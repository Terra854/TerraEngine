// TerrainProceduralJobs.h -- Bounded ThreadPool previews for terrain operators.

#pragma once

#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainEditTransaction.h"
#include "Terrain/TerrainSelectionQuery.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	enum class TerrainProceduralStage : std::uint8_t { Idle, Queued, Selecting, RunningKernel, PreparingTransaction, ReadyPreview, Cancelled, Stale, Failed };
	struct TerrainProceduralWorkEstimate { std::size_t areas = 0u; std::size_t samples = 0u; std::size_t kernelBatches = 0u; std::size_t previewBytes = 0u; };
	struct TerrainProceduralProgress { TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID; TerrainProceduralStage stage = TerrainProceduralStage::Idle; float fraction = 0.0F; TerrainProceduralWorkEstimate estimate{}; std::string message; };
	using TerrainProceduralKernel = std::function<TerraEngine::Terrain::TerrainResultCode(
		std::span<const std::int32_t>, std::span<std::int32_t>, std::span<const std::uint8_t>, const std::atomic_bool&)>;
	// One full canonical area is the smallest safe batch for erosion and spline
	// neighbourhood kernels.  Jobs may combine several such batches atomically.
	struct TerrainProceduralJobArea
	{
		TerraEngine::Terrain::TerrainAreaDescriptor observedArea{};
		std::vector<std::int32_t> heightMm;
		TerraEngine::Terrain::TerrainSelectionGrid selectionGrid{};
	};

	struct TerrainProceduralJobRequest
	{
		TerraEngine::Terrain::TerrainRootCapture observedRoot{};
		TerraEngine::Terrain::TerrainAreaDescriptor observedArea{};
		std::vector<std::int32_t> heightMm;
		std::vector<std::uint8_t> paintedMask;
		std::vector<TerraEngine::Terrain::TerrainWeightSample> weights;
		TerraEngine::Terrain::TerrainSelectionGrid selectionGrid{};
		TerraEngine::Terrain::TerrainSelectionCriteria selectionCriteria{};
		TerrainProceduralKernel kernel;
		std::vector<TerrainProceduralJobArea> additionalAreas;
		std::string historyGroup = "Terrain procedural operation";
		std::size_t maximumAreas = 1u;
		// Neighbour-aware erosion and spline kernels need a complete area.  Smaller
		// sample slicing would make seams and exact inverse data incorrect.
		std::size_t maximumKernelBatchSamples = TerraEngine::Terrain::kTerrainAreaSampleCount;
	};

	struct TerrainProceduralPreviewArea
	{
		TerraEngine::Terrain::TerrainAreaDescriptor observedArea{};
		std::vector<std::int32_t> beforeHeightMm;
		std::vector<std::int32_t> afterHeightMm;
	};

	struct TerrainProceduralPreview
	{
		TerraEngine::UUID jobId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::TerrainRootCapture observedRoot{};
		TerraEngine::Terrain::TerrainAreaDescriptor observedArea{};
		std::vector<std::int32_t> beforeHeightMm;
		std::vector<std::int32_t> afterHeightMm;
		std::string historyGroup;
		std::vector<TerrainProceduralPreviewArea> areas;
	};

	class TerrainProceduralJobs
	{
	public:
		[[nodiscard]] TerrainProceduralWorkEstimate Estimate(const TerrainProceduralJobRequest& request) const noexcept;
		[[nodiscard]] bool Start(TerrainProceduralJobRequest request, std::string& outError);
		void Cancel(std::string_view reason);
		[[nodiscard]] TerrainProceduralProgress Progress() const;
		[[nodiscard]] std::optional<TerrainProceduralPreview> TakeReadyPreview();
		// This is the sole conversion from immutable worker output to a typed edit.
		// It rejects a changed root before storing any candidate blob or queuing work.
		[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainEditTransaction> BuildTransaction(
			TerraEngine::Terrain::TerrainAssetStore& store, const TerrainProceduralPreview& preview, std::string& outError);

	private:
		struct Completed { TerrainProceduralProgress progress; std::optional<TerrainProceduralPreview> preview; };
		// Worker jobs retain this value object rather than a pointer back to the
		// window.  Progress therefore remains readable while the UI keeps drawing.
		struct WorkerState { mutable std::mutex mutex; TerrainProceduralProgress progress; };
		mutable std::mutex mutex;
		TerrainProceduralProgress progress{};
		std::shared_ptr<WorkerState> runningState;
		std::shared_ptr<std::atomic_bool> cancellation;
		std::future<Completed> future;
		bool active = false;
	};
}
