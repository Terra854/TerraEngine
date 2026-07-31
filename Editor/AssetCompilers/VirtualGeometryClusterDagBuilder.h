// VirtualGeometryClusterDagBuilder.h - Deterministic bounded cluster grouping.

#pragma once

#include <cstdint>
#include <vector>

namespace TerraEngine::Editor
{
	struct VirtualGeometryClusterInput
	{
		std::uint64_t stableId = 0u;
		std::uint32_t material = 0u;
		std::vector<std::uint64_t> neighbors;
		float error = 0.0f;
	};

	struct VirtualGeometryDagNode
	{
		std::uint64_t id = 0u;
		std::uint64_t parentId = 0u;
		std::uint64_t fallbackId = 0u;
		std::vector<std::uint64_t> children;
		float error = 0.0f;
		bool completeCoverage = false;
	};

	struct VirtualGeometryDagBuildSettings
	{
		std::uint32_t maximumChildren = 4u;
	};

	[[nodiscard]] std::vector<VirtualGeometryDagNode> BuildVirtualGeometryClusterDag(
		std::vector<VirtualGeometryClusterInput> inputs, VirtualGeometryDagBuildSettings settings = {});

	[[nodiscard]] bool ValidateVirtualGeometryDag(const std::vector<VirtualGeometryDagNode> &nodes);
} // namespace TerraEngine::Editor
