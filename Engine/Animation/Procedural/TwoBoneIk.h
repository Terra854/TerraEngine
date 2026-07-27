// TwoBoneIk.h - Analytic, finite two-segment inverse kinematics.

#pragma once

#include "Animation/Procedural/ProceduralTypes.h"

namespace TerraEngine::Animation {
	// All transforms and target values are already expressed in space. The graph
	// layer performs hierarchy conversion before scheduling this allocator-free job.
	struct TwoBoneIkRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		AnimationTransform rootTransform{};
		AnimationTransform middleTransform{};
		AnimationTransform endTransform{};
		AnimationFloat3 targetPosition{};
		AnimationFloat3 polePosition{0.0F, 1.0F, 0.0F};
		ProceduralJointLimit middleLimit{};
		ProceduralCoordinateSpace space = ProceduralCoordinateSpace::Model;
		std::uint64_t goalId = 0U;
		std::uint64_t producerOrder = 0U;
		std::int32_t priority = 0;
		float weight = 1.0F;
		float maximumStretchScale = 1.0F;
		float twistRadians = 0.0F;
		float toleranceMeters = kProceduralPositionToleranceMeters;
		bool allowStretch = false;
	};

	struct TwoBoneIkResult {
		ProceduralResultHeader header;
		AnimationTransform rootTransform{};
		AnimationTransform middleTransform{};
		AnimationTransform endTransform{};
		float upperLength = 0.0F;
		float lowerLength = 0.0F;
		float stretchScale = 1.0F;
		float bendRadians = 0.0F;
		float targetErrorMeters = 0.0F;
		bool usedPoleFallback = false;
		ProceduralDiagnosticRecord diagnostic;
	};

	// Geometric failures still return Ok with a finite state such as Singular or
	// Unreachable. API errors are reserved for malformed or stale copied inputs.
	TERRAENGINE_API AnimationStatus SolveTwoBoneIk(
		const TwoBoneIkRequest& request,
		TwoBoneIkResult& output) noexcept;
}
