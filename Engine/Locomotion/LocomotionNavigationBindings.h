// Protected navigation capture and immutable validity checks for physical targets.
#pragma once

#include "LocomotionPreparation.h"

namespace TerraEngine::Locomotion {
	// Capture is coordinator-only. The returned guard owns copied identities, not
	// live component pointers; Physics calls it only at protected boundaries.
	[[nodiscard]] std::shared_ptr<const Physics::CharacterMotionValidity> CaptureNavigationBinding(
		LocomotionPreparationInput& input);
}
