// VulkanIndexedGeometry.cpp - Checked corner-token and section preparation.
#include "VulkanIndexedGeometry.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace TerraEngine::Renderer::Vulkan
{
	bool TryEncodeIndexedCornerToken(std::uint32_t meshletIndex,
		std::uint32_t primitiveIndex, std::uint32_t corner, std::uint32_t& output) noexcept
	{
		if (primitiveIndex >= kIndexedMeshletPrimitiveCapacity || corner >= kIndexedTriangleCornerCount) return false;
		const std::uint64_t token = (static_cast<std::uint64_t>(meshletIndex)
			* kIndexedMeshletPrimitiveCapacity + primitiveIndex) * kIndexedTriangleCornerCount + corner;
		if (token > std::numeric_limits<std::uint32_t>::max()) return false;
		output = static_cast<std::uint32_t>(token);
		return true;
	}

	bool TryBuildIndexedCornerGeometry(std::shared_ptr<const Mesh> source,
		IndexedCornerGeometry& output, std::string& reason)
	{
		if (!source) { reason = "An immutable mesh owner is required."; return false; }
		IndexedCornerGeometry candidate;
		if (!TryBuildIndexedCornerGeometry(*source, candidate, reason)) return false;
		candidate.sourceOwner = std::move(source);
		output = std::move(candidate);
		return true;
	}

	std::optional<IndexedCornerSection> ResolveIndexedCornerRange(const IndexedCornerGeometry& geometry,
		std::uint32_t meshletOffset, std::uint32_t meshletCount) noexcept
	{
		const auto pastLast = static_cast<std::uint64_t>(meshletOffset) + meshletCount;
		if (meshletCount == 0 || pastLast >= geometry.firstIndexByMeshlet.size()) return std::nullopt;
		const auto firstIndex = geometry.firstIndexByMeshlet[meshletOffset];
		const auto endIndex = geometry.firstIndexByMeshlet[static_cast<std::size_t>(pastLast)];
		if (endIndex <= firstIndex || endIndex > geometry.indices.size()) return std::nullopt;
		return IndexedCornerSection{firstIndex, endIndex - firstIndex, meshletOffset, meshletCount};
	}

	bool TryBuildIndexedCornerGeometry(const Mesh& source, IndexedCornerGeometry& output, std::string& reason)
	{
		const auto reject = [&reason](const char* detail) { reason = detail; return false; };
		const auto& layout = source.runtimeLayout;
		if (source.publicationGeneration == 0 || layout.meshletCount == 0 || layout.meshletVertexCount == 0
			|| layout.meshletSectionCount == 0 || layout.meshletCount != source.meshletRecords.size()
			|| layout.meshletSectionCount != source.meshletSections.size()) return reject("Indexed topology requires a complete accepted mesh publication.");
		if (layout.layout.strideBytes != kMeshletPackedVertexStrideBytes
			|| static_cast<std::uint64_t>(layout.meshletVertexCount) * kMeshletPackedVertexStrideBytes != source.meshletVertexData.size()
			|| layout.meshletPrimitiveIndexCount != source.meshletPrimitiveIndexData.size()) return reject("Indexed packed vertex or primitive bytes do not match the accepted counts.");
		if (layout.meshletVertexRemapEncoding > static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16))
			return reject("Indexed remap encoding is unsupported.");
		const std::uint32_t remapStride = layout.meshletVertexRemapEncoding == static_cast<std::uint32_t>(MeshletRemapEncoding::Uint16) ? 2U : 4U;
		if (static_cast<std::uint64_t>(layout.meshletVertexRemapCount) * remapStride != source.meshletVertexRemapData.size()
			|| (!source.meshletVertexRemapDecoded.empty() && source.meshletVertexRemapDecoded.size() != layout.meshletVertexRemapCount))
			return reject("Indexed vertex remap bytes do not match the accepted count.");
		// Read the same little-endian bytes as ByteAddressBuffer.Load. A decoded
		// optional cache cannot silently disagree with what the vertex shader sees.
		for (std::uint32_t index = 0; index < layout.meshletVertexRemapCount; ++index)
		{
			const auto offset = static_cast<std::size_t>(index) * remapStride;
			std::uint32_t vertex = 0;
			for (std::uint32_t byte = 0; byte < remapStride; ++byte)
				vertex |= static_cast<std::uint32_t>(source.meshletVertexRemapData[offset + byte]) << (byte * 8U);
			if (vertex >= layout.meshletVertexCount || (!source.meshletVertexRemapDecoded.empty() && source.meshletVertexRemapDecoded[index] != vertex))
				return reject("Indexed remap names an unavailable owner vertex or conflicting decoded value.");
		}
		try
		{
			IndexedCornerGeometry candidate;
			candidate.cpuPublicationGeneration = source.publicationGeneration;
			auto& firstIndexByMeshlet = candidate.firstIndexByMeshlet;
			firstIndexByMeshlet.reserve(static_cast<std::size_t>(layout.meshletCount) + 1U);
			std::uint64_t indexCount = 0;
			for (std::uint32_t meshlet = 0; meshlet < layout.meshletCount; ++meshlet)
			{
				firstIndexByMeshlet.push_back(static_cast<std::uint32_t>(indexCount));
				const auto& record = source.meshletRecords[meshlet];
				if (record.vertexCount == 0 || record.vertexCount > 64U || record.primitiveIndexCount == 0
					|| record.primitiveIndexCount % kIndexedTriangleCornerCount != 0
					|| record.primitiveIndexCount / kIndexedTriangleCornerCount > kIndexedMeshletPrimitiveCapacity
					|| static_cast<std::uint64_t>(record.vertexOffset) + record.vertexCount > layout.meshletVertexRemapCount
					|| static_cast<std::uint64_t>(record.primitiveIndexOffset) + record.primitiveIndexCount > layout.meshletPrimitiveIndexCount)
					return reject("Indexed meshlet topology or byte range is invalid.");
				for (std::uint32_t element = 0; element < record.primitiveIndexCount; ++element)
					if (source.meshletPrimitiveIndexData[static_cast<std::size_t>(record.primitiveIndexOffset) + element] >= record.vertexCount)
						return reject("Indexed primitive names an unavailable meshlet-local vertex.");
				std::uint32_t lastToken = 0;
				if (!TryEncodeIndexedCornerToken(meshlet, record.primitiveIndexCount / kIndexedTriangleCornerCount - 1U, 2U, lastToken))
					return reject("Indexed primitive identity exceeds the UINT32 token domain.");
				indexCount += record.primitiveIndexCount;
				if (indexCount > std::numeric_limits<std::uint32_t>::max()) return reject("Indexed draw element count exceeds UINT32.");
			}
			firstIndexByMeshlet.push_back(static_cast<std::uint32_t>(indexCount));
			candidate.sections.reserve(layout.meshletSectionCount);
			for (const auto& section : source.meshletSections)
			{
				const std::uint64_t end = static_cast<std::uint64_t>(section.meshletOffset) + section.meshletCount;
				if (end > layout.meshletCount) return reject("Indexed section exceeds the accepted meshlet table.");
				const auto first = firstIndexByMeshlet[section.meshletOffset];
				candidate.sections.push_back({first, firstIndexByMeshlet[static_cast<std::size_t>(end)] - first, section.meshletOffset, section.meshletCount});
			}
			candidate.indices.reserve(static_cast<std::size_t>(indexCount));
			for (std::uint32_t meshlet = 0; meshlet < layout.meshletCount; ++meshlet)
			{
				const auto primitiveCount = source.meshletRecords[meshlet].primitiveIndexCount / kIndexedTriangleCornerCount;
				for (std::uint32_t primitive = 0; primitive < primitiveCount; ++primitive)
					for (std::uint32_t corner = 0; corner < kIndexedTriangleCornerCount; ++corner)
					{
						std::uint32_t token = 0;
						if (!TryEncodeIndexedCornerToken(meshlet, primitive, corner, token)) return reject("Indexed token validation changed during preparation.");
						candidate.indices.push_back(token);
					}
			}
			// One move is the publication point. All source validation and owned
			// allocation completed before the caller's previous result is replaced.
			output = std::move(candidate);
			reason.clear();
			return true;
		}
		catch (const std::bad_alloc&) { return reject("Indexed topology allocation failed."); }
		catch (const std::length_error&) { return reject("Indexed topology allocation size is invalid."); }
	}
}
