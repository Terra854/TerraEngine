// TerrainAuthoringSession.cpp -- Transactional terrain editing session.

#include "TerrainAuthoringSession.h"

#include "Terrain/TerrainAreaCodec.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <limits>

namespace TerraEngine::Editor::Terrain
{
	bool TerrainAuthoringSession::Begin(const TerraEngine::Terrain::TerrainRootCapture& capture, std::string& outError)
	{
		Cancel();
		if(!capture.IsValid() || capture.root->assetUuid == TerraEngine::EMPTY_UUID)
		{
			outError = "A committed terrain root is required before authoring can begin.";
			return false;
		}

		active = true;
		assetId = capture.root->assetUuid;
		observedGeneration = capture.publicationGeneration;
		observedLayerStackGeneration = capture.root->layerStackGeneration;
		observedRootHash = capture.root->rootContentHash;
		constexpr std::size_t kMaximumObservedAreas = 4096u;
		const auto coveredAreas = TerraEngine::Terrain::EnumerateTerrainAreas(
			capture.root->authoredBounds, kMaximumObservedAreas);
		const bool declaresCoverage = capture.root->authoredBounds.IsValid()
			&& capture.root->authoredBounds.minX < capture.root->authoredBounds.maxXExclusive
			&& capture.root->authoredBounds.minZ < capture.root->authoredBounds.maxZExclusive;
		if(!coveredAreas && declaresCoverage)
		{
			Cancel();
			outError = "The terrain extent exceeds the bounded authoring observation cache.";
			return false;
		}
		TerraEngine::Terrain::TerrainAssetStore resolver(capture.root);
		if(coveredAreas) for(const auto coordinate : *coveredAreas)
		{
			const auto descriptor = resolver.ResolveDescriptorOrImplicit(*capture.root, coordinate);
			observations.emplace(coordinate, TerraEngine::Terrain::TerrainAreaObservation{
				coordinate, descriptor.editGeneration, descriptor.authoredContentHash});
		}
		else
		{
			// Legacy in-memory tests and compatibility roots may predate authored
			// bounds. Preserve their explicit descriptor observations while all new
			// persisted roots use the authoritative bounds-driven path above.
			for(const auto& shard : capture.root->directoryShards)
				for(const auto& [coordinate, descriptor] : shard.descriptors)
					observations.emplace(coordinate, TerraEngine::Terrain::TerrainAreaObservation{
						coordinate, descriptor.editGeneration, descriptor.authoredContentHash});
		}
		return true;
	}

	void TerrainAuthoringSession::Cancel() noexcept
	{
		active = false;
		assetId = TerraEngine::EMPTY_UUID;
		observedGeneration = 0U;
		observedLayerStackGeneration = 0U;
		observedRootHash = {};
		observations.clear();
		residentAreas.clear();
		heightDeltasMm.clear();
	}

	void TerrainAuthoringSession::SetResidentAreas(std::set<TerraEngine::Terrain::TerrainAreaCoordinate> resident)
	{
		residentAreas = std::move(resident);
	}

	bool TerrainAuthoringSession::AddHeightDelta(
		TerraEngine::Terrain::TerrainAreaCoordinate area,
		std::vector<std::int32_t> deltaMm,
		std::string& outError)
	{
		if(!active)
		{
			outError = "Start an authoring session before sculpting.";
			return false;
		}
		if(!residentAreas.contains(area))
		{
			outError = "The targeted terrain area is not resident; the stroke was rejected without a preview.";
			return false;
		}
		if(deltaMm.size() != TerraEngine::Terrain::kTerrainAreaSampleCount)
		{
			outError = "A terrain preview delta must contain exactly one area of samples.";
			return false;
		}

		std::vector<std::int32_t>& accumulated = heightDeltasMm[area];
		if(accumulated.empty()) accumulated.assign(deltaMm.size(), 0);
		// Preflight the complete area before changing detached preview state. A
		// rejected stroke is atomic even when an overflow occurs near the end.
		for(std::size_t index = 0U; index < deltaMm.size(); ++index)
		{
			const std::int64_t sum = static_cast<std::int64_t>(accumulated[index])
				+ static_cast<std::int64_t>(deltaMm[index]);
			if(sum < std::numeric_limits<std::int32_t>::min()
				|| sum > std::numeric_limits<std::int32_t>::max())
			{
				outError = "The terrain preview delta exceeds the signed 32-bit millimetre range.";
				return false;
			}
		}
		for(std::size_t index = 0U; index < deltaMm.size(); ++index)
			accumulated[index] += deltaMm[index];
		return true;
	}

