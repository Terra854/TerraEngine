// TerrainProceduralJobs.cpp -- Coalesced worker preparation for terrain previews.

#include "TerrainProceduralJobs.h"

#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
	namespace
	{
		std::optional<TerraEngine::Terrain::TerrainSampleBounds> AreaBounds(const TerraEngine::Terrain::TerrainAreaCoordinate& coordinate)
		{
			constexpr std::int64_t extent = TerraEngine::Terrain::kTerrainAreaExtentSamples;
			if(coordinate.x > std::numeric_limits<std::int64_t>::max() / extent || coordinate.x < std::numeric_limits<std::int64_t>::min() / extent
				|| coordinate.z > std::numeric_limits<std::int64_t>::max() / extent || coordinate.z < std::numeric_limits<std::int64_t>::min() / extent) return std::nullopt;
			const std::int64_t minX = coordinate.x * extent; const std::int64_t minZ = coordinate.z * extent;
			if(minX > std::numeric_limits<std::int64_t>::max() - extent || minZ > std::numeric_limits<std::int64_t>::max() - extent) return std::nullopt;
			return TerraEngine::Terrain::TerrainSampleBounds{minX, minZ, minX + extent, minZ + extent};
		}
	}
	TerrainProceduralWorkEstimate TerrainProceduralJobs::Estimate(const TerrainProceduralJobRequest& request) const noexcept
	{
		TerrainProceduralWorkEstimate estimate;
		estimate.areas = 1u + request.additionalAreas.size();
		estimate.samples = request.heightMm.size();
		for(const TerrainProceduralJobArea& area : request.additionalAreas) estimate.samples += area.heightMm.size();
		estimate.kernelBatches = estimate.areas;
		estimate.previewBytes = estimate.samples * sizeof(std::int32_t) * 2u;
		return estimate;
	}

	bool TerrainProceduralJobs::Start(TerrainProceduralJobRequest request, std::string& outError)
	{
		if(!request.observedRoot.IsValid() || request.maximumAreas == 0u || request.maximumKernelBatchSamples == 0u
			|| request.heightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount || !request.kernel)
		{
			outError = "Procedural work requires one or more captured complete areas, a non-zero bounded batch size, and a kernel.";
			return false;
		}
		if(request.additionalAreas.size() + 1u > request.maximumAreas)
		{
			outError = "Procedural work exceeds its declared bounded area count.";
			return false;
		}
		if(request.maximumKernelBatchSamples < TerraEngine::Terrain::kTerrainAreaSampleCount)
		{
			outError = "Neighbour-aware procedural kernels require a full-area kernel batch.";
			return false;
		}
		for(const TerrainProceduralJobArea& area : request.additionalAreas)
			if(area.heightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
			{
				outError = "Every procedural area batch requires a complete canonical height area.";
				return false;
			}
		Cancel("Coalesced by a newer terrain procedural preview.");
		const auto token = std::make_shared<std::atomic_bool>(false);
		const TerrainProceduralWorkEstimate estimate = Estimate(request);
		const TerraEngine::UUID jobId = TerraEngine::GenerateUUID();
		const auto state = std::make_shared<WorkerState>();
		state->progress = {jobId, TerrainProceduralStage::Queued, 0.0F, estimate, "Queued bounded terrain preview."};
		{
			std::scoped_lock lock(mutex);
			cancellation = token;
			progress = state->progress;
			runningState = state;
			active = true;
		}
		auto work = [request = std::move(request), token, jobId, estimate, state]() mutable -> Completed {
			Completed completed;
			auto update = [&state](TerrainProceduralStage stage, float fraction, std::string message) {
				std::scoped_lock lock(state->mutex);
				state->progress.stage = stage; state->progress.fraction = fraction; state->progress.message = std::move(message);
			};
			completed.progress = {jobId, TerrainProceduralStage::Selecting, 0.1F, estimate, "Evaluating immutable terrain selection."};
			update(completed.progress.stage, completed.progress.fraction, completed.progress.message);
			if(token->load(std::memory_order_acquire)) { completed.progress.stage = TerrainProceduralStage::Cancelled; completed.progress.message = "Cancelled before selection."; return completed; }
			std::vector<TerrainProceduralPreviewArea> areas;
			areas.reserve(request.additionalAreas.size() + 1u);
			auto runArea = [&](const TerraEngine::Terrain::TerrainAreaDescriptor& descriptor, const std::vector<std::int32_t>& heights,
				TerraEngine::Terrain::TerrainSelectionGrid selectionGrid, std::size_t index) -> bool {
				// Spans are rebound to copied job storage, so UI lifetime cannot race.
				selectionGrid.heightMm = heights;
				selectionGrid.paintedMask = index == 0u ? request.paintedMask : std::span<const std::uint8_t>{};
				selectionGrid.weights = index == 0u ? request.weights : std::span<const TerraEngine::Terrain::TerrainWeightSample>{};
				const auto selection = TerraEngine::Terrain::EvaluateTerrainSelection(selectionGrid, request.selectionCriteria);
				if(!selection.Succeeded()) { completed.progress.stage = TerrainProceduralStage::Failed; completed.progress.message = selection.message; return false; }
				if(token->load(std::memory_order_acquire)) { completed.progress.stage = TerrainProceduralStage::Cancelled; completed.progress.message = "Cancelled between bounded area batches."; return false; }
				completed.progress.stage = TerrainProceduralStage::RunningKernel;
				completed.progress.fraction = 0.2F + 0.6F * static_cast<float>(index) / static_cast<float>(request.additionalAreas.size() + 1u);
				update(completed.progress.stage, completed.progress.fraction, "Running bounded area kernel batch " + std::to_string(index + 1u) + ".");
				std::vector<std::int32_t> output(heights.size());
				const auto code = request.kernel(heights, output, selection.mask, *token);
				if(token->load(std::memory_order_acquire)) { completed.progress.stage = TerrainProceduralStage::Cancelled; completed.progress.message = "Cancelled during a bounded kernel batch."; return false; }
				if(code != TerraEngine::Terrain::TerrainResultCode::Ok) { completed.progress.stage = TerrainProceduralStage::Failed; completed.progress.message = "Procedural kernel rejected a bounded area batch."; return false; }
				areas.push_back({descriptor, heights, std::move(output)}); return true;
			};
			if(!runArea(request.observedArea, request.heightMm, request.selectionGrid, 0u)) return completed;
			for(std::size_t index = 0u; index < request.additionalAreas.size(); ++index)
				if(!runArea(request.additionalAreas[index].observedArea, request.additionalAreas[index].heightMm, request.additionalAreas[index].selectionGrid, index + 1u)) return completed;
			completed.progress.stage = TerrainProceduralStage::PreparingTransaction;
			completed.progress.fraction = 0.85F;
			update(completed.progress.stage, completed.progress.fraction, "Preparing exact immutable before/after data.");
			if(token->load(std::memory_order_acquire)) { completed.progress.stage = TerrainProceduralStage::Cancelled; completed.progress.message = "Cancelled before detached preview publication."; return completed; }
			completed.preview = TerrainProceduralPreview{jobId, request.observedRoot, request.observedArea, areas.front().beforeHeightMm, areas.front().afterHeightMm, std::move(request.historyGroup), std::move(areas)};
			completed.progress.stage = TerrainProceduralStage::ReadyPreview;
			completed.progress.fraction = 1.0F;
			completed.progress.message = "Detached procedural preview is ready to Apply or Cancel.";
			update(completed.progress.stage, completed.progress.fraction, completed.progress.message);
			return completed;
		};
		auto task = ThreadPool::instance().createTask(std::move(work));
		future = task->getFuture();
		ThreadPool::instance().enqueue(task, estimate.samples);
		return true;
	}

	void TerrainProceduralJobs::Cancel(std::string_view reason)
	{
		std::scoped_lock lock(mutex);
		if(cancellation) cancellation->store(true, std::memory_order_release);
		if(active) { progress.stage = TerrainProceduralStage::Cancelled; progress.message = std::string(reason); }
		if(runningState)
		{
			std::scoped_lock stateLock(runningState->mutex);
			runningState->progress.stage = TerrainProceduralStage::Cancelled;
			runningState->progress.message = std::string(reason);
		}
	}

	TerrainProceduralProgress TerrainProceduralJobs::Progress() const
	{
		std::scoped_lock lock(mutex);
		if(runningState)
		{
			std::scoped_lock stateLock(runningState->mutex);
			return runningState->progress;
		}
		return progress;
	}

	std::optional<TerrainProceduralPreview> TerrainProceduralJobs::TakeReadyPreview()
	{
		if(!future.valid() || future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
		Completed completed = future.get();
		std::scoped_lock lock(mutex);
		active = false;
		progress = completed.progress;
		runningState.reset();
		return std::move(completed.preview);
	}

	std::optional<TerraEngine::Terrain::TerrainEditTransaction> TerrainProceduralJobs::BuildTransaction(TerraEngine::Terrain::TerrainAssetStore& store, const TerrainProceduralPreview& preview, std::string& outError)
	{
		const auto currentRoot = store.CaptureRoot();
		if(!currentRoot.IsValid() || !preview.observedRoot.IsValid() || currentRoot.root != preview.observedRoot.root)
		{
			std::scoped_lock lock(mutex);
			progress.stage = TerrainProceduralStage::Stale;
			progress.message = "Procedural preview was rejected because its captured root is stale.";
			outError = "Procedural preview is stale because the authored terrain root changed.";
			return std::nullopt;
		}
		const std::vector<TerrainProceduralPreviewArea> fallbackAreas = {{preview.observedArea, preview.beforeHeightMm, preview.afterHeightMm}};
		const auto& previewAreas = preview.areas.empty() ? fallbackAreas : preview.areas;
		if(previewAreas.empty())
		{
			outError = "Procedural preview contains no bounded terrain areas.";
			return std::nullopt;
		}
		auto makeSpan = [](TerraEngine::Terrain::ContentHash hash, const std::vector<std::uint8_t>& bytes) {
			TerraEngine::Terrain::TerrainExactSpan span; span.channel = TerraEngine::Terrain::TerrainChannelKind::Height;
			span.uncompressedByteCount = static_cast<std::uint32_t>(bytes.size()); span.compressedContentHash = hash;
			span.immutableBlobHash = hash; span.compressedBytes = bytes; return span;
		};
		TerraEngine::Terrain::TerrainEditTransaction transaction; transaction.transactionId = TerraEngine::GenerateUUID(); transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
		transaction.targetAssetUuid = currentRoot.root->assetUuid; transaction.historyGroup = preview.historyGroup; transaction.inputRootHash = currentRoot.root->rootContentHash;
		transaction.expectedLayerStackGeneration = currentRoot.root->layerStackGeneration; transaction.budget.maximumAreas = previewAreas.size();
		transaction.budget.maximumSamples = previewAreas.size() * TerraEngine::Terrain::kTerrainAreaSampleCount;
		const auto firstBounds = AreaBounds(previewAreas.front().observedArea.coordinate);
		if(!firstBounds) { outError = "Procedural area coordinate cannot form an exact affected bounds."; return std::nullopt; }
		transaction.affectedBounds = *firstBounds;
		for(const TerrainProceduralPreviewArea& area : previewAreas)
		{
			if(area.beforeHeightMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount || area.afterHeightMm.size() != area.beforeHeightMm.size())
			{
				outError = "Every procedural preview batch must contain exactly one complete area.";
				return std::nullopt;
			}
			std::vector<std::uint8_t> before(area.beforeHeightMm.size() * sizeof(std::int32_t));
			std::vector<std::uint8_t> after(area.afterHeightMm.size() * sizeof(std::int32_t));
			std::memcpy(before.data(), area.beforeHeightMm.data(), before.size()); std::memcpy(after.data(), area.afterHeightMm.data(), after.size());
			const auto beforeHash = TerraEngine::Terrain::HashTerrainBytes(before); const auto afterHash = TerraEngine::Terrain::HashTerrainBytes(after);
			if(!store.Blobs().PutVerified(beforeHash, before) || !store.Blobs().PutVerified(afterHash, after))
			{
				outError = "Procedural preview could not retain immutable before/after height blobs.";
				return std::nullopt;
			}
			TerraEngine::Terrain::TerrainPatch patch; patch.kind = TerraEngine::Terrain::TerrainPatchKind::ReplaceHeight; patch.preparedBlobHash = afterHash;
			patch.replacement = area.observedArea; patch.replacement.editGeneration++; patch.replacement.authoredContentHash = afterHash; patch.replacement.channels.heightHash = afterHash;
			patch.beforeSpans = {makeSpan(beforeHash, before)}; patch.afterSpans = {makeSpan(afterHash, after)}; patch.retainedBlobHashes = {beforeHash, afterHash};
			TerraEngine::Terrain::TerrainPatch inverse = patch; inverse.preparedBlobHash = beforeHash; inverse.replacement.authoredContentHash = beforeHash; inverse.replacement.channels.heightHash = beforeHash;
			inverse.beforeSpans = patch.afterSpans; inverse.afterSpans = patch.beforeSpans;
			transaction.readSet.push_back({area.observedArea.coordinate, area.observedArea.editGeneration, area.observedArea.authoredContentHash});
			transaction.patches.push_back(std::move(patch)); transaction.inversePatches.push_back(std::move(inverse));
			const auto bounds = AreaBounds(area.observedArea.coordinate);
			if(!bounds) { outError = "Procedural area coordinate cannot form an exact affected bounds."; return std::nullopt; }
			transaction.affectedBounds.minX = std::min(transaction.affectedBounds.minX, bounds->minX);
			transaction.affectedBounds.minZ = std::min(transaction.affectedBounds.minZ, bounds->minZ);
			transaction.affectedBounds.maxXExclusive = std::max(transaction.affectedBounds.maxXExclusive, bounds->maxXExclusive);
			transaction.affectedBounds.maxZExclusive = std::max(transaction.affectedBounds.maxZExclusive, bounds->maxZExclusive);
		}
		return transaction;
	}
}
