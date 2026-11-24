// VulkanIndexedGeometry.h - Worker-safe indexed topology from accepted TEMF data.
#pragma once

#include "Core/Export.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct Mesh;

namespace TerraEngine::Renderer::Vulkan
{
	inline constexpr std::uint32_t kIndexedMeshletPrimitiveCapacity = 124U;
	inline constexpr std::uint32_t kIndexedTriangleCornerCount = 3U;

	struct IndexedCornerSection
	{
		std::uint32_t firstIndex = 0, indexCount = 0;
		std::uint32_t meshletOffset = 0, meshletCount = 0;
		bool operator==(const IndexedCornerSection&) const = default;
	};

	struct IndexedCornerGeometry
	{
		// GPU handoff requires this exact immutable owner. The const-reference
		// overload remains useful for CPU checks but is not upload admission.
		std::shared_ptr<const Mesh> sourceOwner;
		std::uint64_t cpuPublicationGeneration = 0;
		// These are real UINT32 raster indices, not the existing uint8 local
		// primitive bytes. SV_VertexID decodes the token, then pulls the packed
		// owner vertex through the existing mesh storage-buffer descriptors.
		std::vector<std::uint32_t> indices;
		// Prefix offsets translate the exact selected meshlet span to a native
		// index range, even when a selected LOD is only part of an authored section.
		std::vector<std::uint32_t> firstIndexByMeshlet;
		std::vector<IndexedCornerSection> sections;
		bool operator==(const IndexedCornerGeometry&) const = default;
	};

	// All three corners carry the same primitive identity without requiring
	// geometryShader or a fragment-stage SV_PrimitiveID input. Failure preserves
	// the previous output token, including when the uint64 intermediate overflows
	// the UINT32 raster index domain.
	[[nodiscard]] TERRAENGINE_API bool TryEncodeIndexedCornerToken(
		std::uint32_t meshletIndex, std::uint32_t primitiveIndex,
		std::uint32_t corner, std::uint32_t& output) noexcept;

	[[nodiscard]] TERRAENGINE_API bool TryBuildIndexedCornerGeometry(
		std::shared_ptr<const Mesh> source, IndexedCornerGeometry& output, std::string& reason);
	[[nodiscard]] TERRAENGINE_API std::optional<IndexedCornerSection> ResolveIndexedCornerRange(
		const IndexedCornerGeometry& geometry, std::uint32_t meshletOffset, std::uint32_t meshletCount) noexcept;

	// Reads only the caller's immutable Mesh. No ObjectManager, resource manager,
	// GPU object, or live scene is accessed. A worker may prepare this owned result;
	// the protected uploader must retain the exact Mesh owner/publication and
	// publish its INDEX_BUFFER only after the complete upload succeeds.
	// This validates topology/addressability, not material/deformation/MSAA
	// capability. Rejection or allocation failure never changes the prior output.
	[[nodiscard]] TERRAENGINE_API bool TryBuildIndexedCornerGeometry(
		const Mesh& source, IndexedCornerGeometry& output, std::string& reason);
}
