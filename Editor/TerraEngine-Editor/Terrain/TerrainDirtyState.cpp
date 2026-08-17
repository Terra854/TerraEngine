// TerrainDirtyState.cpp -- Generation numbers never decide terrain dirtiness.

#include "TerrainDirtyState.h"

namespace TerraEngine::Editor::Terrain
{
	void TerrainDirtyState::Observe(const TerraEngine::Terrain::TerrainRootCapture& capture)
	{
		if(capture.IsValid()) currentHash = capture.root->rootContentHash;
	}
	void TerrainDirtyState::MarkSaved(const TerraEngine::Terrain::TerrainRootCapture& capture)
	{
		Observe(capture);
		savedHash = currentHash;
	}
}
