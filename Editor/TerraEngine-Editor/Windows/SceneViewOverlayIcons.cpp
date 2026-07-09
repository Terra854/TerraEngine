// SceneViewOverlayIcons.cpp - Original TerraEngine Scene View icon metadata.

#include "SceneViewOverlayIcons.h"

#include <algorithm>
#include <unordered_set>

namespace TerraEngine::Editor::SceneViewOverlay
{
	std::vector<OverlayIconDefinition> BuildDefaultIconDefinitions()
	{
		return {
			{"tool.hand", "Hand Tool", IconCategory::Toolbar, "hand-pan", true, 26.0f, 100u, "Pan or fly the Scene View camera", "Hand Tool", "outlined"},
			{"tool.move", "Move Tool", IconCategory::Toolbar, "axis-arrows", true, 26.0f, 100u, "Move selected objects", "Move Tool", "outlined"},
			{"tool.rotate", "Rotate Tool", IconCategory::Toolbar, "orbit-ring", true, 26.0f, 100u, "Rotate selected objects", "Rotate Tool", "outlined"},
			{"tool.scale", "Scale Tool", IconCategory::Toolbar, "corner-scale", true, 26.0f, 100u, "Scale selected objects", "Scale Tool", "outlined"},
			{"toolbar.pivot", "Center/Pivot", IconCategory::Toolbar, "pivot-dot", true, 26.0f, 90u, "Switch between center and pivot handles", "Center Pivot Toggle", "outlined"},
			{"toolbar.orientation", "Local/Global", IconCategory::Toolbar, "space-cube", true, 26.0f, 90u, "Switch between local and global orientation", "Local Global Toggle", "outlined"},
			{"toolbar.snap", "Snap", IconCategory::Toolbar, "magnet-grid", true, 26.0f, 80u, "Enable transform snapping", "Snap Toggle", "outlined"},
			{"toolbar.grid", "Grid", IconCategory::Grid, "grid-lines", true, 26.0f, 70u, "Show or hide the Scene View grid", "Grid Toggle", "outlined"},
			{"marker.camera", "Camera", IconCategory::Camera, "camera-frustum", true, 22.0f, 70u, "Camera object", "Camera Marker", "outlined"},
			{"marker.directionalLight", "Directional Light", IconCategory::DirectionalLight, "sun-rays", true, 22.0f, 65u, "Directional light object", "Directional Light Marker", "outlined"},
			{"marker.pointLight", "Point Light", IconCategory::PointLight, "light-bulb", true, 22.0f, 60u, "Point light object", "Point Light Marker", "outlined"},
			{"marker.spotLight", "Spot Light", IconCategory::SpotLight, "spot-cone", true, 22.0f, 60u, "Spot light object", "Spot Light Marker", "outlined"},
			{"marker.empty", "Empty Object", IconCategory::EmptyObject, "empty-cross", true, 20.0f, 30u, "Empty transform object", "Empty Object Marker", "outlined"},
			{"marker.selected", "Selected Object", IconCategory::Selection, "selection-ring", true, 24.0f, 100u, "Selected object", "Selected Object Marker", "outlined"},
			{"orientation.widget", "Orientation Widget", IconCategory::Orientation, "axis-widget", true, 32.0f, 100u, "Snap the Scene View camera to an axis", "Orientation Widget", "outlined"},
			{"toolbar.diagnostics", "Diagnostics", IconCategory::Diagnostics, "pulse-bars", false, 26.0f, 20u, "Show Scene View diagnostics overlay", "Diagnostics Toggle", "outlined"}
		};
	}

	const OverlayIconDefinition* FindIconDefinition(const std::vector<OverlayIconDefinition>& icons, std::string_view iconId)
	{
		const auto iterator = std::find_if(icons.begin(), icons.end(), [&](const OverlayIconDefinition& icon) {
			return icon.iconId == iconId;
		});
		return iterator == icons.end() ? nullptr : &(*iterator);
	}

	bool ValidateIconDefinitions(const std::vector<OverlayIconDefinition>& icons, std::string* outError)
	{
		std::unordered_set<std::string> ids;
		for (const OverlayIconDefinition& icon : icons)
		{
			if (icon.iconId.empty() || icon.displayName.empty() || icon.tooltip.empty() || icon.accessibilityName.empty())
			{
				if (outError != nullptr)
				{
					*outError = "Scene View icon definitions require id, display name, tooltip, and accessibility name.";
				}
				return false;
			}
			if (icon.minimumHitSize < 18.0f)
			{
				if (outError != nullptr)
				{
					*outError = "Scene View icon hit targets must remain large enough for reliable mouse interaction.";
				}
				return false;
			}
			if (!ids.insert(icon.iconId).second)
			{
				if (outError != nullptr)
				{
					*outError = "Scene View icon ids must be unique.";
				}
				return false;
			}
		}
		return true;
	}

	const char* ToolbarGlyph(SceneViewTool tool)
	{
		switch (tool)
		{
		case SceneViewTool::Hand: return "H";
		case SceneViewTool::Move: return "M";
		case SceneViewTool::Rotate: return "R";
		case SceneViewTool::Scale: return "S";
		case SceneViewTool::None: return "-";
		default: return "?";
		}
	}
}
