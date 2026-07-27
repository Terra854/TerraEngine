// MotionMatchNodeEvaluator.h - CPU-authoritative motion selection graph seam.

#pragma once

#include "Animation/Graph/LayerEvaluator.h"
#include "Animation/MotionMatching/MotionMatchDiagnostics.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace TerraEngine::Animation {
	enum class MotionMatchCompositionStage : std::uint8_t {
		Selection = 0U,
		TransitionBlend = 1U,
		Retarget = 2U,
		Procedural = 3U,
		Ragdoll = 4U,
		Recording = 5U
	};

	// Asset streaming resolves a chosen sample into an immutable copied pose before
	// this call. Candidate identity prevents a stale segment from being blended
	// after a database reload or a different CPU-authoritative choice.
	struct MotionMatchResolvedPose {
		MotionMatchCandidate candidate{};
		const GraphPoseContribution* pose = nullptr;
	};

	struct MotionMatchGraphNodeRequest {
		const MotionDatabaseAsset* database = nullptr;
		const MotionMatchQuery* query = nullptr;
		const MotionMatchGpuReadback* gpuReadback = nullptr;
		const GraphPoseContribution* currentPose = nullptr;
		MotionMatchResolvedPose resolvedPose{};
		const GraphPoseContribution* fallbackPose = nullptr;
		float transitionWeight = 1.0F;
		const AnimationMaskAsset* mask = nullptr;
		AnimationFixedTick evaluationTick = 0;
		bool retargetFollows = false;
		bool proceduralFollows = false;
		bool ragdollFollows = false;
		bool recordingRequested = false;
		bool rollbackRequested = false;
	};

	struct MotionMatchGraphSnapshot {
		AnimationAssetHandle database{};
		std::uint64_t schemaFingerprint = 0U;
		AnimationFixedTick evaluationTick = 0;
		MotionMatchCandidate chosen{};
		GraphPoseContribution pose{};
	};

	struct MotionMatchGraphNodeResult {
		AnimationStatus status = AnimationStatus::NotReady;
		MotionMatchSelection selection{};
		GraphPoseContribution pose{};
		std::vector<MotionMatchCompositionStage> orderedStages;
		std::optional<MotionMatchGraphSnapshot> recordingSnapshot;
		std::optional<MotionMatchGraphSnapshot> rollbackSnapshot;
		bool usedFallbackState = false;
	};

	TERRAENGINE_API AnimationStatus EvaluateMotionMatchGraphNode(
		const MotionMatchGraphNodeRequest& request,
		MotionMatchGraphNodeResult& output) noexcept;
}
