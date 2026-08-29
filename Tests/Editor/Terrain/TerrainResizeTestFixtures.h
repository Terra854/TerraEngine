// TerrainResizeTestFixtures.h -- Editor resize job fixtures shared by workflow tests.

#pragma once

#include "Editor/TerraEngine-Editor/Terrain/TerrainResizeJobs.h"
#include "../../Engine/Terrain/TerrainResizeTestFixtures.h"

namespace TerraEngine::Tests::TerrainResizeEditorFixtures
{
	inline TerraEngine::Editor::Terrain::TerrainResizeJobRequest MakeExpansionRequest(
		const std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot>& root,
		TerraEngine::Terrain::TerrainSampleBounds proposed)
	{
		TerraEngine::Editor::Terrain::TerrainResizeJobRequest request{};
		request.observedRoot = {root, root->publicationGeneration};
		request.delta.kind = TerraEngine::Terrain::TerrainResizeKind::Expand;
		request.delta.previousBounds = root->authoredBounds;
		request.delta.proposedBounds = proposed;
		request.delta.expansionPolicy = TerraEngine::Terrain::TerrainExpansionPolicy::EdgeBlend;
		request.delta.transitionSamples = 32u;
		return request;
	}
}
