// VirtualGeometryIdentity.h - Stable content-derived geometry identities.

#pragma once

#include <cstdint>
#include <string_view>

namespace TerraEngine::Editor
{
	struct VirtualGeometryPrimitiveMapping
	{
		std::uint64_t primitiveId = 0u;
		std::uint64_t clusterId = 0u;
		std::uint32_t pageId = 0u;
	};

	// A boundary group is assigned from source identity before clustering or page
	// scheduling. Jobs may therefore be reordered without changing the shared
	// quantization lattice used by vertices that meet at a protected boundary.
	struct VirtualGeometryBoundaryGroup
	{
		std::uint64_t groupId = 0u;
		std::uint64_t sourcePrimitiveId = 0u;
		std::uint32_t sourceVertexOrdinal = 0u;
	};

	// The namespace distinguishes otherwise identical primitive, cluster, and
	// page identifiers. The result never uses zero, which remains "no identity".
	[[nodiscard]] std::uint64_t BuildVirtualGeometryStableId(std::string_view namespaceName, std::uint64_t contentHash, std::uint32_t ordinal) noexcept;

	[[nodiscard]] VirtualGeometryPrimitiveMapping AssignVirtualGeometryPrimitive(
		std::uint64_t assetHash, std::uint32_t primitiveOrdinal, std::uint32_t pageCount) noexcept;

	// This deliberately excludes page and cluster ordinals: both are downstream
	// implementation details which can change when parallel cook work is split.
	[[nodiscard]] VirtualGeometryBoundaryGroup AssignVirtualGeometryBoundaryGroup(
		std::uint64_t assetHash, std::uint32_t primitiveOrdinal, std::uint32_t sourceVertexOrdinal) noexcept;
} // namespace TerraEngine::Editor
