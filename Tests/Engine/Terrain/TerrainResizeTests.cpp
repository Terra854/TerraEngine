// TerrainResizeTests.cpp -- Bounds, policy, and candidate regressions for resize.

#include "TestHarness.h"
#include "Terrain/TerrainResize.h"
#include "TerrainResizeTestFixtures.h"

#include <algorithm>
#include <array>
#include <limits>

namespace TerraEngine::Tests
{
	void RegisterTerrainResizeTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Resize.BoundsCoverNegativePartialAreas", [](TestContext& context) {
			const auto areas = Terrain::TerrainAreasForSamples({-1, -257, 257, 1});
			const auto first = Terrain::TerrainActiveRectForArea({-1, -257, 257, 1}, {-1, -2});
			context.Expect(areas && *areas == Terrain::TerrainAreaBounds{-1, -2, 2, 1},
				"Signed sample bounds must convert to the exact half-open area range.");
			context.Expect(first && first->minX == 255u && first->minZ == 255u,
				"Partial negative edge areas must expose only their authored local tail.");
		});

		registry.Add("Terrain.Resize.CheckedCoordinateMathRejectsOverflow", [](TestContext& context) {
			bool overflow = false;
			context.Expect(Terrain::FloorDivideTerrainCoordinate(-257, 256, overflow) == -2 && !overflow,
				"Negative terrain coordinates must use mathematical floor division.");
			context.Expect(Terrain::FloorModuloTerrainCoordinate(-257, 256, overflow) == 255 && !overflow,
				"Negative terrain coordinates must produce a non-negative local sample.");
			const Terrain::TerrainAreaBounds enormous{(std::numeric_limits<std::int64_t>::min)(), 0,
				(std::numeric_limits<std::int64_t>::max)(), 2};
			context.Expect(!enormous.AreaCount().has_value(),
				"An area range whose product cannot fit size_t must reject before allocation.");
			context.Expect(!Terrain::EnumerateTerrainAreas({(std::numeric_limits<std::int64_t>::min)(), 0,
				(std::numeric_limits<std::int64_t>::max)(), 1}, 64u),
				"Root-driven enumeration must reject an extreme extent before incrementing through it.");
		});

		registry.Add("Terrain.Resize.ActiveRectClipsAllFourSides", [](TestContext& context) {
			const Terrain::TerrainSampleBounds bounds{-250, -240, 260, 270};
			const auto minimum = Terrain::TerrainActiveRectForArea(bounds, {-1, -1});
			const auto maximum = Terrain::TerrainActiveRectForArea(bounds, {1, 1});
			context.Expect(minimum && *minimum == Terrain::TerrainAreaActiveRect{6u, 16u, 256u, 256u},
				"Negative partial areas must expose the exact local retained rectangle.");
			context.Expect(maximum && *maximum == Terrain::TerrainAreaActiveRect{0u, 0u, 4u, 14u},
				"Positive partial areas must expose the exact local retained rectangle.");
		});

		registry.Add("Terrain.Resize.RejectsMixedMove", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.authoredBounds = {0, 0, 256, 256};
			Terrain::TerrainResizeDelta delta{};
			delta.previousBounds = root.authoredBounds;
			delta.proposedBounds = {-256, 0, 128, 256};
			context.Expect(!Terrain::ValidateTerrainResizeDelta(delta, root).Succeeded(),
				"One resize may not hide data on one side while adding data on another.");
		});

		registry.Add("Terrain.Resize.PreviewRegionsPartitionExpansionAndCrop", [](TestContext& context) {
			Terrain::TerrainResizeDelta delta{};
			delta.previousBounds = {0, 0, 512, 512};
			delta.proposedBounds = {-256, -512, 768, 1024};
			const auto expansion = Terrain::BuildTerrainResizePreviewRegions(delta);
			context.ExpectEqual(static_cast<std::size_t>(5u), expansion.size(),
				"Four non-overlapping expansion strips plus retained coverage should drive the viewport overlay.");
			context.Expect(expansion.front() == Terrain::TerrainResizePreviewRegion{
				{0, 0, 512, 512}, Terrain::TerrainResizePreviewRegionKind::Retained},
				"Expansion preview must preserve the exact previous footprint as retained coverage.");
			std::uint64_t changedSamples = 0u;
			for(const auto& region : expansion)
				if(region.kind == Terrain::TerrainResizePreviewRegionKind::Changed)
					changedSamples += static_cast<std::uint64_t>(region.bounds.maxXExclusive - region.bounds.minX)
						* static_cast<std::uint64_t>(region.bounds.maxZExclusive - region.bounds.minZ);
			context.ExpectEqual(static_cast<std::uint64_t>(1'310'720u), changedSamples,
				"Expansion strips must cover outer minus retained exactly once without translucent overlap.");

			delta.kind = Terrain::TerrainResizeKind::Crop;
			delta.previousBounds = {-256, -512, 768, 1024};
			delta.proposedBounds = {0, 0, 512, 512};
			const auto crop = Terrain::BuildTerrainResizePreviewRegions(delta);
			context.Expect(crop == expansion,
				"Crop must reuse the same footprint partition while presentation changes blue expansion to red discard.");

			delta.kind = Terrain::TerrainResizeKind::Expand;
			delta.previousBounds = {0, 0, 1024, 1024};
			delta.proposedBounds = {-512, -512, 1536, 1536};
			const auto grid = Terrain::BuildTerrainResizePreviewGrid(delta, 64u);
			const auto containsX = [&grid](std::int64_t coordinate) {
				return std::binary_search(grid.sampleX.begin(), grid.sampleX.end(), coordinate);
			};
			context.Expect(grid.sampleX.size() <= 69u && grid.sampleZ.size() <= 69u,
				"Adaptive resize mesh axes must remain bounded by their segment budget plus exact seam samples.");
			context.Expect(grid.sampleX.front() == -512 && grid.sampleX.back() == 1535
				&& containsX(-1) && containsX(0) && containsX(1023) && containsX(1024),
				"Adaptive resize mesh axes must preserve the proposal edges and both sides of the former boundary.");
			context.Expect(Terrain::BuildTerrainResizePreviewGrid(delta, 0u).sampleX.empty(),
				"A zero presentation budget must fail closed without allocating preview mesh coordinates.");
		});

		registry.Add("Terrain.Resize.EdgeBlendIsGlobalAndDeterministic", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.authoredBounds = {0, 0, 4, 4};
			root.defaultHeightMm = 0;
			Terrain::TerrainResizeDelta delta{};
			delta.previousBounds = root.authoredBounds;
			delta.proposedBounds = {-4, -4, 8, 8};
			delta.transitionSamples = 4u;
			const auto sample = [](Terrain::TerrainSampleCoordinate coordinate) -> std::optional<std::int32_t> {
				return static_cast<std::int32_t>(coordinate.x * 10 + coordinate.z * 20);
			};
			const auto first = Terrain::InitializeExpandedTerrainHeight(root, delta, {-1, -1}, sample);
			const auto second = Terrain::InitializeExpandedTerrainHeight(root, delta, {-1, -1}, sample);
			context.Expect(first == second && first != root.defaultHeightMm,
				"Edge Blend must evaluate corners directly from one immutable source in global coordinates.");
		});

		registry.Add("Terrain.Resize.ExpansionPoliciesHandleUnevenEdgesAndCorners", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.authoredBounds = {0, 0, 4, 4};
			root.defaultHeightMm = -100;
			root.defaultLayerId = 7u;
			Terrain::TerrainResizeDelta delta{};
			delta.previousBounds = root.authoredBounds;
			delta.proposedBounds = {-4, -4, 8, 8};
			delta.transitionSamples = 4u;
			const auto uneven = [](Terrain::TerrainSampleCoordinate coordinate) -> std::optional<std::int32_t> {
				return static_cast<std::int32_t>(1000 + coordinate.x * 100 + coordinate.z * 40);
			};
			delta.expansionPolicy = Terrain::TerrainExpansionPolicy::ExtendEdge;
			context.ExpectEqual(Terrain::InitializeExpandedTerrainHeight(root, delta, {4, 2}, uneven),
				*uneven({3, 2}), "Extend Edge must copy the nearest uneven boundary profile exactly.");
			delta.expansionPolicy = Terrain::TerrainExpansionPolicy::FlatDefault;
			context.ExpectEqual(Terrain::InitializeExpandedTerrainHeight(root, delta, {4, 2}, uneven), -100,
				"Flat Default must remain independent of the modified authored edge.");
			delta.expansionPolicy = Terrain::TerrainExpansionPolicy::EdgeBlend;
			const auto xz = Terrain::InitializeExpandedTerrainHeight(root, delta, {-1, -2}, uneven);
			const auto zx = Terrain::InitializeExpandedTerrainHeight(root, delta, {-2, -1},
				[](Terrain::TerrainSampleCoordinate coordinate) -> std::optional<std::int32_t> {
					return static_cast<std::int32_t>(1000 + coordinate.x * 40 + coordinate.z * 100);
				});
			context.ExpectEqual(xz, zx, "Corner blending must be symmetric rather than depending on side pass order.");
		});

		registry.Add("Terrain.Resize.SurfaceNormalizationAndBoundaryHolePolicyAreExplicit", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.authoredBounds = {0, 0, 4, 4};
			root.defaultLayerId = 1u;
			Terrain::TerrainResizeDelta delta{};
			delta.previousBounds = root.authoredBounds;
			delta.proposedBounds = {0, 0, 8, 4};
			delta.transitionSamples = 4u;
			const std::array<Terrain::TerrainWeightContribution, 1u> painted{{{9u, 255u}}};
			const auto boundarySurface = Terrain::NormalizeTerrainWeights(painted);
			const auto surface = Terrain::InitializeExpandedTerrainSurface(root, delta, {4, 2},
				[boundarySurface](Terrain::TerrainSampleCoordinate) { return std::optional{boundarySurface}; });
			std::uint32_t total = 0u;
			for(const auto& slot : surface.slots) total += slot.weight;
			context.Expect(surface.IsCanonical() && total == 255u,
				"Edge surface blending must produce one stable normalized authored sample.");
			delta.holePolicy = Terrain::TerrainBoundaryHolePolicy::CloseAtFormerEdge;
			context.Expect(!Terrain::InitializeExpandedTerrainHole(delta, {4, 2}, [](auto) { return true; }),
				"New coverage must remain solid unless connected boundary holes are explicitly extended.");
			delta.holePolicy = Terrain::TerrainBoundaryHolePolicy::ExtendConnectedBoundaryHoles;
			context.Expect(Terrain::InitializeExpandedTerrainHole(delta, {4, 2}, [](auto) { return true; }),
				"The explicit extension policy must continue a hole touching the old boundary.");
		});

		registry.Add("Terrain.Resize.CropCanonicalizesInactiveTailsWithoutResurrection", [](TestContext& context) {
			std::vector<std::int32_t> heights(Terrain::kTerrainAreaSampleCount);
			for(std::uint32_t z = 0u; z < 256u; ++z)
				for(std::uint32_t x = 0u; x < 256u; ++x)
					heights[static_cast<std::size_t>(z) * 256u + x] = static_cast<std::int32_t>(z * 1000u + x);
			std::vector<std::uint8_t> holes(Terrain::kTerrainAreaSampleCount / 8u, 0xffu);
			const Terrain::TerrainAreaActiveRect active{8u, 9u, 20u, 30u};
			const auto croppedHeights = Terrain::CanonicalizeCroppedTerrainHeights(heights, active);
			const auto croppedHoles = Terrain::CanonicalizeCroppedTerrainHoles(holes, active);
			context.Expect(croppedHeights.size() == Terrain::kTerrainAreaSampleCount
				&& croppedHeights.front() == heights[9u * 256u + 8u]
				&& croppedHeights.back() == heights[29u * 256u + 19u],
				"Crop must preserve retained samples and edge-clamp every inactive height tail.");
			const auto holeAt = [&croppedHoles](std::uint32_t x, std::uint32_t z) {
				return ((croppedHoles[(static_cast<std::size_t>(z) * 256u + x) / 8u] >> (x & 7u)) & 1u) != 0u;
			};
			context.Expect(holeAt(8u, 9u) && !holeAt(0u, 0u) && !holeAt(255u, 255u),
				"Crop must exclude holes from inactive storage so re-expansion cannot resurrect them.");
			const std::array<Terrain::TerrainWeightContribution, 1u> painted{{{9u, 255u}}};
			std::vector<Terrain::TerrainWeightSample> surfaces(Terrain::kTerrainAreaSampleCount,
				Terrain::NormalizeTerrainWeights(painted));
			const auto croppedSurfaces = Terrain::CanonicalizeCroppedTerrainSurfaces(surfaces, active, 1u);
			context.Expect(croppedSurfaces.front().slots[0].layerId == 1u
				&& croppedSurfaces[9u * 256u + 8u].slots[0].layerId == 9u,
				"Inactive crop surface controls must become the default while retained paint stays exact.");
		});

		registry.Add("Terrain.Resize.CandidateAppliesBoundsAndMembershipOnce", [](TestContext& context) {
			auto root = std::make_shared<Terrain::TerrainAssetRoot>();
			root->assetUuid = TerraEngine::GenerateUUID();
			root->publicationGeneration = 4u;
			root->persistedRootGeneration = 1u;
			root->rootContentHash = {1u, 2u};
			root->authoredBounds = {0, 0, 512, 256};
			root->sampleSpacingMetres = 1.0;
			root->defaultLayerId = 1u;
			root->layerStackGeneration = 1u;
			Terrain::TerrainAreaDescriptor kept{};
			kept.coordinate = {0, 0}; kept.editGeneration = 1u; kept.authoredContentHash = {3u, 4u};
			Terrain::TerrainAreaDescriptor removed = kept;
			removed.coordinate = {1, 0}; removed.authoredContentHash = {5u, 6u};
			Terrain::TerrainDirectoryShard shard{};
			shard.areaBounds = {0, 0, 2, 1};
			shard.descriptors.emplace(kept.coordinate, kept);
			shard.descriptors.emplace(removed.coordinate, removed);
			root->directoryShards.push_back(std::move(shard));
			Terrain::TerrainAssetStore store(root);
			std::shared_ptr<const Terrain::TerrainAssetRoot> candidate;
			const auto result = store.BuildResizeCandidate(*root, {0, 0, 256, 256}, {}, {{1, 0}}, candidate);
			context.Expect(result.Succeeded() && candidate && candidate->publicationGeneration == 5u
				&& candidate->authoredBounds == Terrain::TerrainSampleBounds{0, 0, 256, 256},
				"A crop candidate must apply its exact bounds in one new generation.");
			context.Expect(candidate && !store.FindDescriptor(*candidate, {1, 0}),
				"Removed terrain descriptors must not remain current after a crop candidate.");
		});

		registry.Add("Terrain.Resize.ProposalFingerprintBindsRootBoundsAndBudget", [](TestContext& context) {
			auto root = TerrainResizeFixtures::MakeRoot();
			Terrain::TerrainResizeRequest request{};
			request.transactionId = TerraEngine::GenerateUUID();
			request.targetAssetUuid = root->assetUuid;
			request.expectedRootHash = root->rootContentHash;
			request.expectedRootGeneration = root->publicationGeneration;
			request.delta.previousBounds = root->authoredBounds;
			request.delta.proposedBounds = {-256, 0, 256, 256};
			const auto first = Terrain::PreviewTerrainResize(*root, request);
			request.budget.maximumPreparedBytes += 1u;
			const auto second = Terrain::PreviewTerrainResize(*root, request);
			context.Expect(first.Succeeded() && second.Succeeded()
				&& first.proposalFingerprint != second.proposalFingerprint,
				"Resize approval must be invalidated when a canonical budget or proposal input changes.");
		});
	}
}
