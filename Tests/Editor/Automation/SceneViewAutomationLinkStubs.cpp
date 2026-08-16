#include "Editor/TerraEngine-Editor/Windows/SceneViewWindow.h"

#include <cmath>

namespace SceneOverlay = TerraEngine::Editor::SceneViewOverlay;

// Editor automation tests link the service without the complete Dear ImGui
// window stack. These narrow definitions preserve the semantic validation
// contract used by service tests; the installed-editor target links and runs
// the production definitions from SceneViewWindow.cpp.
nlohmann::json SceneViewWindow::GetAutomationState() const
{
	const auto encodePosition = [](const TerraEngine::Scene::WorldPosition& value) {
		return nlohmann::json{
			{"cell", {
				{"x", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.x)},
				{"y", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.y)},
				{"z", TerraEngine::Scene::EncodeWorldCellAxis(value.cell.z)}}},
			{"offsetMetres", {value.offsetMetres.x, value.offsetMetres.y, value.offsetMetres.z}}};
	};
	return {
		{"camera", {{"position", encodePosition(m_CameraPosition)}, {"fieldOfViewDegrees", m_Fov}}},
		{"visible", IsVisible()},
		{"selectedObjectId", nullptr}
	};
}

bool SceneViewWindow::SetAutomationCamera(
	const TerraEngine::Scene::WorldPosition& position,
	const TerraEngine::Scene::WorldPosition& focus,
	std::string& outError)
{
	const TerraEngine::Scene::WorldCoordinateResult normalizedPosition = TerraEngine::Scene::NormalizeWorldPosition(position);
	const TerraEngine::Scene::WorldCoordinateResult normalizedFocus = TerraEngine::Scene::NormalizeWorldPosition(focus);
	if (!normalizedPosition.Succeeded() || !normalizedFocus.Succeeded())
	{
		outError = "Camera position and focus must be normalized canonical locations.";
		return false;
	}
	const TerraEngine::Scene::WorldLocalizationResult localized = TerraEngine::Scene::LocalizeWorldPosition(
		normalizedFocus.position,
		normalizedPosition.position,
		1000.0);
	if (!localized.Succeeded() || glm::length(glm::vec3(localized.offsetMetres)) <= 0.0001f)
	{
		outError = "Camera focus must be a distinct location within the bounded Scene View working frame.";
		return false;
	}
	m_CameraPosition = normalizedPosition.position;
	m_CameraFocus = normalizedFocus.position;
	m_CameraFocusInitialized = true;
	m_CameraFocusDistance = glm::length(glm::vec3(localized.offsetMetres));
	outError.clear();
	return true;
}

bool SceneViewWindow::FrameAutomationObject(entt::entity, std::string& outError)
{
	outError = "Object framing requires the installed editor's live Scene View.";
	return false;
}

bool SceneViewWindow::SetAutomationTool(std::string_view tool, std::string& outError)
{
	if (tool == "hand") m_ToolState.activeTool = SceneOverlay::SceneViewTool::Hand;
	else if (tool == "move") m_ToolState.activeTool = SceneOverlay::SceneViewTool::Move;
	else if (tool == "rotate") m_ToolState.activeTool = SceneOverlay::SceneViewTool::Rotate;
	else if (tool == "scale") m_ToolState.activeTool = SceneOverlay::SceneViewTool::Scale;
	else if (tool == "none") m_ToolState.activeTool = SceneOverlay::SceneViewTool::None;
	else
	{
		outError = "Tool must be one of: hand, move, rotate, scale, none.";
		return false;
	}
	outError.clear();
	return true;
}

bool SceneViewWindow::SetAutomationOrientation(std::string_view orientation, std::string& outError)
{
	if (orientation == "local" || orientation == "global")
	{
		m_ToolState.orientationMode = orientation == "local"
			? SceneOverlay::OrientationMode::Local
			: SceneOverlay::OrientationMode::Global;
		outError.clear();
		return true;
	}
	if (orientation == "perspective" || orientation == "orthographic")
	{
		m_ToolState.projectionMode = orientation == "perspective"
			? SceneOverlay::ProjectionMode::Perspective
			: SceneOverlay::ProjectionMode::Orthographic;
		outError.clear();
		return true;
	}
	outError = "Orientation is not supported by the editor-test Scene View double.";
	return false;
}

bool SceneViewWindow::SetAutomationSnapping(bool enabled, const std::optional<glm::vec3>& values, std::string& outError)
{
	if (values.has_value() && (!std::isfinite(values->x) || !std::isfinite(values->y) || !std::isfinite(values->z)
		|| values->x <= 0.0f || values->y <= 0.0f || values->z <= 0.0f))
	{
		outError = "Snap values must be finite positive increments.";
		return false;
	}
	m_ToolState.snapEnabled = enabled;
	if (values.has_value()) m_ToolState.snapValues = {values->x, values->y, values->z};
	outError.clear();
	return true;
}

bool SceneViewWindow::SetAutomationVisualization(std::string_view mode, std::string& outError)
{
	if (mode != "off" && mode != "none" && mode != "lod" && mode != "cluster"
		&& mode != "meshlet_id" && mode != "meshlet-id" && mode != "bounds"
		&& mode != "culling" && mode != "residency" && mode != "hiz"
		&& mode != "hi-z" && mode != "error")
	{
		outError = "Visualization mode is not allowlisted.";
		return false;
	}
	outError.clear();
	return true;
}
