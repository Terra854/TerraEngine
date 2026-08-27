// TerrainAuthoringSession.h -- Detached editor state for one terrain stroke.
//
// A session owns only copies of observations and preview deltas.  It never
// aliases a live terrain root, which keeps Cancel cheap and makes Apply an
// explicit typed transaction at the protected authoring boundary.

#pragma once

#include "Terrain/TerrainEditTransaction.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	class TerrainAuthoringSession
	{
	public:
		[[nodiscard]] bool Begin(const TerraEngine::Terrain::TerrainRootCapture& capture, std::string& outError);
		void Cancel() noexcept;
		[[nodiscard]] bool IsActive() const noexcept { return active; }
		[[nodiscard]] bool IsDirty() const noexcept { return !heightDeltasMm.empty(); }
		[[nodiscard]] TerraEngine::Terrain::Generation ObservedGeneration() const noexcept { return observedGeneration; }
		[[nodiscard]] TerraEngine::UUID TargetAssetId() const noexcept { return assetId; }

		// A resident snapshot is required before a brush may alter its area.  This
		// prevents a partial stroke from silently inventing unavailable terrain.
		void SetResidentAreas(std::set<TerraEngine::Terrain::TerrainAreaCoordinate> resident);
		[[nodiscard]] bool AddHeightDelta(
			TerraEngine::Terrain::TerrainAreaCoordinate area,
			std::vector<std::int32_t> deltaMm,
			std::string& outError);
		[[nodiscard]] const std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::int32_t>>& PreviewHeightDeltas() const noexcept { return heightDeltasMm; }

		// The caller supplies prepared descriptors after encoding the detached
		// deltas.  This method only constructs the transactional publication request.
		[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainEditTransaction> BuildCommitRequest(
			std::vector<TerraEngine::Terrain::TerrainPatch> patches,
			std::string historyGroup,
			std::string& outError) const;

		// Layer controls also edit detached copies, but they do not need a sculpt
		// session or resident sample buffers. This shared builder gives Apply and
		// edit-deactivation the same typed transaction and exact undo pair.
		[[nodiscard]] static std::optional<TerraEngine::Terrain::TerrainEditTransaction> BuildLayerCommitRequest(
			const TerraEngine::Terrain::TerrainRootCapture& capture,
			TerraEngine::Terrain::TerrainLayerPatch forward,
			TerraEngine::Terrain::TerrainLayerPatch inverse,
			std::string historyGroup,
			std::string& outError);

	private:
		bool active = false;
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerraEngine::Terrain::Generation observedGeneration = 0U;
		TerraEngine::Terrain::Generation observedLayerStackGeneration = 0U;
		TerraEngine::Terrain::ContentHash observedRootHash{};
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, TerraEngine::Terrain::TerrainAreaObservation> observations;
		std::set<TerraEngine::Terrain::TerrainAreaCoordinate> residentAreas;
		std::map<TerraEngine::Terrain::TerrainAreaCoordinate, std::vector<std::int32_t>> heightDeltasMm;
	};
}
