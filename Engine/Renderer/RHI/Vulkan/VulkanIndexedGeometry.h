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
		// overload remains useful for CPU checks but has no GPU owner.
		std::shared_ptr<const Mesh> sourceOwner;
		std::uint64_t cpuPublicationGeneration = 0;
		// Prefix corner counts validate any selected meshlet range without
		// generating another index stream. The GPU reads packed meshlet data.
		std::vector<std::uint32_t> firstIndexByMeshlet;
		std::vector<IndexedCornerSection> sections;
		bool operator==(const IndexedCornerGeometry&) const = default;
	};

	// All three corners carry the same primitive identity without requiring
	// geometryShader or a fragment-stage SV_PrimitiveID input. Failure preserves
	// the previous output token, including when the uint64 intermediate overflows
	// the UINT32 corner token domain.
	[[nodiscard]] TERRAENGINE_API bool TryEncodeIndexedCornerToken(
		std::uint32_t meshletIndex, std::uint32_t primitiveIndex,
		std::uint32_t corner, std::uint32_t& output) noexcept;

	[[nodiscard]] TERRAENGINE_API bool TryBuildIndexedCornerGeometry(
		std::shared_ptr<const Mesh> source, IndexedCornerGeometry& output, std::string& reason);
	[[nodiscard]] TERRAENGINE_API std::optional<IndexedCornerSection> ResolveIndexedCornerRange(
		const IndexedCornerGeometry& geometry, std::uint32_t meshletOffset, std::uint32_t meshletCount) noexcept;

	// Reads only the caller's immutable Mesh. No ObjectManager, resource manager,
	// GPU object, or live scene is accessed. A worker may prepare this owned result;
	// the protected boundary retains the exact Mesh owner/publication before
	// making its packed meshlet data available to compatibility mesh draws.
	// This validates topology/addressability, not material/deformation/MSAA
	// capability. Rejection or allocation failure never changes the prior output.
	[[nodiscard]] TERRAENGINE_API bool TryBuildIndexedCornerGeometry(
		const Mesh& source, IndexedCornerGeometry& output, std::string& reason);
}
