// TerrainDirectMesh.cpp -- Canonical direct-resident terrain mesh generation.

#include "Renderer/Terrain/TerrainDirectMesh.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Terrain/TerrainMaterialRuntimeRegistry.h"
#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryPackedVertex.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace TerraEngine::Renderer::Terrain
{
	// Direct-resident geometry carries only control UV. Its material consumer uses
	// the same fixed record as page-backed packets, preventing a first-layer-only
	// compatibility layout from becoming a second rendering ABI.
	static_assert(sizeof(TerrainGpuLayerBinding) == 96u);

	namespace
	{
		constexpr std::uint32_t kMeshletCellExtent = 7u;

		bool IsHole(std::span<const std::uint8_t> bits, std::uint32_t x, std::uint32_t z) noexcept
		{
			if(bits.empty() || x >= 255u || z >= 255u) return false;
			const std::uint32_t bit = z * 256u + x;
			return (bits[bit / 8u] & static_cast<std::uint8_t>(1u << (bit % 8u))) != 0u;
		}

		std::array<float, 3u> Normalize(std::array<float, 3u> value) noexcept
		{
			const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
			if(!std::isfinite(length) || length <= 1.0e-12F) return {0.0F, 1.0F, 0.0F};
			return {value[0] / length, value[1] / length, value[2] / length};
		}

		std::array<float, 3u> ComputeNormal(
			std::span<const std::int32_t> heights,
			std::uint32_t x,
			std::uint32_t z,
			float spacingX,
			float spacingZ) noexcept
		{
			const std::uint32_t left = x == 0u ? 0u : x - 1u;
			const std::uint32_t right = std::min<std::uint32_t>(255u, x + 1u);
			const std::uint32_t south = z == 0u ? 0u : z - 1u;
			const std::uint32_t north = std::min<std::uint32_t>(255u, z + 1u);
			const float heightLeft = static_cast<float>(heights[z * 256u + left]) * 0.001F;
			const float heightRight = static_cast<float>(heights[z * 256u + right]) * 0.001F;
			const float heightSouth = static_cast<float>(heights[south * 256u + x]) * 0.001F;
			const float heightNorth = static_cast<float>(heights[north * 256u + x]) * 0.001F;
			const float xDistance = static_cast<float>(right - left) * spacingX;
			const float zDistance = static_cast<float>(north - south) * spacingZ;
			// Cross the positive-Z derivative with positive-X so a flat terrain has
			// an upward normal in TerraEngine's Y-up coordinate system.
			return Normalize({
				-(heightRight - heightLeft) * std::max(1.0e-6F, zDistance),
				std::max(1.0e-6F, zDistance) * std::max(1.0e-6F, xDistance),
				-(heightNorth - heightSouth) * std::max(1.0e-6F, xDistance)});
		}

		void ConfigureRuntimeLayout(MeshletRuntimeLayout& layout) noexcept
		{
			std::memcpy(layout.magic, "TEMF", 4u);
			layout.version = kMeshCurrentVersion;
			layout.headerSize = static_cast<std::uint32_t>(sizeof(MeshletRuntimeLayout));
			layout.layout.strideBytes = kMeshletPackedVertexStrideBytes;
			layout.layout.attributeCount = 4u;
			layout.layout.attributes[0] = {static_cast<std::uint32_t>(AttributeSemantic::Position), static_cast<std::uint32_t>(AttributeFormat::R16G16B16Unorm), 0u};
			layout.layout.attributes[1] = {static_cast<std::uint32_t>(AttributeSemantic::Normal), static_cast<std::uint32_t>(AttributeFormat::Oct12x2), 6u};
			layout.layout.attributes[2] = {static_cast<std::uint32_t>(AttributeSemantic::Tangent), static_cast<std::uint32_t>(AttributeFormat::Oct10x2TangentSign), 9u};
			layout.layout.attributes[3] = {static_cast<std::uint32_t>(AttributeSemantic::TexCoord0), static_cast<std::uint32_t>(AttributeFormat::R16G16Unorm), 12u};
			layout.meshletVertexRemapEncoding = static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16);
			layout.meshletFlags = kMeshletHeaderFlagHasBounds;
		}
	}

	TerrainDirectMeshResult BuildTerrainDirectMesh(
		const TerrainDirectMeshInput& input,
		const std::atomic_bool* cancellation)
	{
		TerrainDirectMeshResult result{};
		result.assetId = input.assetId;
		result.authoredHash = input.authoredHash;
		result.authoredGeneration = input.authoredGeneration;
		const float sampleStepX = input.sampleStepMetres[0] > 0.0F
			? input.sampleStepMetres[0] : input.sampleSpacingMetres;
		const float sampleStepZ = input.sampleStepMetres[1] > 0.0F
			? input.sampleStepMetres[1] : input.sampleSpacingMetres;
		if(input.assetId == TerraEngine::EMPTY_UUID || !input.authoredHash.IsValid()
			|| input.authoredGeneration == 0u || !std::isfinite(input.sampleSpacingMetres)
			|| input.sampleSpacingMetres <= 0.0F
			|| !std::isfinite(sampleStepX) || sampleStepX <= 0.0F
			|| !std::isfinite(sampleStepZ) || sampleStepZ <= 0.0F
			|| !input.activeRect.IsValid()
			|| input.heightMillimetres.size() != TerraEngine::Terrain::kTerrainAreaSampleCount
			|| (!input.holeCellBits.empty() && input.holeCellBits.size() != TerraEngine::Terrain::kTerrainAreaSampleCount / 8u))
		{
			result.diagnostic = "Direct terrain rendering requires one complete 256 by 256 authored area and a stable generation.";
			return result;
		}

		auto mesh = std::make_unique<Mesh>();
		ConfigureRuntimeLayout(mesh->runtimeLayout);
		std::int32_t minimumHeight = (std::numeric_limits<std::int32_t>::max)();
		std::int32_t maximumHeight = (std::numeric_limits<std::int32_t>::min)();
		for (std::uint32_t z = input.activeRect.minZ; z < input.activeRect.maxZExclusive; ++z)
			for (std::uint32_t x = input.activeRect.minX; x < input.activeRect.maxXExclusive; ++x)
			{
				const std::int32_t height = input.heightMillimetres[static_cast<std::size_t>(z) * 256u + x];
				minimumHeight = std::min(minimumHeight, height);
				maximumHeight = std::max(maximumHeight, height);
			}
		const float minimumY = static_cast<float>(minimumHeight) * 0.001F;
		const float maximumY = static_cast<float>(maximumHeight) * 0.001F;
		const double areaOriginX = input.sampleOriginMetres
			? (*input.sampleOriginMetres)[0]
			: static_cast<double>(input.area.x) * 256.0 * input.sampleSpacingMetres;
		const double areaOriginZ = input.sampleOriginMetres
			? (*input.sampleOriginMetres)[1]
			: static_cast<double>(input.area.z) * 256.0 * input.sampleSpacingMetres;
		if(!std::isfinite(areaOriginX) || !std::isfinite(areaOriginZ)
			|| std::abs(areaOriginX) > std::numeric_limits<float>::max()
			|| std::abs(areaOriginZ) > std::numeric_limits<float>::max())
		{
			result.diagnostic = "Terrain area placement exceeds the direct render proxy coordinate range.";
			return result;
		}
		const float areaMinimumX = static_cast<float>(areaOriginX);
		const float areaMinimumZ = static_cast<float>(areaOriginZ);
		const float minimumX = areaMinimumX + input.activeRect.minX * sampleStepX;
		const float minimumZ = areaMinimumZ + input.activeRect.minZ * sampleStepZ;
		const float maximumX = areaMinimumX + (input.activeRect.maxXExclusive - 1u) * sampleStepX;
		const float maximumZ = areaMinimumZ + (input.activeRect.maxZExclusive - 1u) * sampleStepZ;
		const std::array<float, 3u> boundsMinimum{minimumX, minimumY, minimumZ};
		const std::array<float, 3u> boundsMaximum{maximumX, maximumY, maximumZ};
		for(std::size_t axis = 0u; axis < 3u; ++axis)
		{
			mesh->runtimeLayout.quantization.positionOffset[axis] = (boundsMinimum[axis] + boundsMaximum[axis]) * 0.5F;
			const float extent = (boundsMaximum[axis] - boundsMinimum[axis]) * 0.5F;
			mesh->runtimeLayout.quantization.positionScale[axis] = extent > 1.0e-6F ? extent : 1.0F;
			mesh->localBoundsMin[axis] = boundsMinimum[axis];
			mesh->localBoundsMax[axis] = boundsMaximum[axis];
		}
		mesh->runtimeLayout.quantization.uvScale[0] = 1.0F;
		mesh->runtimeLayout.quantization.uvScale[1] = 1.0F;
		mesh->hasLocalBounds = true;
		mesh->sourcePath = "generated/terrain-direct-mesh";

		mesh->reconstructedVertexData.resize(
			TerraEngine::Terrain::kTerrainAreaSampleCount * sizeof(TerraEngine::VirtualGeometryPackedVertex));
		for(std::uint32_t z = 0u; z < 256u; ++z)
		{
			if(cancellation != nullptr && cancellation->load(std::memory_order_acquire))
			{
				result.cancelled = true;
				return result;
			}
			for(std::uint32_t x = 0u; x < 256u; ++x)
			{
				const std::size_t sample = static_cast<std::size_t>(z) * 256u + x;
				const std::array<float, 3u> position{
					(areaMinimumX + static_cast<float>(x) * sampleStepX - mesh->runtimeLayout.quantization.positionOffset[0]) / mesh->runtimeLayout.quantization.positionScale[0],
					(static_cast<float>(input.heightMillimetres[sample]) * 0.001F - mesh->runtimeLayout.quantization.positionOffset[1]) / mesh->runtimeLayout.quantization.positionScale[1],
					(areaMinimumZ + static_cast<float>(z) * sampleStepZ - mesh->runtimeLayout.quantization.positionOffset[2]) / mesh->runtimeLayout.quantization.positionScale[2]};
				const auto packed = TerraEngine::PackVirtualGeometryVertex(
					position, ComputeNormal(input.heightMillimetres, x, z, sampleStepX, sampleStepZ),
					{1.0F, 0.0F, 0.0F}, 1.0F,
					// TexCoord0 is the independent area-local control address. Dividing
					// by the owned 256-sample extent lets the shader recover the exact
					// local sample as uv * 256; physical surface UVs are then computed
					// from sample spacing and a bounded per-area material phase.
					{static_cast<float>(x) / 256.0F, static_cast<float>(z) / 256.0F},
					16u, 12u, 10u, 16u);
				std::memcpy(mesh->reconstructedVertexData.data() + sample * sizeof(packed), &packed, sizeof(packed));
			}
		}

		std::vector<std::uint8_t> primitiveIndices;
		const std::uint32_t cellMaxXExclusive = input.activeRect.maxXExclusive - 1u;
		const std::uint32_t cellMaxZExclusive = input.activeRect.maxZExclusive - 1u;
		primitiveIndices.reserve(static_cast<std::size_t>(cellMaxXExclusive - input.activeRect.minX)
			* (cellMaxZExclusive - input.activeRect.minZ) * 6u);
		for(std::uint32_t minZ = input.activeRect.minZ; minZ < cellMaxZExclusive; minZ += kMeshletCellExtent)
		{
			if(cancellation != nullptr && cancellation->load(std::memory_order_acquire))
			{
				result.cancelled = true;
				return result;
			}
			const std::uint32_t cellsZ = std::min(kMeshletCellExtent, cellMaxZExclusive - minZ);
			for(std::uint32_t minX = input.activeRect.minX; minX < cellMaxXExclusive; minX += kMeshletCellExtent)
			{
				const std::uint32_t cellsX = std::min(kMeshletCellExtent, cellMaxXExclusive - minX);
				const std::uint32_t rowVertices = cellsX + 1u;
				MeshletRecord record{};
				record.vertexOffset = static_cast<std::uint32_t>(mesh->meshletVertexRemapDecoded.size());
				record.vertexCount = rowVertices * (cellsZ + 1u);
				record.primitiveIndexOffset = static_cast<std::uint32_t>(primitiveIndices.size());
				for(std::uint32_t localZ = 0u; localZ <= cellsZ; ++localZ)
					for(std::uint32_t localX = 0u; localX <= cellsX; ++localX)
						mesh->meshletVertexRemapDecoded.push_back((minZ + localZ) * 256u + minX + localX);
				for(std::uint32_t localZ = 0u; localZ < cellsZ; ++localZ)
					for(std::uint32_t localX = 0u; localX < cellsX; ++localX)
					{
						if(IsHole(input.holeCellBits, minX + localX, minZ + localZ)) continue;
						const std::uint8_t v00 = static_cast<std::uint8_t>(localZ * rowVertices + localX);
						const std::uint8_t v10 = static_cast<std::uint8_t>(v00 + 1u);
						const std::uint8_t v01 = static_cast<std::uint8_t>(v00 + rowVertices);
						const std::uint8_t v11 = static_cast<std::uint8_t>(v01 + 1u);
						primitiveIndices.insert(primitiveIndices.end(), {v00, v01, v10, v10, v01, v11});
					}
				record.primitiveIndexCount = static_cast<std::uint32_t>(primitiveIndices.size()) - record.primitiveIndexOffset;
				if(record.primitiveIndexCount == 0u)
				{
					mesh->meshletVertexRemapDecoded.resize(record.vertexOffset);
					continue;
				}
				mesh->meshletRecords.push_back(record);
				MeshletBounds bounds{};
				bounds.center[0] = areaMinimumX + (static_cast<float>(minX) + static_cast<float>(cellsX) * 0.5F) * sampleStepX;
				bounds.center[1] = (minimumY + maximumY) * 0.5F;
				bounds.center[2] = areaMinimumZ + (static_cast<float>(minZ) + static_cast<float>(cellsZ) * 0.5F) * sampleStepZ;
				const float halfX = static_cast<float>(cellsX) * sampleStepX * 0.5F;
				const float halfY = (maximumY - minimumY) * 0.5F;
				const float halfZ = static_cast<float>(cellsZ) * sampleStepZ * 0.5F;
				bounds.radius = std::sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ);
				bounds.coneCutoff = 2.0F;
				mesh->meshletBounds.push_back(bounds);
			}
		}

		if(mesh->meshletRecords.empty() || primitiveIndices.empty())
		{
			result.diagnostic = "The terrain area contains no solid cells to render.";
			return result;
		}
		mesh->meshletSections.push_back({0u, static_cast<std::uint32_t>(mesh->meshletRecords.size())});
		mesh->runtimeLayout.meshletVertexCount = TerraEngine::Terrain::kTerrainAreaSampleCount;
		mesh->runtimeLayout.meshletVertexRemapCount = static_cast<std::uint32_t>(mesh->meshletVertexRemapDecoded.size());
		mesh->runtimeLayout.meshletPrimitiveIndexCount = static_cast<std::uint32_t>(primitiveIndices.size());
		mesh->runtimeLayout.meshletCount = static_cast<std::uint32_t>(mesh->meshletRecords.size());
		mesh->runtimeLayout.meshletSectionCount = 1u;
		mesh->runtimeLayout.meshletBoundsCount = static_cast<std::uint32_t>(mesh->meshletBounds.size());
		const std::size_t remapBytes = mesh->meshletVertexRemapDecoded.size() * sizeof(std::uint16_t);
		mesh->storage.resize(remapBytes + primitiveIndices.size());
		for(std::size_t index = 0u; index < mesh->meshletVertexRemapDecoded.size(); ++index)
		{
			const std::uint16_t remap = static_cast<std::uint16_t>(mesh->meshletVertexRemapDecoded[index]);
			std::memcpy(mesh->storage.data() + index * sizeof(remap), &remap, sizeof(remap));
		}
		std::memcpy(mesh->storage.data() + remapBytes, primitiveIndices.data(), primitiveIndices.size());
		mesh->meshletVertexData = mesh->reconstructedVertexData;
		mesh->meshletVertexRemapData = std::span<const std::uint8_t>(mesh->storage.data(), remapBytes);
		mesh->meshletPrimitiveIndexData = std::span<const std::uint8_t>(mesh->storage.data() + remapBytes, primitiveIndices.size());
		// This mesh is produced in memory rather than decoded from a TEMF file. Give
		// frame routing a typed proof of the finished resident payload instead of
		// forging a serialized production header that no loader actually validated.
		std::uint64_t sourceContentIdentity = input.authoredHash.low64 ^ input.authoredHash.high64;
		if(sourceContentIdentity == 0u)
			sourceContentIdentity = input.authoredHash.low64 != 0u
				? input.authoredHash.low64 : input.authoredHash.high64;
		mesh->generatedDirectMeshletRoute.sourceContentIdentity = sourceContentIdentity;
		mesh->generatedDirectMeshletRoute.generationIdentity = input.authoredGeneration;
		mesh->generatedDirectMeshletRoute.residentPayloadBytes =
			static_cast<std::uint64_t>(mesh->reconstructedVertexData.size())
			+ static_cast<std::uint64_t>(mesh->storage.size())
			+ static_cast<std::uint64_t>(mesh->meshletRecords.size() * sizeof(MeshletRecord))
			+ static_cast<std::uint64_t>(mesh->meshletSections.size() * sizeof(MeshletSection))
			+ static_cast<std::uint64_t>(mesh->meshletBounds.size() * sizeof(MeshletBounds));
		// The editor proxy is replaced asynchronously after terrain edits. This
		// typed proof lets normal direct-meshlet routing consume the new generation.
		mesh->generatedDirectMeshletRoute.validated = true;
		result.mesh = std::move(mesh);
		return result;
	}
}
