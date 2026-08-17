// TerrainPageCookerTests.cpp -- Deterministic terrain hierarchy and packet tests.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainRenderPackets.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
	TerraEngine::Renderer::Terrain::TerrainCookInput MakeCookInput()
	{
		static std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount);
		for (std::uint32_t z = 0u; z < 256u; ++z)
			for (std::uint32_t x = 0u; x < 256u; ++x)
				heights[z * 256u + x] = static_cast<std::int32_t>(x * 10u + z * 7u);
		TerraEngine::Renderer::Terrain::TerrainCookInput input{};
		input.assetId = TerraEngine::GenerateUUID();
		input.authoredHash = {42u, 99u};
		input.authoredGeneration = 7u;
		input.heightMillimetres = heights;
		return input;
	}
}

	void RegisterTerrainPageCookerTests(TestRegistry& registry)
	{
		registry.Add("Terrain.PageCooker.BuildsCompleteConservativeHierarchy", [](TestContext& context) {
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(MakeCookInput());
			context.Expect(cooked.complete && cooked.errorPyramid.size() == 9u, "A 256-sample terrain area should cook a complete 256-to-1 error pyramid.");
			context.Expect(cooked.errorPyramid.front().bounds.size() == 256u * 256u && cooked.errorPyramid.back().bounds.size() == 1u, "Every hierarchy level needs explicit conservative bounds.");
			context.Expect(cooked.meshlets.size() == 256u && cooked.clusters.size() == 17u && cooked.clusters.back().meshletCount == 256u, "The deterministic cooker should expose base meshlets and one root cluster.");
			context.Expect(cooked.pageRanges.size() == 1u && cooked.pageRanges.front().payloadIdentity == cooked.sourceIdentity, "A page range must retain the exact payload identity used by streaming publication.");
		});

		registry.Add("Terrain.PageCooker.RejectsMismatchedCookIdentityFromRenderPacket", [](TestContext& context) {
			const auto input = MakeCookInput();
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
			view.assetId = input.assetId; view.authoredHash = {1u, 2u}; view.pageSourceIdentity = cooked.sourceIdentity; view.authoredGeneration = input.authoredGeneration; view.frameId = 1u;
			const auto packet = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, cooked, {});
			context.Expect(!packet.has_value(), "A mismatched cooked source must not become a fresh render packet without last-known-good fallback.");
		});
	}
}
