#pragma once

// Use the engine include path; this test helper no longer lives beside RenderTypes.
#include "Renderer/RenderTypes.h"
#include <string>

namespace TerraEngine::Renderer
{
	// Fixture controls are optional for ordinary rendering. This pure admission
	// check never changes the caller's limits or consumes its one-shot fault.
	inline bool ValidateRendererEfficiencyFixtureShadowBatchControls(
		const std::optional<RendererEfficiencyFixtureShadowBatchControls>& controls,
		bool owned, std::string& reason)
	{
		reason.clear();
		if (!controls) return true;
		// Limits and fault injection belong exclusively to an owned validation
		// session. Zero means the backend must use its actual device limit.
		if (!owned) {
			reason = "Shadow batch fixture controls require an owned validation session.";
			return false;
		}
		if (controls->maxInstancesPerGroup > 65536U) {
			reason = "Shadow batch maxInstancesPerGroup exceeds 65536.";
			return false;
		}
		if (controls->maxTaskGroupsX > 1048576U) {
			reason = "Shadow batch maxTaskGroupsX exceeds 1048576.";
			return false;
		}
		// Runtime owns one-shot dispatch; validation preserves the request.
		return true;
	}
}
