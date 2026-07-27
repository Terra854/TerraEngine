// RetargetNodeEvaluator.h - Cross-rig graph composition over immutable packets.

#pragma once

#include "Animation/Graph/LayerEvaluator.h"
#include "Animation/Retargeting/RetargetEvaluator.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	// These values describe the fixed semantic order shared by CPU and GPU graph
	// lowering. Recording consumes the final authoritative composition rather than
	// capturing a transient pose between retargeting, IK, and ragdoll blending.
	enum class RetargetCompositionStage : std::uint8_t {
		MotionSelection = 0U,
		Retarget = 1U,
		Procedural = 2U,
		Ragdoll = 3U,
		Recording = 4U
	};

	struct RetargetGraphNodeRequest {
		RetargetEvaluationRequest evaluation{};
		const GraphPoseContribution* targetBasePose = nullptr;
		AnimationGraphLayerMode layerMode = AnimationGraphLayerMode::Override;
		float controllerWeight = 1.0F;
		float nodeWeight = 1.0F;
		float layerWeight = 1.0F;
		const AnimationMaskAsset* mask = nullptr;
		const GraphPoseContribution* additiveReference = nullptr;
		// Hybrid callers name durable target-joint identities. The evaluator closes
		// the set over target ancestors before returning worker-owned indices.
		std::span<const std::uint64_t> hybridConsumerJointIds{};
		bool motionSelectionApplied = false;
		bool proceduralFollows = false;
		bool ragdollFollows = false;
		bool recordingRequested = false;
		bool rollbackRequested = false;
	};

	struct RetargetGraphNodeResult {
		RetargetEvaluationResult retarget{};
		GraphPoseContribution pose{};
		std::vector<std::uint32_t> hybridJointClosure;
		std::vector<RetargetCompositionStage> orderedStages;
		std::optional<GraphPoseContribution> recordingPose;
		std::optional<GraphPoseContribution> rollbackPose;
	};

	// The result is staged in full before output changes. A stale profile, invalid
	// mask, or missing Hybrid joint therefore leaves the prior graph publication
	// untouched and cannot expose a partially retargeted skeleton.
	TERRAENGINE_API AnimationStatus EvaluateRetargetGraphNode(
		const RetargetGraphNodeRequest& request,
		RetargetGraphNodeResult& output) noexcept;
}
