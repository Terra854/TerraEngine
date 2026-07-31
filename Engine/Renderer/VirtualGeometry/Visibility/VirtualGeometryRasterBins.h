#pragma once
#include "VirtualGeometryVisibility.h"
#include <cstdint>
#include <vector>
namespace TerraEngine::Renderer
{
	struct VirtualGeometryRasterCandidate
	{
		std::uint64_t stableId = 0;
		std::uint32_t materialBin = 0;
		bool hardwareSupported = true;
		bool computeSupported = true;
		bool alphaMasked = false;
		bool requiresStableIdentity = true;
		// Callers may use this value to map stable-ID ordered results back to
		// packet ownership without changing packet order.
		std::uint32_t sourceOrdinal = 0;
	};
	struct VirtualGeometryRasterBinPlan
	{
		// Routes are ordered by stable ID so identical packet sets produce the
		// same validation result even when worker jobs finish in a different order.
		std::vector<VirtualGeometryRasterRoute> routes;
		std::vector<std::uint64_t> routeStableIds;
		std::vector<std::uint32_t> routeSourceOrdinals;
		std::uint32_t overflowCount = 0;
		std::uint32_t hardwareCount = 0;
		std::uint32_t computeCount = 0;
		std::uint32_t indexedCount = 0;
		bool failOpen = false;
		bool hybridCoverageComplete = false;
		std::vector<std::uint64_t> fallbackStableIds;
	};
	[[nodiscard]] VirtualGeometryRasterBinPlan BuildVirtualGeometryRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate> &, std::uint32_t materialBinCapacity);
	// Hybrid validation is a deterministic mixture of the three real routes.
	// It never creates a synthetic fourth route, and it uses Indexed whenever a
	// requested accelerated route is not supported by a candidate.
	[[nodiscard]] VirtualGeometryRasterBinPlan BuildVirtualGeometryHybridRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate> &, std::uint32_t materialBinCapacity);
} // namespace TerraEngine::Renderer
