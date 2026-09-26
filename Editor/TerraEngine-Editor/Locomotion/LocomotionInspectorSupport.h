// LocomotionInspectorSupport.h - UI-independent character movement authoring and diagnostics.

#pragma once

#include "Components/CharacterLocomotionComponent.h"
#include "Locomotion/LocomotionTypes.h"

#include <cstdint>
#include <string>

namespace TerraEngine::Editor::Locomotion {
	struct LocomotionAuthoringValidation {
		bool accepted = false;
		std::string field;
		std::string explanation;
	};

	// Validate one detached, complete authoring candidate before the Inspector
	// includes it in a multi-object transaction. No live component is changed.
	[[nodiscard]] LocomotionAuthoringValidation ValidateCharacterLocomotionDraft(
		const CharacterLocomotionComponent& candidate);

	struct LocomotionRuntimeDisplay {
		std::string authority;
		std::string mode;
		std::string binding;
		std::string navigation;
		std::string animation;
		double desiredSpeed = 0.0;
		double realizedSpeed = 0.0;
		std::uint64_t physicalTick = 0U;
	};

	// Convert one immutable runtime observation into readable, read-only labels.
	// The caller chooses the observation from a copied publication root.
	[[nodiscard]] LocomotionRuntimeDisplay DescribeLocomotionRuntime(
		const TerraEngine::Locomotion::LocomotionObservation& observation);
}
