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
		// Projected work estimates let the route planner keep ordinary triangles
		// on fixed-function raster while moving only quad-inefficient microgeometry
		// to the one-lane-per-triangle compute path.
		float projectedPixelArea = 0.0f;
		std::uint32_t triangleCount = 0u;
		bool preferCompute = false;
		// A prior stable decision enables asymmetric enter/exit thresholds. Tests,
		// captures, and future backend calibration can omit it without changing the
		// deterministic stateless default.
		VirtualGeometryRasterRoute priorRoute = VirtualGeometryRasterRoute::Indexed;
		bool priorRouteValid = false;
		// Callers may use this value to map stable-ID ordered results back to
		// packet ownership without changing packet order.
		std::uint32_t sourceOrdinal = 0;
	};

	// Compute raster wins only when the average triangle covers less than a
	// pixel and the packet rectangle is bounded. The rectangle guard prevents
	// one long, thin triangle from turning the conservative pixel loop into a
	// worse choice than fixed-function rasterization.
	struct VirtualGeometryRasterCalibration
	{
		float computeEnterAverageTrianglePixelArea = 0.85f;
		float computeExitAverageTrianglePixelArea = 1.15f;
		float maximumPacketPixelArea = 65536.0f;
		// Estimated triangles touching the same pixel are a portable contention
		// proxy. The limit avoids forcing a vendor-specific atomic model into the
		// API-neutral route planner.
		float maximumEstimatedAtomicContention = 12.0f;
	};
	inline constexpr VirtualGeometryRasterCalibration kDefaultVirtualGeometryRasterCalibration{};
	inline constexpr float kVirtualGeometryComputeAverageTrianglePixelArea =
		kDefaultVirtualGeometryRasterCalibration.computeEnterAverageTrianglePixelArea;
	inline constexpr float kVirtualGeometryComputeMaximumPacketPixelArea =
		kDefaultVirtualGeometryRasterCalibration.maximumPacketPixelArea;
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
		std::uint32_t routeSwitchCount = 0;
		float averageTrianglePixelArea = 0.0f;
		float peakEstimatedAtomicContention = 0.0f;
		bool failOpen = false;
		bool hybridCoverageComplete = false;
		std::vector<std::uint64_t> fallbackStableIds;
	};
	[[nodiscard]] VirtualGeometryRasterBinPlan BuildVirtualGeometryRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate> &, std::uint32_t materialBinCapacity,
		VirtualGeometryRasterCalibration calibration = kDefaultVirtualGeometryRasterCalibration);
	// Hybrid validation is a deterministic mixture of the three real routes.
	// It never creates a synthetic fourth route, and it uses Indexed whenever a
	// requested accelerated route is not supported by a candidate.
	[[nodiscard]] VirtualGeometryRasterBinPlan BuildVirtualGeometryHybridRasterBinPlan(
		const std::vector<VirtualGeometryRasterCandidate> &, std::uint32_t materialBinCapacity);
} // namespace TerraEngine::Renderer
