// TerrainPageCooker.cpp -- Minimal deterministic base terrain-page cooker.

#include "Renderer/Terrain/TerrainPageCooker.h"

#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace TerraEngine::Renderer::Terrain
{
namespace
{
	std::uint16_t PackNormal(float x, float z)
	{
		const float length = std::sqrt(x * x + 1.0f + z * z);
		const auto pack = [](float value) { return static_cast<std::uint16_t>(std::clamp((value * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f)); };
		return static_cast<std::uint16_t>(pack(x / length) | (pack(z / length) << 8u));
	}

	void AppendI32(std::vector<std::uint8_t>& bytes, std::int32_t value)
	{
		for (std::uint32_t shift = 0u; shift < 32u; shift += 8u) bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(value) >> shift));
	}

	void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
	{
		for (std::uint32_t shift = 0u; shift < 64u; shift += 8u) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}

	bool IsHoleCell(std::span<const std::uint8_t> bits, std::uint32_t x, std::uint32_t z) noexcept
	{
		if (bits.empty() || x >= 256u || z >= 256u) return false;
		const std::uint32_t bit = z * 256u + x;
		return (bits[bit / 8u] & static_cast<std::uint8_t>(1u << (bit % 8u))) != 0u;
	}
}

	bool AreTerrainLodLevelsCompatible(std::uint16_t first, std::uint16_t second) noexcept
	{
		return first > second ? first - second <= 1u : second - first <= 1u;
	}

	TerrainPageCookResult CookTerrainPage(const TerrainCookInput& input)
	{
		TerrainPageCookResult result{};
		if (input.assetId == TerraEngine::EMPTY_UUID || !input.authoredHash.IsValid() || input.authoredGeneration == 0u || !std::isfinite(input.sampleSpacingMetres) || input.sampleSpacingMetres <= 0.0 || input.heightMillimetres.size() != TerraEngine::Terrain::kTerrainAreaSampleCount || (!input.holeCellBits.empty() && input.holeCellBits.size() != 8192u))
		{
			result.diagnostic = "Terrain cooker requires one complete authored 256 by 256 height area and a stable source identity.";
			return result;
		}
		result.assetId = input.assetId;
		result.area = input.area;
		result.authoredHash = input.authoredHash;
		result.authoredGeneration = input.authoredGeneration;
		std::vector<std::uint8_t> neighborIdentityBytes;
		neighborIdentityBytes.reserve(input.neighborAuthoredHashes.size() * 16u);
		for (const auto neighbor : input.neighborAuthoredHashes)
		{
			AppendU64(neighborIdentityBytes, neighbor.low64);
			AppendU64(neighborIdentityBytes, neighbor.high64);
		}
		result.neighborDependencyHash = TerraEngine::Terrain::HashTerrainBytes(neighborIdentityBytes);
		const auto [minimum, maximum] = std::minmax_element(input.heightMillimetres.begin(), input.heightMillimetres.end());
		result.minimumHeightMetres = static_cast<float>(*minimum) / 1000.0f;
		result.maximumHeightMetres = static_cast<float>(*maximum) / 1000.0f;
		// Build a complete min/max/error hierarchy before meshlets. Each parent
		// conservatively contains its four children, so selection can safely retain
		// it when a finer page is unavailable instead of inventing a new bound.
		TerrainPageCookResult::ErrorPyramidLevel baseLevel{};
		baseLevel.width = 256u;
		baseLevel.height = 256u;
		baseLevel.errors.assign(TerraEngine::Terrain::kTerrainAreaSampleCount, 0.0f);
		baseLevel.bounds.reserve(TerraEngine::Terrain::kTerrainAreaSampleCount);
		for (const std::int32_t height : input.heightMillimetres)
		{
			const float metres = static_cast<float>(height) / 1000.0f;
			baseLevel.bounds.push_back({metres, metres, 0.0f});
		}
		result.errorPyramid.push_back(std::move(baseLevel));
		while (result.errorPyramid.back().width > 1u || result.errorPyramid.back().height > 1u)
		{
			const auto& child = result.errorPyramid.back();
			TerrainPageCookResult::ErrorPyramidLevel parent{};
			parent.width = static_cast<std::uint16_t>((child.width + 1u) / 2u);
			parent.height = static_cast<std::uint16_t>((child.height + 1u) / 2u);
			parent.errors.resize(static_cast<std::size_t>(parent.width) * parent.height);
			parent.bounds.resize(parent.errors.size());
			for (std::uint16_t z = 0u; z < parent.height; ++z)
				for (std::uint16_t x = 0u; x < parent.width; ++x)
				{
					TerrainPageCookResult::ConservativeBounds bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f};
					for (std::uint16_t childZ = static_cast<std::uint16_t>(z * 2u); childZ < std::min<std::uint16_t>(static_cast<std::uint16_t>(z * 2u + 2u), child.height); ++childZ)
						for (std::uint16_t childX = static_cast<std::uint16_t>(x * 2u); childX < std::min<std::uint16_t>(static_cast<std::uint16_t>(x * 2u + 2u), child.width); ++childX)
						{
							const auto& childBounds = child.bounds[static_cast<std::size_t>(childZ) * child.width + childX];
							bounds.minimumHeightMetres = std::min(bounds.minimumHeightMetres, childBounds.minimumHeightMetres);
							bounds.maximumHeightMetres = std::max(bounds.maximumHeightMetres, childBounds.maximumHeightMetres);
							bounds.maximumGeometricErrorMetres = std::max(bounds.maximumGeometricErrorMetres, childBounds.maximumGeometricErrorMetres);
						}
					bounds.maximumGeometricErrorMetres = std::max(bounds.maximumGeometricErrorMetres, (bounds.maximumHeightMetres - bounds.minimumHeightMetres) * 0.5f);
					parent.bounds[static_cast<std::size_t>(z) * parent.width + x] = bounds;
					parent.errors[static_cast<std::size_t>(z) * parent.width + x] = bounds.maximumGeometricErrorMetres;
					result.maximumGeometricErrorMetres = std::max(result.maximumGeometricErrorMetres, bounds.maximumGeometricErrorMetres);
				}
			result.errorPyramid.push_back(std::move(parent));
		}
		// Fixed 16 by 16 cell blocks form deterministic base meshlet/cluster
		// regions. A later hierarchy pass can merge these values without changing
		// their source identity or requiring a live renderer query.
		for (std::uint32_t meshletZ = 0u; meshletZ < 16u; ++meshletZ)
		{
			for (std::uint32_t meshletX = 0u; meshletX < 16u; ++meshletX)
			{
				const std::uint32_t minX = meshletX * 16u;
				const std::uint32_t minZ = meshletZ * 16u;
				std::int32_t minimumMm = std::numeric_limits<std::int32_t>::max();
				std::int32_t maximumMm = std::numeric_limits<std::int32_t>::min();
				bool holeBoundary = false;
				for (std::uint32_t z = minZ; z <= std::min(minZ + 16u, 255u); ++z)
					for (std::uint32_t x = minX; x <= std::min(minX + 16u, 255u); ++x)
					{
						const std::int32_t value = input.heightMillimetres[z * 256u + x];
						minimumMm = std::min(minimumMm, value);
						maximumMm = std::max(maximumMm, value);
					}
				for (std::uint32_t z = minZ; z < std::min(minZ + 16u, 256u); ++z)
					for (std::uint32_t x = minX; x < std::min(minX + 16u, 256u); ++x)
					{
						const bool hole = IsHoleCell(input.holeCellBits, x, z);
						if (hole != IsHoleCell(input.holeCellBits, x == 0u ? x : x - 1u, z)
							|| hole != IsHoleCell(input.holeCellBits, x, z == 0u ? z : z - 1u)) holeBoundary = true;
					}
				const std::uint32_t firstByte = static_cast<std::uint32_t>((minZ * 256u + minX) * sizeof(std::int32_t));
				result.meshlets.push_back({static_cast<std::uint32_t>(result.meshlets.size()), static_cast<std::uint16_t>(minX), static_cast<std::uint16_t>(minZ), static_cast<std::uint16_t>(std::min(minX + 17u, 256u)), static_cast<std::uint16_t>(std::min(minZ + 17u, 256u)), {static_cast<float>(minimumMm) / 1000.0f, static_cast<float>(maximumMm) / 1000.0f, (static_cast<float>(maximumMm - minimumMm) / 2000.0f)}, firstByte, 17u * 17u * sizeof(std::int32_t), holeBoundary});
			}
		}
		result.packedNormals.resize(TerraEngine::Terrain::kTerrainAreaSampleCount);
		result.pagePayload.reserve(TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(std::int32_t));
		for (std::uint32_t z = 0u; z < static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples); ++z)
		{
			for (std::uint32_t x = 0u; x < static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples); ++x)
			{
				const auto sample = [&, x, z](std::uint32_t sampleX, std::uint32_t sampleZ) { return input.heightMillimetres[sampleZ * static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples) + sampleX]; };
				const std::uint32_t left = x == 0u ? x : x - 1u;
				const std::uint32_t right = std::min(x + 1u, static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples - 1));
				const std::uint32_t down = z == 0u ? z : z - 1u;
				const std::uint32_t up = std::min(z + 1u, static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples - 1));
				const float dx = static_cast<float>(sample(right, z) - sample(left, z)) / static_cast<float>(1000.0 * input.sampleSpacingMetres);
				const float dz = static_cast<float>(sample(x, up) - sample(x, down)) / static_cast<float>(1000.0 * input.sampleSpacingMetres);
				result.packedNormals[z * static_cast<std::uint32_t>(TerraEngine::Terrain::kTerrainAreaExtentSamples) + x] = PackNormal(-dx, -dz);
				AppendI32(result.pagePayload, sample(x, z));
				result.maximumGeometricErrorMetres = std::max(result.maximumGeometricErrorMetres, std::abs(dx) + std::abs(dz));
			}
		}
		// A page is derived from both its authored samples and its seam neighbors.
		// Hashing both values prevents a seam-only edit from reusing an old stitch.
		std::vector<std::uint8_t> sourceIdentityBytes = result.pagePayload;
		sourceIdentityBytes.insert(sourceIdentityBytes.end(), input.holeCellBits.begin(), input.holeCellBits.end());
		sourceIdentityBytes.insert(sourceIdentityBytes.end(), neighborIdentityBytes.begin(), neighborIdentityBytes.end());
		result.sourceIdentity = TerraEngine::Terrain::HashTerrainBytes(sourceIdentityBytes);
		// One node per pyramid level exposes a stable coarse-to-fine morph chain.
		// Per-node bounds come directly from the already conservative hierarchy.
		for (std::uint16_t level = 0u; level < result.errorPyramid.size(); ++level)
		{
			const auto& pyramid = result.errorPyramid[level];
			const auto& bounds = pyramid.bounds.front();
			const std::uint32_t parent = level + 1u < result.errorPyramid.size() ? static_cast<std::uint32_t>(level + 1u) : 0xffffffffu;
			const float morphStart = std::max(0.01f, bounds.maximumGeometricErrorMetres * 32.0f);
			const bool refinesHoleBoundary = std::any_of(result.meshlets.begin(), result.meshlets.end(), [](const auto& meshlet) { return meshlet.hasHoleBoundary; });
			result.lodNodes.push_back({level, parent, level, bounds, morphStart, morphStart * 1.25f, refinesHoleBoundary});
		}
		// Border meshlets receive variants for same level and the two legal
		// neighboring levels. Missing neighbors use the coarser conservative edge.
		for (const auto& meshlet : result.meshlets)
		{
			const std::array<std::pair<TerrainPageEdge, bool>, 4u> edges = {{
				{TerrainPageEdge::West, meshlet.minX == 0u}, {TerrainPageEdge::East, meshlet.maxXExclusive == 256u},
				{TerrainPageEdge::South, meshlet.minZ == 0u}, {TerrainPageEdge::North, meshlet.maxZExclusive == 256u}
			}};
			for (std::size_t edge = 0u; edge < edges.size(); ++edge)
			{
				if (!edges[edge].second) continue;
				const bool missingNeighbor = !input.neighborAuthoredHashes[edge].IsValid();
				for (const std::int8_t delta : {-1, 0, 1})
					result.edgeStitchVariants.push_back({meshlet.meshletId, edges[edge].first, delta, missingNeighbor && delta >= 0});
			}
		}
		for (std::uint32_t group = 0u; group < 16u; ++group)
		{
			TerrainPageCookResult::ConservativeBounds bounds{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f};
			for (std::uint32_t meshlet = group * 16u; meshlet < group * 16u + 16u; ++meshlet)
			{
				const auto& childBounds = result.meshlets[meshlet].bounds;
				bounds.minimumHeightMetres = std::min(bounds.minimumHeightMetres, childBounds.minimumHeightMetres);
				bounds.maximumHeightMetres = std::max(bounds.maximumHeightMetres, childBounds.maximumHeightMetres);
				bounds.maximumGeometricErrorMetres = std::max(bounds.maximumGeometricErrorMetres, childBounds.maximumGeometricErrorMetres);
			}
			result.clusters.push_back({group, 16u, group * 16u, 16u, bounds});
		}
		result.clusters.push_back({16u, 0xffffffffu, 0u, 256u, {result.minimumHeightMetres, result.maximumHeightMetres, result.maximumGeometricErrorMetres}});
		result.pageRanges.push_back({0u, 0u, static_cast<std::uint32_t>(result.pagePayload.size()), result.sourceIdentity});
		result.complete = result.sourceIdentity.IsValid();
		result.diagnostic = result.complete ? "Cooked deterministic terrain heights, normals, conservative bounds, and a base page payload." : "Terrain page identity hash was empty.";
		return result;
	}
}
