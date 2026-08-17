// TerrainLodSeamTests.cpp -- deterministic seam, morph, and neighbor identity tests.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainDistantProxy.h"
#include "Renderer/RenderSystem.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainLodSeamTests(TestRegistry& registry)
	{
		registry.Add("Terrain.LodSeams.CooksStitchesAndInvalidatesNeighborChanges", [](TestContext& context) {
			std::vector<std::int32_t> heights(Terrain::kTerrainAreaSampleCount);
			for (std::uint32_t z = 0u; z < 256u; ++z)
				for (std::uint32_t x = 0u; x < 256u; ++x) heights[z * 256u + x] = static_cast<std::int32_t>(x + z * 2u);
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {11u, 12u}; input.authoredGeneration = 3u; input.heightMillimetres = heights;
			input.neighborAuthoredHashes[0] = {21u, 22u};
			const auto first = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			input.neighborAuthoredHashes[0] = {23u, 24u};
			const auto changedNeighbor = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			context.Expect(first.complete && !first.edgeStitchVariants.empty() && first.lodNodes.size() == first.errorPyramid.size(),
				"A cooked terrain page needs explicit morph nodes and border stitch variants.");
			context.Expect(first.sourceIdentity != changedNeighbor.sourceIdentity && first.neighborDependencyHash != changedNeighbor.neighborDependencyHash,
				"A neighbor-only edit must invalidate derived seam data.");
			context.Expect(TerraEngine::Renderer::Terrain::AreTerrainLodLevelsCompatible(4u, 5u)
				&& !TerraEngine::Renderer::Terrain::AreTerrainLodLevelsCompatible(2u, 4u),
				"Terrain cuts may differ by one LOD only, guaranteeing a stitch or parent fallback.");
		});

		registry.Add("Terrain.LodSeams.RegionalCompleteCutProvidesMultiViewAncestorCoverage", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			std::vector<std::int32_t> heights(Terrain::kTerrainAreaSampleCount, 120);
			std::vector<TerraEngine::Renderer::Terrain::TerrainPageCookResult> pages;
			for (std::int64_t z = 0; z < 2; ++z)
				for (std::int64_t x = 0; x < 2; ++x)
				{
					TerraEngine::Renderer::Terrain::TerrainCookInput input{};
					input.assetId = asset; input.area = {x, z}; input.authoredHash = {90u, 91u}; input.authoredGeneration = 6u; input.heightMillimetres = heights;
					pages.push_back(TerraEngine::Renderer::Terrain::CookTerrainPage(input));
				}
			const auto regional = TerraEngine::Renderer::Terrain::BuildTerrainDistantProxy(asset, {0, 0}, 2u, pages);
			context.Expect(regional.IsComplete() && regional.conservativeErrorMetres >= 0.0f,
				"A regional boundary requires every child page before it becomes an ancestor fallback proxy.");

			std::vector<TerraEngine::Renderer::Terrain::TerrainViewPacket> views;
			for (std::uint32_t viewId = 1u; viewId <= 2u; ++viewId)
			{
				TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
				view.frameId = 10u; view.viewId = viewId; view.assetId = asset; view.area = {0, 0}; view.authoredHash = {90u, 91u};
				view.authoredGeneration = 6u; view.pageSourceIdentity = pages.front().sourceIdentity; view.requestedLod = static_cast<std::uint16_t>(viewId - 1u);
				views.push_back(view);
			}
			const auto packets = RenderSystem::PrepareCommittedTerrainPackets(views, pages, {});
			context.Expect(packets.size() == 2u && packets[0].view.viewId != packets[1].view.viewId
				&& packets[0].candidate.candidateId != packets[1].candidate.candidateId,
				"Scene and capture-like views must retain independent LOD packet identities while a complete regional cut prevents coverage holes.");
			context.Expect(!pages.front().edgeStitchVariants.empty() && pages.front().lodNodes.front().morphEndMetres > pages.front().lodNodes.front().morphStartMetres,
				"The selected complete cut must carry real stitch variants and parent geomorph intervals.");
		});
	}
}
