#pragma once
#include "VirtualGeometryWorldHierarchy.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace TerraEngine::Renderer
{
	struct VirtualGeometryOracleNode
	{
		std::uint64_t id = 0, parent = 0;
		bool resident = false;
		float error = 0;
	};
	struct VirtualGeometrySelectionResult
	{
		std::vector<std::uint64_t> selected, requests;
		bool failOpen = false;
		// A bounded worker plan must explain why it retained conservative
		// coverage. This is diagnostic data only; it never reaches live scene
		// state and therefore remains safe to inspect from worker jobs.
		std::string fallbackReason;
	};

	struct VirtualGeometryOracleOptions
	{
		// A zero capacity is an explicit fail-open request: the caller cannot
		// submit a device packet and must retain its already-valid fallback.
		std::size_t selectionCapacity = 0u;
		bool requestMissingDetail = true;
	};

	// Selects a deterministic cut from an immutable hierarchy snapshot. Missing
	// detail is requested once and resolved to the nearest resident ancestor; a
	// malformed chain or capacity exhaustion retains conservative coverage rather
	// than silently dropping geometry.
	[[nodiscard]] VirtualGeometrySelectionResult SelectVirtualGeometryOracle(
		const std::vector<VirtualGeometryOracleNode> &nodes,
		float threshold,
		const VirtualGeometryOracleOptions &options);

	[[nodiscard]] VirtualGeometrySelectionResult SelectVirtualGeometryOracle(
		const std::vector<VirtualGeometryOracleNode> &nodes, float threshold, std::size_t capacity);
} // namespace TerraEngine::Renderer
