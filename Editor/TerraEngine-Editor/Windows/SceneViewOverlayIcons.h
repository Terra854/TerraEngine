// SceneViewOverlayIcons.h - Original TerraEngine Scene View icon metadata.

#pragma once

#include "SceneViewOverlayTypes.h"

#include <string_view>
#include <vector>

namespace TerraEngine::Editor::SceneViewOverlay
{
	[[nodiscard]] std::vector<OverlayIconDefinition> BuildDefaultIconDefinitions();
	[[nodiscard]] const OverlayIconDefinition* FindIconDefinition(const std::vector<OverlayIconDefinition>& icons, std::string_view iconId);
	[[nodiscard]] bool ValidateIconDefinitions(const std::vector<OverlayIconDefinition>& icons, std::string* outError = nullptr);
	[[nodiscard]] const char* ToolbarGlyph(SceneViewTool tool);
}
