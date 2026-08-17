// TerrainSurfaceRenderingTests.cpp -- control residency and coupled derived publication coverage.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainSurfaceCache.h"
#include "Terrain/TerrainDerivedSystem.h"

#include <memory>

namespace TerraEngine::Tests
{
	void RegisterTerrainSurfaceRenderingTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Surface.CacheKeepsProtectedControlAndRejectsStalePublication", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest pinned{};
			pinned.key = {asset, {0, 0}, {1u, 2u}, 4u}; pinned.byteCount = 80u; pinned.priority = 10u; pinned.protectedFromEviction = true;
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest ordinary{};
			ordinary.key = {asset, {1, 0}, {3u, 4u}, 4u}; ordinary.byteCount = 80u;
			const auto plan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(4u, {}, {pinned, ordinary}, 100u);
			std::vector<TerraEngine::Renderer::Terrain::TerrainSurfacePageHandle> visible;
			std::uint64_t generation = 0u;
			context.Expect(plan.candidate.size() == 1u && plan.candidate.front().key.area == pinned.key.area,
				"Control cache pressure must retain a protected page and evict ordinary material detail.");
			context.Expect(!TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 5u)
				&& TerraEngine::Renderer::Terrain::PublishTerrainSurfaceCachePlan(visible, generation, plan, 4u),
				"A stale control-page candidate must not replace last-known-good visible surface state.");
			TerraEngine::Renderer::Terrain::TerrainSurfaceCacheRequest replacement = pinned;
			replacement.key.sourceHash = {9u, 9u};
			const auto lkgPlan = TerraEngine::Renderer::Terrain::PrepareTerrainSurfaceCachePlan(5u, visible, {replacement}, 80u);
			context.Expect(lkgPlan.candidate.size() == 1u && lkgPlan.candidate.front().lastKnownGood
				&& lkgPlan.candidate.front().key.sourceHash == pinned.key.sourceHash,
				"An over-budget replacement must retain the prior exact control page as last-known-good coverage.");
		});

		registry.Add("Terrain.Surface.DerivedBatchRequiresGeometryHolesAndControlsTogether", [](TestContext& context) {
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID(); root->publicationGeneration = 2u; root->rootContentHash = {8u, 9u}; root->authoredBounds = {0, 0, 1, 1};
			Terrain::TerrainAssetStore store(root);
			Terrain::TerrainDerivedSurfaceBatch batch{};
			batch.source = {root->assetUuid, root->rootContentHash, root->publicationGeneration, true, nullptr};
			batch.derivedGeneration = 1u; batch.geometryIdentity = {1u, 1u}; batch.holeIdentity = {2u, 2u}; batch.surfaceControlIdentity = {3u, 3u};
			batch.geometryComplete = batch.holesComplete = batch.surfaceControlsComplete = true;
			const auto accepted = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			batch.surfaceControlsComplete = false;
			const auto rejected = Terrain::ValidateTerrainDerivedSurfaceBatch(batch, store);
			context.Expect(accepted.Succeeded() && !rejected.Succeeded(),
				"Geometry, hole coverage, and material controls must pass one generation-checked publication barrier.");
		});
	}
}
