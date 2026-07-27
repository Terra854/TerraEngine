// FootPlacement.h - Immutable-query foot placement and pelvis compensation.
//
// Worker jobs receive every scene/physics fact through ProceduralQuerySnapshot.
// The solver never stores a live scene object, physics body, or callback pointer.

#pragma once

#include "Animation/Procedural/ProceduralTypes.h"

#include <span>
#include <vector>

namespace TerraEngine::Animation {
	enum class FootContactPhase : std::uint8_t {
		Released = 0U,
		Locked = 1U,
		Releasing = 2U
	};

	// Contact state is copied from the previous complete publication. Keeping the
	// target generation beside the locked transform prevents a reused object from
	// inheriting a former occupant's planted foot.
	struct FootContactState {
		std::uint64_t stableFootId = 0U;
		ProceduralTargetIdentity target;
		AnimationTransform lockedTransform{};
		AnimationFloat3 surfaceNormal{0.0F, 1.0F, 0.0F};
		float contribution = 0.0F;
		FootContactPhase phase = FootContactPhase::Released;

		auto operator<=>(const FootContactState&) const = default;
	};

	// One foot input contains authored pose data, reach policy, and the previously
	// published contact value. queryId selects one bounded copied query stream.
	struct FootPlacementFootInput {
		std::uint64_t stableFootId = 0U;
		std::uint64_t queryId = 0U;
		AnimationTransform animatedTransform{};
		AnimationFloat3 hipPosition{};
		FootContactState previousContact;
		float maximumReachMeters = 1.0F;
		float soleOffsetMeters = 0.0F;
		float positionWeight = 1.0F;
		float orientationWeight = 1.0F;
		bool lockOnContact = true;
	};

	struct FootPlacementRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		const ProceduralQuerySnapshot* querySnapshot = nullptr;
		std::span<const FootPlacementFootInput> feet;
		AnimationTransform pelvisTransform{};
		float previousPelvisOffsetMeters = 0.0F;
		float maximumPelvisDropMeters = 0.35F;
		float maximumPelvisRaiseMeters = 0.20F;
		float pelvisWeight = 1.0F;
		float deltaSeconds = 0.0F;
		float releaseSmoothingSeconds = 0.10F;
		float pelvisSmoothingSeconds = 0.08F;
		AnimationFixedTick maximumQueryAgeTicks = 1;
	};

	struct FootPlacementFootResult {
		std::uint64_t stableFootId = 0U;
		AnimationTransform transform{};
		FootContactState contact;
		float targetErrorMeters = 0.0F;
		bool reachClamped = false;
		ProceduralDiagnosticRecord diagnostic;
	};

	struct FootPlacementResult {
		ProceduralResultHeader header;
		AnimationTransform pelvisTransform{};
		float pelvisOffsetMeters = 0.0F;
		std::vector<FootPlacementFootResult> feet;
		std::vector<ProceduralDiagnosticRecord> diagnostics;
	};

	// The complete candidate is staged before output is replaced. Invalid or
	// allocation-failed work therefore cannot expose a half-updated set of feet.
	TERRAENGINE_API AnimationStatus SolveFootPlacement(
		const FootPlacementRequest& request,
		FootPlacementResult& output) noexcept;
}
