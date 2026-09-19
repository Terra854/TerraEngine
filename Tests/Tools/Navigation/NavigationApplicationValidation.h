// NavigationApplicationValidation.h - actual-host NavigationAgent workload contract.
//
// The installed editor and exported launcher call this validation helper only
// after they have loaded the authored AgentWorkload fixture. It intentionally
// drives Engine::Update for export hosts, while editor-play callers observe one
// already-completed frame at a time to avoid recursively entering the scheduler.

#pragma once

#include "Core/Export.h"

#include <nlohmann/json_fwd.hpp>

class Engine;

namespace TerraEngine::Navigation::Validation {
	// Runs or advances one deterministic, application-hosted navigation workload.
	// Options are JSON because launcher and editor automation already exchange
	// bounded JSON envelopes; all values are validated and copied before use.
	TERRAENGINE_API nlohmann::json RunNavigationApplicationValidation(
		Engine& engine, const nlohmann::json& options);
}
