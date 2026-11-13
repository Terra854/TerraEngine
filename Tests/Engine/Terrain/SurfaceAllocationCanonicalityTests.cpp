#include "TestHarness.h"
#include "Renderer/Terrain/TerrainSurfaceCache.h"
#include "Terrain/TerrainAreaCodec.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <set>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace Renderer::Terrain;
		TerrainSurfacePageKey AllocationKey(const TerraEngine::UUID& asset, std::int64_t area) {
			return {asset, {area, 0}, {static_cast<std::uint64_t>(area + 1), 19U}, 7U};
		}
		void ExpectCanonicalWeight(TestContext& context, std::span<const Terrain::TerrainWeightContribution> contributions) {
			const auto sample = Terrain::NormalizeTerrainWeights(contributions);
			context.Expect(sample.IsCanonical(), "Production normalization must satisfy the canonical sample contract.");
			unsigned sum = 0;
			for (const auto slot : sample.slots) {
				sum += slot.weight;
				context.Expect((slot.layerId == 0U) == (slot.weight == 0U), "Quantized empty slots must also clear their layer identity.");
			}
			context.ExpectEqual(255U, sum, "A nonempty normalized sample must sum to exactly 255.");
			std::array<std::uint8_t, 12> encoded{};
			context.Expect(Terrain::EncodeTerrainWeightSample(sample, encoded).Succeeded(), "The actual canonical encoder must accept normalized weights.");
			Terrain::TerrainWeightSample decoded;
			context.Expect(Terrain::DecodeTerrainWeightSample(encoded, decoded).Succeeded() && decoded == sample, "The actual decoder must retain every normalized slot.");
		}
	}
	void RegisterSurfaceAllocationCanonicalityTests(TestRegistry& registry) {
		registry.Add("Terrain.SurfaceAllocation.SparseTilesNeverAlias", [](TestContext& context) {
			const auto asset = GenerateUUID();
			const std::vector<TerrainSurfacePageHandle> prior{
				{AllocationKey(asset, 0), 0U, 16U, 7U}, {AllocationKey(asset, 1), 2U, 16U, 7U}};
			const auto plan = PrepareTerrainSurfaceCachePlan(7U, prior, {{AllocationKey(asset, 2), 16U}}, 128U);
			context.ExpectEqual(std::size_t{3}, plan.candidate.size(), "The budget must retain both existing pages and the requested page.");
			std::set<std::uint32_t> tiles;
			for (const auto& page : plan.candidate) context.Expect(page.IsResident() && tiles.insert(page.atlasTile).second, "Every resident page must own a distinct valid physical tile after sparse eviction.");
			context.Expect(plan.candidate[0].atlasTile == 0U && plan.candidate[1].atlasTile == 2U, "Allocating a page may not move existing owners.");
		});
		registry.Add("Terrain.SurfaceAllocation.HighTileUsesBoundedFreeIdentity", [](TestContext& context) {
			const auto asset = GenerateUUID();
			const std::vector<TerrainSurfacePageHandle> prior{
				{AllocationKey(asset, 0), std::numeric_limits<std::uint32_t>::max() - 1U, 16U, 7U},
				{AllocationKey(asset, 1), 0U, 16U, 7U}};
			const auto plan = PrepareTerrainSurfaceCachePlan(7U, prior, {{AllocationKey(asset, 2), 16U}}, 128U);
			context.ExpectEqual(std::size_t{3}, plan.candidate.size(), "Sparse high tile identities must not require a large backing vector.");
			const auto tile = plan.candidate.back().atlasTile;
			context.Expect(tile != 0U && tile != prior.front().atlasTile && tile != std::numeric_limits<std::uint32_t>::max(), "Allocation must avoid existing owners and the nonresident sentinel.");
		});
		registry.Add("Terrain.SurfaceAllocation.CancelAndStalePublishAreAtomic", [](TestContext& context) {
			const auto asset = GenerateUUID();
			std::vector<TerrainSurfacePageHandle> visible{{AllocationKey(asset, 0), 0U, 16U, 7U}};
			std::uint64_t visibleGeneration = 7U;
			std::atomic_bool cancelled{true};
			const auto cancelledPlan = PrepareTerrainSurfaceCachePlan(7U, visible, {{AllocationKey(asset, 1), 16U}}, 128U, &cancelled);
			context.Expect(!PublishTerrainSurfaceCachePlan(visible, visibleGeneration, cancelledPlan, 7U), "Cancelled worker preparation must not publish.");
			const auto stale = PrepareTerrainSurfaceCachePlan(7U, visible, {{AllocationKey(asset, 1), 16U}}, 128U);
			context.Expect(!PublishTerrainSurfaceCachePlan(visible, visibleGeneration, stale, 8U), "Stale generation must reject the complete candidate.");
			context.Expect(visible.size() == 1U && visible.front().key.area.x == 0 && visible.front().atlasTile == 0U && visibleGeneration == 7U, "Rejected publication must preserve the exact prior owner and generation.");
			context.Expect(PublishTerrainSurfaceCachePlan(visible, visibleGeneration, stale, 7U) && visible.size() == 2U, "A later valid publication must still make progress.");
		});
		registry.Add("Terrain.WeightCanonicality.TinyContributorsClearEmptyIds", [](TestContext& context) {
			const std::array<Terrain::TerrainWeightContribution, 3> contributions{{{9U, 1000U}, {1U, 999U}, {2U, 1U}}};
			ExpectCanonicalWeight(context, contributions);
		});
		registry.Add("Terrain.WeightCanonicality.QuantizedTiesUseStableIdOrder", [](TestContext& context) {
			const std::array<Terrain::TerrainWeightContribution, 4> contributions{{{9U, 1000U}, {1U, 999U}, {2U, 998U}, {3U, 997U}}};
			ExpectCanonicalWeight(context, contributions);
		});
		registry.Add("Terrain.WeightCanonicality.DuplicatesAndPermutationRetainIdentity", [](TestContext& context) {
			const std::array<Terrain::TerrainWeightContribution, 5> first{{{4U, 10U}, {2U, 10U}, {4U, 7U}, {0U, 200U}, {3U, 0U}}};
			const std::array<Terrain::TerrainWeightContribution, 5> second{{{3U, 0U}, {4U, 7U}, {0U, 200U}, {2U, 10U}, {4U, 10U}}};
			ExpectCanonicalWeight(context, first);
			context.Expect(Terrain::NormalizeTerrainWeights(first) == Terrain::NormalizeTerrainWeights(second), "Equivalent weight contributors must produce identical stable-layer slots.");
			const auto empty = Terrain::NormalizeTerrainWeights({});
			context.Expect(empty.IsCanonical() && empty == Terrain::TerrainWeightSample{}, "Empty paint must preserve the canonical all-zero representation.");
		});
	}
}
