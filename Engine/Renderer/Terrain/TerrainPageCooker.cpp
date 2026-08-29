// TerrainPageCooker.cpp -- Deterministic spatial TERRAROOT1 terrain-page cooker.

#include "Renderer/Terrain/TerrainPageCooker.h"

#include "Renderer/Terrain/TerrainPageFormat.h"
#include "Terrain/TerrainAreaCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>

namespace TerraEngine::Renderer::Terrain
{
namespace
{
	constexpr std::uint32_t kOwnedCellExtent = 256u;
	constexpr std::uint32_t kOwnedVertexExtent = 257u;
	constexpr std::uint32_t kHaloExtent = 258u;
	constexpr std::uint32_t kInvalidNode = 0xffffffffu;

	std::uint16_t PackNormal(float x, float z)
	{
		const float length = std::sqrt(x * x + 1.0F + z * z);
		const auto pack = [](float value) {
			return static_cast<std::uint16_t>(std::clamp(
				(value * 0.5F + 0.5F) * 255.0F, 0.0F, 255.0F));
		};
		return static_cast<std::uint16_t>(pack(x / length) | (pack(z / length) << 8u));
	}

	void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
	{
		bytes.push_back(static_cast<std::uint8_t>(value));
		bytes.push_back(static_cast<std::uint8_t>(value >> 8u));
	}

	void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
	{
		for(std::uint32_t shift = 0u; shift < 32u; shift += 8u)
			bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}

	void AppendI32(std::vector<std::uint8_t>& bytes, std::int32_t value)
	{
		AppendU32(bytes, static_cast<std::uint32_t>(value));
	}

	void AppendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
	{
		for(std::uint32_t shift = 0u; shift < 64u; shift += 8u)
			bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}

	void AppendF32(std::vector<std::uint8_t>& bytes, float value)
	{
		AppendU32(bytes, std::bit_cast<std::uint32_t>(value));
	}

	std::int32_t ReadOwnedHeight(
		std::span<const std::int32_t> halo, std::uint32_t x, std::uint32_t z) noexcept
	{
		return halo[static_cast<std::size_t>(z + 1u) * kHaloExtent + x + 1u];
	}

	float ReadOwnedHeightMetres(
		std::span<const std::int32_t> halo, std::uint32_t x, std::uint32_t z) noexcept
	{
		return static_cast<float>(ReadOwnedHeight(halo, x, z)) / 1000.0F;
	}

	bool IsHoleCell(std::span<const std::uint8_t> bits, std::uint32_t x, std::uint32_t z) noexcept
	{
		if(bits.empty() || x >= kOwnedCellExtent || z >= kOwnedCellExtent) return false;
		const std::uint32_t bit = z * kOwnedCellExtent + x;
		return (bits[bit / 8u] & static_cast<std::uint8_t>(1u << (bit % 8u))) != 0u;
	}

	bool ContainsHoleBoundary(
		std::span<const std::uint8_t> bits,
		std::uint32_t originX,
		std::uint32_t originZ,
		std::uint32_t extent) noexcept
	{
		if(bits.empty()) return false;
		for(std::uint32_t z = originZ; z < originZ + extent; ++z)
		{
			for(std::uint32_t x = originX; x < originX + extent; ++x)
			{
				const bool hole = IsHoleCell(bits, x, z);
				if((x > 0u && hole != IsHoleCell(bits, x - 1u, z))
					|| (x + 1u < kOwnedCellExtent && hole != IsHoleCell(bits, x + 1u, z))
					|| (z > 0u && hole != IsHoleCell(bits, x, z - 1u))
					|| (z + 1u < kOwnedCellExtent && hole != IsHoleCell(bits, x, z + 1u)))
					return true;
			}
		}
		return false;
	}

