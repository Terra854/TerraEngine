// LocomotionValidationHarness.h - installed application validation entry point.
//
// Editor Play calls once after each normal frame. The export launcher calls
// repeatedly; the shared harness advances only the export Engine scheduler.
#pragma once

#include "Core/Export.h"

#include <nlohmann/json_fwd.hpp>

class Engine;

namespace TerraEngine::Locomotion::Validation {
	TERRAENGINE_API nlohmann::json RunLocomotionApplicationValidation(
		Engine& engine, const nlohmann::json& options);
}
