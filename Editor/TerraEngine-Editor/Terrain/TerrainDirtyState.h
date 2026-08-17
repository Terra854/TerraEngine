// TerrainDirtyState.h -- Root-content identity based terrain save state.

#pragma once

#include "Terrain/TerrainAssetStore.h"

namespace TerraEngine::Editor::Terrain
{
	class TerrainDirtyState
	{
	public:
		void Observe(const TerraEngine::Terrain::TerrainRootCapture& capture);
		void MarkSaved(const TerraEngine::Terrain::TerrainRootCapture& capture);
		[[nodiscard]] bool IsDirty() const noexcept { return currentHash != savedHash; }
		[[nodiscard]] TerraEngine::Terrain::ContentHash SavedHash() const noexcept { return savedHash; }
		[[nodiscard]] TerraEngine::Terrain::ContentHash CurrentHash() const noexcept { return currentHash; }
	private:
		TerraEngine::Terrain::ContentHash currentHash{};
		TerraEngine::Terrain::ContentHash savedHash{};
	};
}
