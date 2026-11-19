// Score geometry routes from captured workload counts before per-view work.
#pragma once

#include "GeometryRoutePolicy.h"

#include <cstddef>
#include <cstdint>

namespace TerraEngine::Renderer
{
	// Source counts describe a single owner LOD. Clusters and physical records
	// describe the hierarchy and page work the virtual routes can incur.
	struct GeometryRouteWorkload
	{
		std::uint32_t sourceTriangles = 0u;
		std::uint32_t sourceMeshlets = 0u;
		std::uint64_t decodedBytes = 0u;
		std::size_t virtualClusters = 0u;
		std::uint32_t physicalPages = 0u;
		std::uint32_t physicalBlocks = 0u;
	};

	// Fill relative estimates and their policy version while preserving the
	// caller's eligibility, capability, and override inputs.
	TERRAENGINE_API void EstimateGeometryRouteCosts(GeometryRoutePolicyInput& input,
		const GeometryRouteWorkload& workload) noexcept;
}
