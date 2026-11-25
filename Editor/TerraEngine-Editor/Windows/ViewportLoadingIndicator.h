#pragma once

#include <algorithm>
#include <imgui.h>

namespace TerraEngine::Editor
{
	// Show activity without inventing a percentage for decode and GPU work.
	// Reserve the same content area as the viewport image when it is ready.
	inline void DrawViewportLoadingIndicator(const ImVec2& size)
	{
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const float width = std::min(240.0f, size.x);
		const float height = ImGui::GetFrameHeight();
		if (width > 0.0f && size.y >= height)
		{
			ImGui::SetCursorScreenPos(ImVec2(origin.x + (size.x - width) * 0.5f,
				origin.y + (size.y - height) * 0.5f));
			ImGui::ProgressBar(-1.0f - static_cast<float>(ImGui::GetTime()),
				ImVec2(width, height), "Loading scene...");
			ImGui::SetCursorScreenPos(origin);
		}
		ImGui::Dummy(size);
	}
}
