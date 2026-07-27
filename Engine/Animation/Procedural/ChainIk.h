// ChainIk.h - Deterministic bounded IK for arbitrary joint chains.

#pragma once

#include "Animation/Procedural/ProceduralTypes.h"

#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct ChainIkJoint {
		std::uint64_t stableJointId = 0U;
		AnimationTransform transform{};
		ProceduralJointLimit limit{};
	};

	struct ChainIkRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		std::span<const ChainIkJoint> joints;
		const ProceduralGoalSnapshot* goalSnapshot = nullptr;
		std::uint32_t maximumIterations = kChainIkIterationDefault;
		float toleranceMeters = kProceduralPositionToleranceMeters;
	};

	struct ChainIkResult {
		ProceduralResultHeader header;
		std::vector<ChainIkJoint> joints;
		std::vector<ProceduralDiagnosticRecord> diagnostics;
		AnimationFloat3 resolvedTarget{};
		float targetErrorMeters = 0.0F;
		float finalContribution = 0.0F;
		std::uint32_t iterationCount = 0U;
		bool encounteredZeroLength = false;
	};

	// FABRIK-style passes never exceed maximumIterations (itself capped at 128).
	// All candidate storage is staged and bounded before output is replaced.
	TERRAENGINE_API AnimationStatus SolveChainIk(
		const ChainIkRequest& request,
		ChainIkResult& output) noexcept;
}
