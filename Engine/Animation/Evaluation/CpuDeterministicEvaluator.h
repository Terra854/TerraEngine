// CpuDeterministicEvaluator.h - Canonical CPU-only advanced animation composition.

#pragma once

#include "Animation/Graph/MotionMatchNodeEvaluator.h"
#include "Animation/Graph/ProceduralNodeEvaluator.h"
#include "Animation/Graph/RetargetNodeEvaluator.h"
#include "Animation/Ragdoll/RagdollBlend.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::Animation {
	// The order is a simulation contract, not an authored presentation choice.
	// Keep values append-only because rollback records persist these stage IDs.
	enum class CpuDeterministicAdvancedStage : std::uint8_t {
		MotionMatch = 0U,
		Retarget = 1U,
		Procedural = 2U,
		Ragdoll = 3U
	};

	// A ragdoll packet is copied and canonicalized before it is blended. This
	// prevents physics producer ordering from changing a deterministic pose.
	struct CpuDeterministicRagdollRequest {
		const PhysicsPoseSnapshot* physicsPose = nullptr;
		RagdollGenerationStamp expectedGeneration{};
		std::vector<RagdollBlendInput> jointInputs;
		float timeWeight = 1.0F;
	};

	// Every pointer names immutable worker input owned by the scheduler packet.
	// A supplied request is evaluated in the fixed stage order below regardless
	// of the order in which graph authoring or worker partitions produced it.
	struct CpuDeterministicEvaluationRequest {
		const GraphPoseContribution* inputPose = nullptr;
		const GraphPoseContribution* fallbackPose = nullptr;
		const MotionMatchGraphNodeRequest* motionMatch = nullptr;
		const RetargetGraphNodeRequest* retarget = nullptr;
		const ProceduralGraphEvaluationRequest* procedural = nullptr;
		const CpuDeterministicRagdollRequest* ragdoll = nullptr;
	};

	struct CpuDeterministicEvaluationResult {
		AnimationStatus status = AnimationStatus::NotReady;
		// A successful fallback remains observable: status is Ok because a complete
		// pose was published, while failureStatus identifies the rejected stage.
		AnimationStatus failureStatus = AnimationStatus::Ok;
		CpuDeterministicAdvancedStage failedStage = CpuDeterministicAdvancedStage::MotionMatch;
		GraphPoseContribution pose{};
		std::vector<CpuDeterministicAdvancedStage> evaluatedStages;
		bool usedFallback = false;
		bool usedGpuResult = false;
	};

	// Advanced CPU evaluation is independent of GPU output and worker count. It
	// quantizes every completed stage to a signed 16.16 lattice, stages all work,
	// and only replaces output after a complete pose or an explicit fallback is
	// available. Malformed requests fail closed instead of silently falling back.
	TERRAENGINE_API AnimationStatus EvaluateCpuDeterministicAdvancedPose(
		const CpuDeterministicEvaluationRequest& request,
		CpuDeterministicEvaluationResult& output) noexcept;
}
