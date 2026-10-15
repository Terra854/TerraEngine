// RendererScratchDiagnostics.h - Read-only JSON projection of copied evidence.
#pragma once

#include <nlohmann/json_fwd.hpp>

namespace TerraEngine::Renderer { struct RenderDiagnosticsSummary; }

namespace TerraEngine::Editor::Automation {
	// Input is an already copied renderer summary. This helper has no owner,
	// resource or renderer access and cannot reset storage or start frame work.
	[[nodiscard]] nlohmann::json SerializeRendererScratchDiagnostics(
		const TerraEngine::Renderer::RenderDiagnosticsSummary& summary);
}
