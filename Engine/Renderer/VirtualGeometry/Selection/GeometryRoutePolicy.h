// GeometryRoutePolicy.h - Correctness-first direct and virtual geometry routing.
//
// Route preparation consumes immutable cook/runtime summaries. It never reads
// live scene, resource, or backend state, so the same input can be evaluated on
// scheduler workers and reproduced by offline calibration tools.

#pragma once

#include "Core/Export.h"
#include "Renderer/VirtualGeometry/VirtualGeometryTypes.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace TerraEngine::Renderer
{
	struct TERRAENGINE_API GeometryRoutePolicyCandidateInput
	{
		bool valid = false;
		bool representationAvailable = true;
		bool correctnessCompatible = true;
		bool materialCompatible = true;
		bool deformationCompatible = true;
		bool viewsCompatible = true;
		bool capabilitiesAvailable = true;
		bool residencyUsable = true;
		float estimatedCost = 0.0F;

		[[nodiscard]] bool IsEligible() const noexcept;
	};

	struct TERRAENGINE_API GeometryRoutePolicyInput
	{
		GeometryRoutePolicyCandidateInput direct{};
		GeometryRoutePolicyCandidateInput virtualGeometryHardware{};
		GeometryRoutePolicyCandidateInput virtualGeometryCompute{};
		std::optional<GeometryRoute> authorOverride;
		float confidenceMargin = 0.05F;
		std::uint32_t minimumStableFrames = 3u;
		std::uint64_t policyIdentity = 1u;
		std::uint64_t calibrationIdentity = 1u;
	};

	[[nodiscard]] TERRAENGINE_API GeometryRouteDecision SelectGeometryRoute(
		const GeometryRoutePolicyInput &input);

	[[nodiscard]] TERRAENGINE_API GeometryRouteDecision SelectGeometryRoute(
		const GeometryRoutePolicyInput &input,
		const GeometryRoutePolicyState &previousState);

	// This compact execution record is used by scheduler and regression tests
	// to prove that a direct-only frame cannot accidentally manufacture VG work.
	struct TERRAENGINE_API GeometryRouteFramePlan
	{
		GeometryRoute selectedRoute = GeometryRoute::DirectMeshlet;
		std::uint32_t instances = 0u;
		std::vector<GeometryViewKind> viewKinds;
		std::uint32_t vgHierarchyTraversalCount = 0u;
		std::uint32_t vgPageRequestCount = 0u;
		std::uint32_t vgDecodeCount = 0u;
		std::uint32_t vgResidencyPublicationCount = 0u;
		std::uint32_t vgCompactionCount = 0u;
		std::uint32_t generatedVirtualGeometryPacketCount = 0u;
		std::uint32_t vgExecutorAllocationCount = 0u;
		std::uint32_t directBatchCount = 0u;
		std::uint32_t sharedGeometryInstanceCount = 0u;
		bool identityStableAcrossViews = false;
	};

	TERRAENGINE_API void BuildGeometryRouteFramePlan(GeometryRouteFramePlan &plan);
} // namespace TerraEngine::Renderer