	std::optional<TerraEngine::Terrain::TerrainEditTransaction> TerrainAuthoringSession::BuildCommitRequest(
		std::vector<TerraEngine::Terrain::TerrainPatch> patches,
		std::string historyGroup,
		std::string& outError) const
	{
		if(!active || !IsDirty())
		{
			outError = "There is no detached terrain preview to apply.";
			return std::nullopt;
		}
		if(patches.empty() || patches.size() != heightDeltasMm.size())
		{
			outError = "Every edited terrain area needs one prepared replacement before Apply.";
			return std::nullopt;
		}

		TerraEngine::Terrain::TerrainEditTransaction transaction;
		transaction.transactionId = TerraEngine::GenerateUUID();
		transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
		transaction.targetAssetUuid = assetId;
		transaction.historyGroup = std::move(historyGroup);
		transaction.inputRootHash = observedRootHash;
		transaction.expectedLayerStackGeneration = observedLayerStackGeneration;
		// Area ownership is a fixed 256-sample lattice. The grouped transaction
		// names one conservative half-open bounding box for publication validation.
		auto firstArea = heightDeltasMm.begin();
		transaction.affectedBounds = {
			firstArea->first.x * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			firstArea->first.z * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			(firstArea->first.x + 1) * TerraEngine::Terrain::kTerrainAreaExtentSamples,
			(firstArea->first.z + 1) * TerraEngine::Terrain::kTerrainAreaExtentSamples};
		for(const auto& [area, delta] : heightDeltasMm)
		{
			(void)delta;
			transaction.affectedBounds.minX = std::min(transaction.affectedBounds.minX, area.x * TerraEngine::Terrain::kTerrainAreaExtentSamples);
			transaction.affectedBounds.minZ = std::min(transaction.affectedBounds.minZ, area.z * TerraEngine::Terrain::kTerrainAreaExtentSamples);
			transaction.affectedBounds.maxXExclusive = std::max(transaction.affectedBounds.maxXExclusive, (area.x + 1) * TerraEngine::Terrain::kTerrainAreaExtentSamples);
			transaction.affectedBounds.maxZExclusive = std::max(transaction.affectedBounds.maxZExclusive, (area.z + 1) * TerraEngine::Terrain::kTerrainAreaExtentSamples);
		}
		transaction.budget.maximumAreas = patches.size();
		transaction.budget.maximumSamples = patches.size() * TerraEngine::Terrain::kTerrainAreaSampleCount;
		transaction.patches = std::move(patches);
		for(const auto& [area, delta] : heightDeltasMm)
		{
			(void)delta;
			auto observation = observations.find(area);
			transaction.readSet.push_back(observation != observations.end()
				? observation->second
				: TerraEngine::Terrain::TerrainAreaObservation{area, 0U, {}});
		}
		return transaction;
	}

	std::optional<TerraEngine::Terrain::TerrainEditTransaction> TerrainAuthoringSession::BuildLayerCommitRequest(
		const TerraEngine::Terrain::TerrainRootCapture& capture,
		TerraEngine::Terrain::TerrainLayerPatch forward,
		TerraEngine::Terrain::TerrainLayerPatch inverse,
		std::string historyGroup,
		std::string& outError)
	{
		if(!capture.IsValid() || !capture.root->layerStack || !forward.expectedGeneration)
		{
			outError = "A complete committed layer generation is required before applying a mapping preview.";
			return std::nullopt;
		}
		if(forward.expectedGeneration != capture.root->layerStackGeneration)
		{
			outError = "The terrain layer generation changed while the detached mapping preview was open.";
			return std::nullopt;
		}

		TerraEngine::Terrain::TerrainEditTransaction transaction;
		transaction.transactionId = TerraEngine::GenerateUUID();
		transaction.origin = TerraEngine::Terrain::TerrainEditOrigin::VisibleEditor;
		transaction.targetAssetUuid = capture.root->assetUuid;
		transaction.historyGroup = std::move(historyGroup);
		transaction.inputRootHash = capture.root->rootContentHash;
		transaction.expectedLayerStackGeneration = capture.root->layerStackGeneration;
		transaction.affectedBounds = capture.root->authoredBounds;
		transaction.budget.maximumAreas = 1u;
		transaction.layerStackPatch = std::move(forward);
		transaction.inverseLayerStackPatch = std::move(inverse);
		return transaction;
	}
}