	float TriangleHeight(
		float h00, float h10, float h01, float h11,
		float x, float z, bool oppositeDiagonal) noexcept
	{
		if(!oppositeDiagonal)
			return x >= z ? h00 + x * (h10 - h00) + z * (h11 - h10)
				: h00 + x * (h11 - h01) + z * (h01 - h00);
		return x + z <= 1.0F ? h00 + x * (h10 - h00) + z * (h01 - h00)
			: h11 + (1.0F - x) * (h01 - h11) + (1.0F - z) * (h10 - h11);
	}

	float ComputeConservativeDeviation(
		std::span<const std::int32_t> halo,
		std::uint32_t originX,
		std::uint32_t originZ,
		std::uint32_t extent)
	{
		const std::uint32_t stride = extent / Format::kRenderedNodeCellExtent;
		float maximumDeviation = 0.0F;
		for(std::uint32_t z = originZ; z <= originZ + extent; ++z)
		{
			for(std::uint32_t x = originX; x <= originX + extent; ++x)
			{
				const std::uint32_t relativeX = x - originX;
				const std::uint32_t relativeZ = z - originZ;
				const std::uint32_t cellX = std::min(relativeX / stride, 31u);
				const std::uint32_t cellZ = std::min(relativeZ / stride, 31u);
				const std::uint32_t x0 = originX + cellX * stride;
				const std::uint32_t z0 = originZ + cellZ * stride;
				const std::uint32_t x1 = x0 + stride;
				const std::uint32_t z1 = z0 + stride;
				const float fx = static_cast<float>(x - x0) / static_cast<float>(stride);
				const float fz = static_cast<float>(z - z0) / static_cast<float>(stride);
				const float source = ReadOwnedHeightMetres(halo, x, z);
				const float h00 = ReadOwnedHeightMetres(halo, x0, z0);
				const float h10 = ReadOwnedHeightMetres(halo, x1, z0);
				const float h01 = ReadOwnedHeightMetres(halo, x0, z1);
				const float h11 = ReadOwnedHeightMetres(halo, x1, z1);
				const float first = std::abs(source - TriangleHeight(h00, h10, h01, h11, fx, fz, false));
				const float second = std::abs(source - TriangleHeight(h00, h10, h01, h11, fx, fz, true));
				maximumDeviation = std::max(maximumDeviation, std::min(first, second));
			}
		}
		return maximumDeviation;
	}

	std::uint32_t FindParentNode(
		const std::vector<TerrainPageCookResult::LodNode>& nodes,
		std::uint32_t originX,
		std::uint32_t originZ,
		std::uint32_t extent) noexcept
	{
		for(const auto& node : nodes)
		{
			if(node.cellExtent == extent * 2u
				&& originX >= node.cellOriginX && originZ >= node.cellOriginZ
				&& originX < node.cellOriginX + node.cellExtent
				&& originZ < node.cellOriginZ + node.cellExtent)
				return node.nodeId;
		}
		return kInvalidNode;
	}
}

	bool AreTerrainLodLevelsCompatible(std::uint16_t first, std::uint16_t second) noexcept
	{
		return first > second ? first - second <= 1u : second - first <= 1u;
	}

