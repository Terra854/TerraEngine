// GroundClearanceKernel.h -- Engine-owned upright capsule validation.
//
// The kernel validates original provider-neutral source geometry. It is kept
// independent from path-provider polygon tests so a coarse build never grants
// an unsupported, low-clearance, or physically blocked route segment.

#pragma once

#include "Navigation/Geometry/MeshColliderNavigationSource.h"
#include "Navigation/NavigationSettings.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Navigation
{
	struct GroundClearanceSource
	{
		GroundGeometryContribution contribution = GroundGeometryContribution::WalkableSupport;
		NavigationGeometryDocument geometry{};
	};

	struct GroundClearanceScene
	{
		std::vector<GroundClearanceSource> sources;
	};

	enum class GroundClearanceStatus : std::uint8_t
	{
		Accepted,
		InvalidInput,
		MissingSupport,
		InsufficientClearance,
		ExcessiveSlope,
		ExcessiveStep,
		CornerCollision,
		ResolutionLimit
	};

	struct GroundCapsuleSegmentRequest
	{
		GroundClearanceScene scene{};
		NavigationProfile profile{};
		NavigationGeometryVector3 start{};
		NavigationGeometryVector3 destination{};
		// The limit bounds only near-quantization source checks. Zero means a
		// near-threshold predicate must return ResolutionLimit rather than guess.
		std::uint32_t maximumRefinementSteps = 8U;
	};

	struct GroundClearanceResult
	{
		bool accepted = false;
		GroundClearanceStatus status = GroundClearanceStatus::InvalidInput;
		double minimumSupportFraction = 0.0;
		std::uint32_t refinementSteps = 0U;
	};

	// Validates the complete swept capsule segment. It does not move an object or
	// alter source data; it returns an explicit conservative result for callers to
	// map into route publication and gameplay-visible state.
	[[nodiscard]] TERRAENGINE_API GroundClearanceResult ValidateGroundCapsuleSegment(
		const GroundCapsuleSegmentRequest& request);
}
