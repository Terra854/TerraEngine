// VirtualGeometryErrorModel.h - simplification safety and accumulated error.

#pragma once

#include <cstdint>
#include <vector>

namespace TerraEngine::Editor
{
	// A simplifier may merge two vertices only when every protected authored
	// boundary agrees. Hashes keep this record compact while retaining material,
	// UV, shading-frame, deformation, and authored-mask distinctions.
	struct VirtualGeometryAttributeSignature
	{
		std::uint32_t materialId = 0u;
		std::uint64_t uvHash = 0u;
		std::uint64_t normalHash = 0u;
		std::uint64_t tangentHash = 0u;
		std::uint64_t hardEdgeGroup = 0u;
		std::uint64_t skinningHash = 0u;
		std::uint64_t morphHash = 0u;
		std::uint64_t authoredMask = 0u;

		auto operator<=>(const VirtualGeometryAttributeSignature &) const = default;
	};

	[[nodiscard]] bool CanSimplifyVirtualGeometryAttributesTogether(
		const VirtualGeometryAttributeSignature &first, const VirtualGeometryAttributeSignature &second) noexcept;

	struct VirtualGeometryCoverageProof
	{
		float objectError = 0.0f;
		float descendantError = 0.0f;
		float screenError = 0.0f;
		bool completeCoverage = false;

		[[nodiscard]] bool Valid() const noexcept;
	};

	// descendantErrors must contain one proof for every child represented by
	// the parent. An empty list is incomplete rather than silently error-free.
	[[nodiscard]] VirtualGeometryCoverageProof BuildVirtualGeometryCoverageProof(
		float objectError, const std::vector<float> &descendantErrors, float projectionScale, float distance);
} // namespace TerraEngine::Editor