	TerrainPageCookResult CookTerrainPage(const TerrainCookInput& input)
	{
		TerrainPageCookResult result{};
		if(input.assetId == TerraEngine::EMPTY_UUID || !input.authoredHash.IsValid()
			|| input.authoredGeneration == 0u || !std::isfinite(input.sampleSpacingMetres)
			|| input.sampleSpacingMetres <= 0.0 || !input.activeRect.IsValid()
			|| input.heightMillimetres.size() != static_cast<std::size_t>(kHaloExtent) * kHaloExtent
			|| (!input.holeCellBits.empty() && input.holeCellBits.size() != 8192u))
		{
			result.diagnostic = "TERRAROOT1 cooking requires an exact 258 by 258 halo and stable authored identity.";
			return result;
		}

		result.assetId = input.assetId;
		result.area = input.area;
		result.authoredHash = input.authoredHash;
		result.authoredGeneration = input.authoredGeneration;
		// Canonical storage remains a full area, while inactive partial-edge cells
		// are holes in every derived LOD. This prevents cropped padding from being
		// rendered, picked, or shadowed without changing persisted channel layout.
		std::vector<std::uint8_t> activeHoleStorage(8192u, 0u);
		if (!input.holeCellBits.empty())
			std::copy(input.holeCellBits.begin(), input.holeCellBits.end(), activeHoleStorage.begin());
		for (std::uint32_t z = 0u; z < kOwnedCellExtent; ++z)
			for (std::uint32_t x = 0u; x < kOwnedCellExtent; ++x)
				if (x < input.activeRect.minX || z < input.activeRect.minZ
					|| x >= input.activeRect.maxXExclusive || z >= input.activeRect.maxZExclusive)
				{
					const std::uint32_t bit = z * kOwnedCellExtent + x;
					activeHoleStorage[bit / 8u] |= static_cast<std::uint8_t>(1u << (bit % 8u));
				}
		const std::span<const std::uint8_t> activeHoleBits(activeHoleStorage);
		std::vector<std::uint8_t> neighborBytes;
		for(const auto& hash : input.neighborAuthoredHashes)
		{
			AppendU64(neighborBytes, hash.low64);
			AppendU64(neighborBytes, hash.high64);
		}
		result.neighborDependencyHash = TerraEngine::Terrain::HashTerrainBytes(neighborBytes);

		std::int32_t minimumMm = std::numeric_limits<std::int32_t>::max();
		std::int32_t maximumMm = std::numeric_limits<std::int32_t>::min();
		// Active rectangles describe owned cells. Conservative height bounds also
		// include the east/north completion vertices used by those cells.
		for(std::uint32_t z = input.activeRect.minZ; z <= input.activeRect.maxZExclusive; ++z)
		{
			for(std::uint32_t x = input.activeRect.minX; x <= input.activeRect.maxXExclusive; ++x)
			{
				const std::int32_t height = ReadOwnedHeight(input.heightMillimetres, x, z);
				minimumMm = std::min(minimumMm, height);
				maximumMm = std::max(maximumMm, height);
			}
		}
		result.minimumHeightMetres = static_cast<float>(minimumMm) / 1000.0F;
		result.maximumHeightMetres = static_cast<float>(maximumMm) / 1000.0F;

		const std::array<std::uint32_t, 4u> extents{256u, 128u, 64u, 32u};
		for(std::uint32_t lod = 0u; lod < extents.size(); ++lod)
		{
			const std::uint32_t extent = extents[lod];
			for(std::uint32_t originZ = 0u; originZ < kOwnedCellExtent; originZ += extent)
			{
				for(std::uint32_t originX = 0u; originX < kOwnedCellExtent; originX += extent)
				{
					TerrainPageCookResult::LodNode node{};
					node.nodeId = static_cast<std::uint32_t>(result.lodNodes.size());
					node.parentNodeId = extent == kOwnedCellExtent
						? kInvalidNode : FindParentNode(result.lodNodes, originX, originZ, extent);
					node.lodLevel = static_cast<std::uint16_t>(3u - lod);
					node.cellOriginX = static_cast<std::uint16_t>(originX);
					node.cellOriginZ = static_cast<std::uint16_t>(originZ);
					node.cellExtent = static_cast<std::uint16_t>(extent);
					node.firstMeshlet = static_cast<std::uint32_t>(result.meshlets.size());
					node.maximumQuantizationErrorMetres = 0.0005F;
					node.maximumMorphDisplacementMetres = 0.0F;
					node.refinesHoleBoundary = ContainsHoleBoundary(
						activeHoleBits, originX, originZ, extent);

					std::int32_t nodeMinimumMm = std::numeric_limits<std::int32_t>::max();
					std::int32_t nodeMaximumMm = std::numeric_limits<std::int32_t>::min();
					for(std::uint32_t z = originZ; z <= originZ + extent; ++z)
					{
						for(std::uint32_t x = originX; x <= originX + extent; ++x)
						{
							const std::int32_t height = ReadOwnedHeight(input.heightMillimetres, x, z);
							nodeMinimumMm = std::min(nodeMinimumMm, height);
							nodeMaximumMm = std::max(nodeMaximumMm, height);
						}
					}
					node.bounds.minimumHeightMetres = static_cast<float>(nodeMinimumMm) / 1000.0F;
					node.bounds.maximumHeightMetres = static_cast<float>(nodeMaximumMm) / 1000.0F;
					node.bounds.maximumGeometricErrorMetres = ComputeConservativeDeviation(
						input.heightMillimetres, originX, originZ, extent)
						+ node.maximumQuantizationErrorMetres + 1.0e-6F;
					node.morphStartMetres = std::max(0.01F,
						node.bounds.maximumGeometricErrorMetres * 32.0F);
					node.morphEndMetres = node.morphStartMetres * 1.25F;

					const std::uint32_t sourceStride = extent / Format::kRenderedNodeCellExtent;
					for(std::uint32_t renderedZ = 0u; renderedZ < 32u; renderedZ += 7u)
					{
						for(std::uint32_t renderedX = 0u; renderedX < 32u; renderedX += 7u)
						{
							const std::uint32_t width = std::min(7u, 32u - renderedX);
							const std::uint32_t height = std::min(7u, 32u - renderedZ);
							TerrainPageCookResult::MeshletRecord meshlet{};
							meshlet.meshletId = static_cast<std::uint32_t>(result.meshlets.size());
							meshlet.nodeId = node.nodeId;
							meshlet.minX = static_cast<std::uint16_t>(originX + renderedX * sourceStride);
							meshlet.minZ = static_cast<std::uint16_t>(originZ + renderedZ * sourceStride);
							meshlet.maxXExclusive = static_cast<std::uint16_t>(originX + (renderedX + width) * sourceStride);
							meshlet.maxZExclusive = static_cast<std::uint16_t>(originZ + (renderedZ + height) * sourceStride);
							meshlet.renderedCellWidth = static_cast<std::uint16_t>(width);
							meshlet.renderedCellHeight = static_cast<std::uint16_t>(height);
							meshlet.vertexCount = (width + 1u) * (height + 1u);
							meshlet.triangleCount = width * height * 2u;
							meshlet.bounds = node.bounds;
							meshlet.hasHoleBoundary = node.refinesHoleBoundary;
							result.meshlets.push_back(meshlet);
						}
					}
					node.meshletCount = static_cast<std::uint32_t>(result.meshlets.size()) - node.firstMeshlet;
					result.lodNodes.push_back(node);
				}
			}
		}

		for(std::size_t index = result.lodNodes.size(); index-- > 0u;)
		{
			const auto& child = result.lodNodes[index];
			if(child.parentNodeId != kInvalidNode)
				result.lodNodes[child.parentNodeId].bounds.maximumGeometricErrorMetres = std::max(
					result.lodNodes[child.parentNodeId].bounds.maximumGeometricErrorMetres,
					child.bounds.maximumGeometricErrorMetres);
		}
		result.maximumGeometricErrorMetres = result.lodNodes.front().bounds.maximumGeometricErrorMetres;

		for(const auto& node : result.lodNodes)
		{
			TerrainPageCookResult::ErrorPyramidLevel level{};
			level.width = 1u;
			level.height = 1u;
			level.errors = {node.bounds.maximumGeometricErrorMetres};
			level.bounds = {node.bounds};
			result.errorPyramid.push_back(std::move(level));
			result.clusters.push_back({node.nodeId, node.parentNodeId,
				node.firstMeshlet, node.meshletCount, node.bounds});
		}

		result.packedNormals.resize(static_cast<std::size_t>(kOwnedVertexExtent) * kOwnedVertexExtent);
		for(std::uint32_t z = 0u; z < kOwnedVertexExtent; ++z)
		{
			for(std::uint32_t x = 0u; x < kOwnedVertexExtent; ++x)
			{
				const std::size_t haloIndex = static_cast<std::size_t>(z + 1u) * kHaloExtent + x + 1u;
				// An area owns its east/north completion row, but the compact halo
				// deliberately stops there. Use the shared vertex and its west/south
				// predecessor on both sides of an area boundary; adjacent cooks then
				// consume the same authored pair and produce identical packed normals.
				const bool xBoundary = x == 0u || x + 1u == kOwnedVertexExtent;
				const bool zBoundary = z == 0u || z + 1u == kOwnedVertexExtent;
				const float dx = static_cast<float>(xBoundary
					? input.heightMillimetres[haloIndex] - input.heightMillimetres[haloIndex - 1u]
					: input.heightMillimetres[haloIndex + 1u] - input.heightMillimetres[haloIndex - 1u])
					/ static_cast<float>((xBoundary ? 1000.0 : 2000.0) * input.sampleSpacingMetres);
				const float dz = static_cast<float>(zBoundary
					? input.heightMillimetres[haloIndex] - input.heightMillimetres[haloIndex - kHaloExtent]
					: input.heightMillimetres[haloIndex + kHaloExtent] - input.heightMillimetres[haloIndex - kHaloExtent])
					/ static_cast<float>((zBoundary ? 1000.0 : 2000.0) * input.sampleSpacingMetres);
				result.packedNormals[static_cast<std::size_t>(z) * kOwnedVertexExtent + x] = PackNormal(-dx, -dz);
			}
		}

		result.pagePayload.insert(result.pagePayload.end(), Format::kMagic.begin(), Format::kMagic.end());
		AppendU64(result.pagePayload, input.authoredHash.low64);
		AppendU64(result.pagePayload, input.authoredHash.high64);
		result.pagePayload.insert(result.pagePayload.end(), neighborBytes.begin(), neighborBytes.end());
		AppendU32(result.pagePayload, static_cast<std::uint32_t>(result.lodNodes.size()));
		AppendU32(result.pagePayload, static_cast<std::uint32_t>(result.meshlets.size()));
		for(const auto& node : result.lodNodes)
		{
			AppendU32(result.pagePayload, node.nodeId);
			AppendU32(result.pagePayload, node.parentNodeId);
			AppendU16(result.pagePayload, node.cellOriginX);
			AppendU16(result.pagePayload, node.cellOriginZ);
			AppendU16(result.pagePayload, node.cellExtent);
			AppendU16(result.pagePayload, node.lodLevel);
			AppendU32(result.pagePayload, node.firstMeshlet);
			AppendU32(result.pagePayload, node.meshletCount);
			AppendF32(result.pagePayload, node.bounds.maximumGeometricErrorMetres);
		}
		// Store one complete fixed-grid vertex set per spatial node. Each vertex
		// carries both child and parent endpoints so height and normal geomorphing
		// is a pure shader interpolation and cannot sample a non-resident parent.
		for(const auto& node : result.lodNodes)
		{
			const std::uint32_t stride = node.cellExtent / Format::kRenderedNodeCellExtent;
			const TerrainPageCookResult::LodNode* parent = node.parentNodeId == kInvalidNode
				? nullptr : &result.lodNodes[node.parentNodeId];
			for(std::uint32_t renderedZ = 0u; renderedZ <= Format::kRenderedNodeCellExtent; ++renderedZ)
			{
				for(std::uint32_t renderedX = 0u; renderedX <= Format::kRenderedNodeCellExtent; ++renderedX)
				{
					const std::uint32_t sourceX = node.cellOriginX + renderedX * stride;
					const std::uint32_t sourceZ = node.cellOriginZ + renderedZ * stride;
					const std::int32_t childHeight = ReadOwnedHeight(input.heightMillimetres, sourceX, sourceZ);
					std::int32_t parentHeight = childHeight;
					std::uint16_t parentNormal = result.packedNormals[
						static_cast<std::size_t>(sourceZ) * kOwnedVertexExtent + sourceX];
					if(parent != nullptr)
					{
						const std::uint32_t parentStride = parent->cellExtent / Format::kRenderedNodeCellExtent;
						const std::uint32_t relativeX = sourceX - parent->cellOriginX;
						const std::uint32_t relativeZ = sourceZ - parent->cellOriginZ;
						const std::uint32_t cellX = std::min(relativeX / parentStride, 31u);
						const std::uint32_t cellZ = std::min(relativeZ / parentStride, 31u);
						const std::uint32_t x0 = parent->cellOriginX + cellX * parentStride;
						const std::uint32_t z0 = parent->cellOriginZ + cellZ * parentStride;
						const float fx = static_cast<float>(sourceX - x0) / static_cast<float>(parentStride);
						const float fz = static_cast<float>(sourceZ - z0) / static_cast<float>(parentStride);
						const float projected = TriangleHeight(
							ReadOwnedHeightMetres(input.heightMillimetres, x0, z0),
							ReadOwnedHeightMetres(input.heightMillimetres, x0 + parentStride, z0),
							ReadOwnedHeightMetres(input.heightMillimetres, x0, z0 + parentStride),
							ReadOwnedHeightMetres(input.heightMillimetres, x0 + parentStride, z0 + parentStride),
							fx, fz, false);
						parentHeight = static_cast<std::int32_t>(std::lround(projected * 1000.0F));
						const std::uint32_t normalX = std::min<std::uint32_t>(
							parent->cellOriginX + ((relativeX + parentStride / 2u) / parentStride) * parentStride,
							parent->cellOriginX + parent->cellExtent);
						const std::uint32_t normalZ = std::min<std::uint32_t>(
							parent->cellOriginZ + ((relativeZ + parentStride / 2u) / parentStride) * parentStride,
							parent->cellOriginZ + parent->cellExtent);
						parentNormal = result.packedNormals[
							static_cast<std::size_t>(normalZ) * kOwnedVertexExtent + normalX];
					}
					const std::uint16_t childNormal = result.packedNormals[
						static_cast<std::size_t>(sourceZ) * kOwnedVertexExtent + sourceX];
					const std::uint32_t holeX = std::min(sourceX, kOwnedCellExtent - 1u);
					const std::uint32_t holeZ = std::min(sourceZ, kOwnedCellExtent - 1u);
					const std::uint16_t flags = IsHoleCell(activeHoleBits, holeX, holeZ) ? 1u : 0u;
					AppendU16(result.pagePayload, static_cast<std::uint16_t>(sourceX));
					AppendU16(result.pagePayload, static_cast<std::uint16_t>(sourceZ));
					AppendI32(result.pagePayload, childHeight);
					AppendI32(result.pagePayload, parentHeight);
					AppendU16(result.pagePayload, childNormal);
					AppendU16(result.pagePayload, parentNormal);
					AppendU16(result.pagePayload, flags);
					AppendU16(result.pagePayload, 0u);
				}
			}
		}
		for(auto& meshlet : result.meshlets)
		{
			meshlet.firstPayloadByte = static_cast<std::uint32_t>(result.pagePayload.size());
			AppendU32(result.pagePayload, meshlet.meshletId);
			AppendU32(result.pagePayload, meshlet.nodeId);
			AppendU16(result.pagePayload, meshlet.minX);
			AppendU16(result.pagePayload, meshlet.minZ);
			AppendU16(result.pagePayload, meshlet.maxXExclusive);
			AppendU16(result.pagePayload, meshlet.maxZExclusive);
			AppendU16(result.pagePayload, meshlet.renderedCellWidth);
			AppendU16(result.pagePayload, meshlet.renderedCellHeight);
			AppendU16(result.pagePayload, static_cast<std::uint16_t>(meshlet.vertexCount));
			AppendU16(result.pagePayload, static_cast<std::uint16_t>(meshlet.triangleCount));
			meshlet.payloadByteCount = static_cast<std::uint32_t>(result.pagePayload.size()) - meshlet.firstPayloadByte;
		}

		std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> topologyCache;
		for(const auto& meshlet : result.meshlets)
		{
			for(std::uint32_t mask = 0u; mask < Format::kStitchVariantCount; ++mask)
			{
				const std::uint32_t width = meshlet.renderedCellWidth;
				const std::uint32_t height = meshlet.renderedCellHeight;
				const std::uint32_t topologyKey = (width << 24u) | (height << 16u) | mask;
				auto topology = topologyCache.find(topologyKey);
				if(topology == topologyCache.end())
				{
					const std::uint32_t firstIndex = static_cast<std::uint32_t>(result.stitchIndices.size());
					const auto remap = [mask, width, height](std::uint32_t x, std::uint32_t z) {
						if((mask & 1u) != 0u && z == height && (x & 1u) != 0u) --x;
						if((mask & 2u) != 0u && x == width && (z & 1u) != 0u) --z;
						if((mask & 4u) != 0u && z == 0u && (x & 1u) != 0u) --x;
						if((mask & 8u) != 0u && x == 0u && (z & 1u) != 0u) --z;
						return static_cast<std::uint16_t>(z * (width + 1u) + x);
					};
					for(std::uint32_t z = 0u; z < height; ++z)
					{
						for(std::uint32_t x = 0u; x < width; ++x)
						{
							result.stitchIndices.push_back(remap(x, z));
							result.stitchIndices.push_back(remap(x + 1u, z));
							result.stitchIndices.push_back(remap(x + 1u, z + 1u));
							result.stitchIndices.push_back(remap(x, z));
							result.stitchIndices.push_back(remap(x + 1u, z + 1u));
							result.stitchIndices.push_back(remap(x, z + 1u));
						}
					}
					topology = topologyCache.emplace(topologyKey, std::pair{
						firstIndex, static_cast<std::uint32_t>(result.stitchIndices.size()) - firstIndex}).first;
				}
				const TerrainPageEdge edge = (mask & 1u) != 0u ? TerrainPageEdge::North
					: (mask & 2u) != 0u ? TerrainPageEdge::East
					: (mask & 4u) != 0u ? TerrainPageEdge::South : TerrainPageEdge::West;
				result.edgeStitchVariants.push_back({meshlet.meshletId, edge,
					static_cast<std::int8_t>(mask), false, static_cast<std::uint8_t>(mask),
					topology->second.first, topology->second.second});
			}
		}

		for(const std::uint16_t index : result.stitchIndices) AppendU16(result.pagePayload, index);
		if(result.pagePayload.empty())
		{
			result.diagnostic = "Cooked TERRAROOT1 page payload was empty.";
			return result;
		}
		for(std::size_t firstByte = 0u; firstByte < result.pagePayload.size(); firstByte += Format::kNominalPageBytes)
		{
			const std::size_t byteCount = std::min<std::size_t>(
				Format::kNominalPageBytes, result.pagePayload.size() - firstByte);
			const auto rangeBytes = std::span<const std::uint8_t>(result.pagePayload)
				.subspan(firstByte, byteCount);
			result.pageRanges.push_back({static_cast<std::uint32_t>(result.pageRanges.size()),
				static_cast<std::uint32_t>(firstByte), static_cast<std::uint32_t>(byteCount),
				TerraEngine::Terrain::HashTerrainBytes(rangeBytes)});
		}
		result.sourceIdentity = result.pageRanges.front().payloadIdentity;
		result.complete = result.sourceIdentity.IsValid();
		result.diagnostic = result.complete
			? "Cooked bounded TERRAROOT1 spatial quadtree, meshlets, normals, errors, morphs, and stitches."
			: "Cooked terrain page identity was invalid.";
		return result;
	}
}
