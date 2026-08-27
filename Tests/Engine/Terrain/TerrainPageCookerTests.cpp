// TerrainPageCookerTests.cpp -- Spatial terrain-page ownership and payload contracts.

#include "TestHarness.h"

#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainRenderPackets.h"
#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace TerraEngine::Tests
{
namespace
{
	constexpr std::uint32_t kOwnedCellExtent = 256u;
	constexpr std::uint32_t kOwnedVertexExtent = 257u;
	constexpr std::uint32_t kHaloExtent = 258u;
	constexpr std::size_t kHaloSampleCount = static_cast<std::size_t>(kHaloExtent) * kHaloExtent;
	constexpr std::uint32_t kNominalPageBytes = 64u * 1024u;

	// Halo coordinates are [-1, 256]. East/north sample 256 completes
	// the final owned cells; west/south sample -1 is normal-only context.
	struct TerrainCookFixture
	{
		std::vector<std::int32_t> heightHalo;
		TerraEngine::Renderer::Terrain::TerrainCookInput input{};

		template<typename HeightFunction>
		TerrainCookFixture(TerraEngine::Terrain::TerrainAreaCoordinate area, HeightFunction&& heightFunction)
			: heightHalo(kHaloSampleCount)
		{
			for(std::int32_t haloZ = 0; haloZ < static_cast<std::int32_t>(kHaloExtent); ++haloZ)
				for(std::int32_t haloX = 0; haloX < static_cast<std::int32_t>(kHaloExtent); ++haloX)
					heightHalo[static_cast<std::size_t>(haloZ) * kHaloExtent + haloX] =
						heightFunction(haloX - 1, haloZ - 1);
			input.assetId = TerraEngine::GenerateUUID();
			input.area = area;
			input.authoredHash = {42u, 99u};
			input.authoredGeneration = 7u;
			input.sampleSpacingMetres = 1.0;
			input.heightMillimetres = heightHalo;
		}
	};

	template<typename Result>
	concept SpatialCookContract = requires(
		const typename Result::LodNode& node,
		const typename Result::MeshletRecord& meshlet)
	{
		{ node.cellOriginX } -> std::convertible_to<std::uint16_t>;
		{ node.cellOriginZ } -> std::convertible_to<std::uint16_t>;
		{ node.cellExtent } -> std::convertible_to<std::uint16_t>;
		{ node.firstMeshlet } -> std::convertible_to<std::uint32_t>;
		{ node.meshletCount } -> std::convertible_to<std::uint32_t>;
		{ meshlet.nodeId } -> std::convertible_to<std::uint32_t>;
		{ meshlet.renderedCellWidth } -> std::convertible_to<std::uint16_t>;
		{ meshlet.renderedCellHeight } -> std::convertible_to<std::uint16_t>;
		{ meshlet.vertexCount } -> std::convertible_to<std::uint32_t>;
		{ meshlet.triangleCount } -> std::convertible_to<std::uint32_t>;
	};

	template<typename Result>
	bool HasCompleteSpatialQuadtree(const Result& cooked)
	{
		if constexpr(!SpatialCookContract<Result>) return false;
		else
		{
			if(cooked.lodNodes.size() != 85u) return false;
			const std::array<std::uint16_t, 4u> expectedExtents{256u, 128u, 64u, 32u};
			const std::array<std::size_t, 4u> expectedCounts{1u, 4u, 16u, 64u};
			for(std::size_t extentIndex = 0u; extentIndex < expectedExtents.size(); ++extentIndex)
			{
				const std::uint16_t extent = expectedExtents[extentIndex];
				std::vector<std::uint8_t> coverage(kOwnedCellExtent * kOwnedCellExtent, 0u);
				std::size_t nodeCount = 0u;
				for(const auto& node : cooked.lodNodes)
				{
					if(node.cellExtent != extent) continue;
					++nodeCount;
					if(node.cellOriginX % extent != 0u || node.cellOriginZ % extent != 0u
						|| static_cast<std::uint32_t>(node.cellOriginX) + extent > kOwnedCellExtent
						|| static_cast<std::uint32_t>(node.cellOriginZ) + extent > kOwnedCellExtent
						|| static_cast<std::size_t>(node.firstMeshlet) + node.meshletCount > cooked.meshlets.size()) return false;
					for(std::uint32_t z = node.cellOriginZ; z < node.cellOriginZ + extent; ++z)
						for(std::uint32_t x = node.cellOriginX; x < node.cellOriginX + extent; ++x)
							if(++coverage[z * kOwnedCellExtent + x] != 1u) return false;
				}
				if(nodeCount != expectedCounts[extentIndex]
					|| std::any_of(coverage.begin(), coverage.end(), [](std::uint8_t count) { return count != 1u; })) return false;
			}
			for(const auto& meshlet : cooked.meshlets)
				if(meshlet.nodeId >= cooked.lodNodes.size()
					|| meshlet.renderedCellWidth == 0u || meshlet.renderedCellWidth > 7u
					|| meshlet.renderedCellHeight == 0u || meshlet.renderedCellHeight > 7u
					|| meshlet.vertexCount > 64u || meshlet.triangleCount > 98u) return false;
			return !cooked.meshlets.empty();
		}
	}

	// Independent bounded directory validation checks before slicing bytes and
	// verifies the exact per-page checksum, so malformed offsets fail safely.
	bool ValidatePageDirectory(
		const TerraEngine::Renderer::Terrain::TerrainPageCookResult& cooked,
		std::span<const std::uint8_t> payload)
	{
		if(cooked.pageRanges.empty() || payload.empty()) return false;
		std::uint64_t expectedFirstByte = 0u;
		for(std::size_t rangeIndex = 0u; rangeIndex < cooked.pageRanges.size(); ++rangeIndex)
		{
			const auto& range = cooked.pageRanges[rangeIndex];
			const std::uint64_t end = static_cast<std::uint64_t>(range.firstByte) + range.byteCount;
			if(range.pageId != rangeIndex || range.firstByte != expectedFirstByte
				|| range.byteCount == 0u || range.byteCount > kNominalPageBytes || end > payload.size()) return false;
			if(TerraEngine::Terrain::HashTerrainBytes(payload.subspan(range.firstByte, range.byteCount))
				!= range.payloadIdentity) return false;
			expectedFirstByte = end;
		}
		return expectedFirstByte == payload.size();
	}

	template<typename Result>
	concept ConservativeErrorContract = SpatialCookContract<Result> && requires(const typename Result::LodNode& node)
	{
		{ node.maximumQuantizationErrorMetres } -> std::convertible_to<float>;
		{ node.maximumMorphDisplacementMetres } -> std::convertible_to<float>;
	};

	float ReadOwnedHeightMetres(std::span<const std::int32_t> halo, std::uint32_t x, std::uint32_t z)
	{
		return static_cast<float>(halo[(z + 1u) * kHaloExtent + (x + 1u)]) / 1000.0F;
	}

	float TriangleHeight(float h00, float h10, float h01, float h11, float x, float z, bool oppositeDiagonal) noexcept
	{
		if(!oppositeDiagonal)
			return x >= z ? h00 + x * (h10 - h00) + z * (h11 - h10)
				: h00 + x * (h11 - h01) + z * (h01 - h00);
		return x + z <= 1.0F ? h00 + x * (h10 - h00) + z * (h01 - h00)
			: h11 + (1.0F - x) * (h01 - h11) + (1.0F - z) * (h10 - h11);
	}

	template<typename Result>
	bool StoresExhaustivelyConservativeError(const Result& cooked, std::span<const std::int32_t> halo)
	{
		if constexpr(!ConservativeErrorContract<Result>) return false;
		else
		{
			for(const auto& node : cooked.lodNodes)
			{
				if(node.cellExtent < 32u || node.cellExtent > 256u || node.cellExtent % 32u != 0u) return false;
				const std::uint32_t stride = node.cellExtent / 32u;
				float exhaustiveDeviation = 0.0F;
				for(std::uint32_t z = node.cellOriginZ; z <= node.cellOriginZ + node.cellExtent; ++z)
					for(std::uint32_t x = node.cellOriginX; x <= node.cellOriginX + node.cellExtent; ++x)
					{
						const std::uint32_t relativeX = x - node.cellOriginX;
						const std::uint32_t relativeZ = z - node.cellOriginZ;
						const std::uint32_t cellX = std::min(relativeX / stride, 31u);
						const std::uint32_t cellZ = std::min(relativeZ / stride, 31u);
						const std::uint32_t x0 = node.cellOriginX + cellX * stride;
						const std::uint32_t z0 = node.cellOriginZ + cellZ * stride;
						const std::uint32_t x1 = x0 + stride;
						const std::uint32_t z1 = z0 + stride;
						const float fx = static_cast<float>(x - x0) / static_cast<float>(stride);
						const float fz = static_cast<float>(z - z0) / static_cast<float>(stride);
						const float source = ReadOwnedHeightMetres(halo, x, z);
						const float h00 = ReadOwnedHeightMetres(halo, x0, z0);
						const float h10 = ReadOwnedHeightMetres(halo, x1, z0);
						const float h01 = ReadOwnedHeightMetres(halo, x0, z1);
						const float h11 = ReadOwnedHeightMetres(halo, x1, z1);
						// The smaller error for both legal diagonals is a lower bound
						// independent of the cooker's deterministic diagonal choice.
						const float first = std::abs(source - TriangleHeight(h00, h10, h01, h11, fx, fz, false));
						const float second = std::abs(source - TriangleHeight(h00, h10, h01, h11, fx, fz, true));
						exhaustiveDeviation = std::max(exhaustiveDeviation, std::min(first, second));
					}
				const float required = std::max(exhaustiveDeviation,
					static_cast<float>(node.maximumMorphDisplacementMetres))
					+ static_cast<float>(node.maximumQuantizationErrorMetres);
				if(!std::isfinite(node.bounds.maximumGeometricErrorMetres)
					|| node.bounds.maximumGeometricErrorMetres + 1.0e-5F < required) return false;
				if(node.parentNodeId != 0xffffffffu
					&& (node.parentNodeId >= cooked.lodNodes.size()
						|| cooked.lodNodes[node.parentNodeId].bounds.maximumGeometricErrorMetres + 1.0e-5F
							< node.bounds.maximumGeometricErrorMetres)) return false;
			}
			return !cooked.lodNodes.empty();
		}
	}
}

	void RegisterTerrainPageCookerTests(TestRegistry& registry)
	{
		registry.Add("Terrain.PageCooker.RequiresStrict258HaloAndOwnsFinalRowAndCorner", [](TestContext& context) {
			TerrainCookFixture fixture({3, -2}, [](std::int32_t x, std::int32_t z) {
				if(x < 0 || z < 0) return 9'000'000;
				if(x == 256 || z == 256) return x == 256 && z == 256 ? 123'456 : 100'000;
				return x * 10 + z * 7;
			});
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(fixture.input);
			std::vector<std::int32_t> oldArea(kOwnedCellExtent * kOwnedCellExtent, 0);
			auto oldInput = fixture.input;
			oldInput.heightMillimetres = oldArea;
			const auto rejectedOldShape = TerraEngine::Renderer::Terrain::CookTerrainPage(oldInput);
			const bool reachesEast = std::any_of(cooked.meshlets.begin(), cooked.meshlets.end(), [](const auto& meshlet) { return meshlet.maxXExclusive == kOwnedCellExtent; });
			const bool reachesNorth = std::any_of(cooked.meshlets.begin(), cooked.meshlets.end(), [](const auto& meshlet) { return meshlet.maxZExclusive == kOwnedCellExtent; });
			context.Expect(cooked.complete && !rejectedOldShape.complete,
				"The page cooker must accept exactly 258x258 halo heights and reject the obsolete 256x256 sample shape.");
			context.Expect(std::abs(cooked.maximumHeightMetres - 123.456F) < 0.001F && reachesEast && reachesNorth,
				"West/south normal-only halo samples must not expand owned bounds, while east/north samples complete the final row and north-east corner.");
		});

		registry.Add("Terrain.PageCooker.HaloNormalsAgreeAcrossAreaBoundary", [](TestContext& context) {
			auto curvedHeight = [](std::int32_t globalX, std::int32_t globalZ) { return globalX * globalX / 8 + globalZ * 13; };
			TerrainCookFixture west({0, 0}, [&](std::int32_t x, std::int32_t z) { return curvedHeight(x, z); });
			TerrainCookFixture east({1, 0}, [&](std::int32_t x, std::int32_t z) { return curvedHeight(256 + x, z); });
			const auto westCook = TerraEngine::Renderer::Terrain::CookTerrainPage(west.input);
			const auto eastCook = TerraEngine::Renderer::Terrain::CookTerrainPage(east.input);
			bool boundaryMatches = westCook.complete && eastCook.complete
				&& westCook.packedNormals.size() == kOwnedVertexExtent * kOwnedVertexExtent
				&& eastCook.packedNormals.size() == kOwnedVertexExtent * kOwnedVertexExtent;
			if(boundaryMatches)
				for(std::uint32_t z = 0u; z < kOwnedVertexExtent; ++z)
					if(westCook.packedNormals[z * kOwnedVertexExtent + 256u]
						!= eastCook.packedNormals[z * kOwnedVertexExtent]) { boundaryMatches = false; break; }
			context.Expect(boundaryMatches,
				"Both areas must reconstruct identical packed normals at every shared boundary vertex from overlapping halos.");
		});

		registry.Add("Terrain.PageCooker.BuildsCompleteSpatialQuadtreeAndBoundedMeshlets", [](TestContext& context) {
			TerrainCookFixture fixture({0, 0}, [](std::int32_t x, std::int32_t z) { return x * 10 + z * 7; });
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(fixture.input);
			context.Expect(cooked.complete && HasCompleteSpatialQuadtree(cooked),
				"The cooker must cover the area at 256/128/64/32 extents and partition every grid into <=7x7-cell, <=64-vertex, <=98-triangle meshlets.");
		});

		registry.Add("Terrain.PageCooker.RealPayloadHasBoundedRangesAndChecksums", [](TestContext& context) {
			TerrainCookFixture fixture({0, 0}, [](std::int32_t x, std::int32_t z) {
				return ((x * 17) ^ (z * 31)) + (x == 137 && z == 91 ? 250'000 : 0);
			});
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(fixture.input);
			std::vector<std::uint8_t> corrupted = cooked.pagePayload;
			if(!corrupted.empty()) corrupted[corrupted.size() / 2u] ^= 0x80u;
			auto badRange = cooked;
			if(!badRange.pageRanges.empty()) badRange.pageRanges.back().byteCount = std::numeric_limits<std::uint32_t>::max();
			context.Expect(cooked.complete && ValidatePageDirectory(cooked, cooked.pagePayload)
				&& !ValidatePageDirectory(cooked, corrupted) && !ValidatePageDirectory(badRange, badRange.pagePayload),
				"Real terrain pages must form bounded dense <=64 KiB ranges whose checksums reject corruption and overflowing offsets.");
		});

		registry.Add("Terrain.PageCooker.StoredErrorCoversSourceQuantizationDescendantsAndMorph", [](TestContext& context) {
			TerrainCookFixture fixture({0, 0}, [](std::int32_t x, std::int32_t z) {
				const std::int32_t noise = ((x * 73) ^ (z * 151) ^ (x * z * 3)) % 7000;
				return noise + (x == 129 && z == 127 ? 350'000 : 0);
			});
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(fixture.input);
			context.Expect(cooked.complete && StoresExhaustivelyConservativeError(cooked, fixture.heightHalo),
				"Every rounded world-metre error must cover exhaustive source deviation, quantization, morph displacement, and descendants.");
		});

		registry.Add("Terrain.PageCooker.RejectsMismatchedCookIdentityFromRenderPacket", [](TestContext& context) {
			TerrainCookFixture fixture({0, 0}, [](std::int32_t x, std::int32_t z) { return x * 10 + z * 7; });
			const auto cooked = TerraEngine::Renderer::Terrain::CookTerrainPage(fixture.input);
			TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
			view.assetId = fixture.input.assetId;
			view.authoredHash = {1u, 2u};
			view.pageSourceIdentity = cooked.sourceIdentity;
			view.authoredGeneration = fixture.input.authoredGeneration;
			view.frameId = 1u;
			const auto packet = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, cooked, {});
			context.Expect(cooked.complete && !packet.has_value(),
				"A complete page with mismatched authored identity must not publish without last-known-good fallback.");
		});
	}
}
