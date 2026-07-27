// GraphEvaluationTypes.h - Immutable copied pose data used by graph evaluators.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Assets/AnimationAssetTypes.h"
#include "Animation/Runtime/PoseBuffer.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace TerraEngine::Animation {
// This deliberately remains a value object. Worker evaluators receive copied
// spans of contributions and never read a live component, scene, or asset.
// Stable joint IDs make the relationship between transforms explicit instead
// of assuming that two unrelated pose buffers happen to use the same indices.
struct GraphPoseContribution {
	AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
	std::uint32_t jointCount = 0U;
	std::vector<std::uint64_t> jointStableIds;
	std::vector<AnimationTransform> joints;
	// Named values are stored in increasing stable-ID order. A channel omitted
	// by one compatible contribution has its authored default value of zero.
	std::vector<AnimationNamedFloatValue> morphs;
	std::vector<AnimationNamedFloatValue> curves;
	std::optional<AnimationRootMotionDelta> rootMotion;
	auto operator<=>(const GraphPoseContribution &) const = default;
};

// Validation is allocation-free so callers can reject corrupt worker packets
// before staging any blend or layer output.
TERRAENGINE_API AnimationStatus ValidateGraphPoseContribution(const GraphPoseContribution &contribution) noexcept;
} // namespace TerraEngine::Animation
