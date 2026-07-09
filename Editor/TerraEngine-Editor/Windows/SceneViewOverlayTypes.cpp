// SceneViewOverlayTypes.cpp - Scene View overlay state helpers.

#include "SceneViewOverlayTypes.h"

#include <cmath>
#include <mutex>
#include <utility>

namespace TerraEngine::Editor::SceneViewOverlay
{
	namespace
	{
		std::mutex g_DiagnosticsMutex;
		SceneViewDiagnosticsSnapshot g_LatestDiagnosticsSnapshot;
	}

	const char* ToString(SceneViewTool tool)
	{
		switch (tool)
		{
		case SceneViewTool::Hand: return "hand";
		case SceneViewTool::Move: return "move";
		case SceneViewTool::Rotate: return "rotate";
		case SceneViewTool::Scale: return "scale";
		case SceneViewTool::None: return "none";
		default: return "unknown";
		}
	}

	const char* ToString(PivotMode mode)
	{
		switch (mode)
		{
		case PivotMode::Center: return "center";
		case PivotMode::Pivot: return "pivot";
		default: return "unknown";
		}
	}

	const char* ToString(OrientationMode mode)
	{
		switch (mode)
		{
		case OrientationMode::Global: return "global";
		case OrientationMode::Local: return "local";
		default: return "unknown";
		}
	}

	const char* ToString(ProjectionMode mode)
	{
		switch (mode)
		{
		case ProjectionMode::Perspective: return "perspective";
		case ProjectionMode::Orthographic: return "orthographic";
		default: return "unknown";
		}
	}

	const char* ToString(MarkerCategory category)
	{
		switch (category)
		{
		case MarkerCategory::Camera: return "camera";
		case MarkerCategory::DirectionalLight: return "directionalLight";
		case MarkerCategory::PointLight: return "pointLight";
		case MarkerCategory::SpotLight: return "spotLight";
		case MarkerCategory::EmptyObject: return "emptyObject";
		case MarkerCategory::SelectedObject: return "selectedObject";
		case MarkerCategory::Unsupported: return "unsupported";
		default: return "unknown";
		}
	}

	const char* ToString(InteractionRegion region)
	{
		switch (region)
		{
		case InteractionRegion::Toolbar: return "toolbar";
		case InteractionRegion::OrientationWidget: return "orientationWidget";
		case InteractionRegion::Marker: return "marker";
		case InteractionRegion::Gizmo: return "gizmo";
		case InteractionRegion::FlyCamera: return "flyCamera";
		case InteractionRegion::TransformDrag: return "transformDrag";
		case InteractionRegion::Picking: return "picking";
		case InteractionRegion::Viewport: return "viewport";
		case InteractionRegion::None: return "none";
		default: return "unknown";
		}
	}

	const char* ToString(TransformEditStatus status)
	{
		switch (status)
		{
		case TransformEditStatus::Previewing: return "previewing";
		case TransformEditStatus::Committed: return "committed";
		case TransformEditStatus::Cancelled: return "cancelled";
		case TransformEditStatus::Rejected: return "rejected";
		case TransformEditStatus::RolledBack: return "rolledBack";
		default: return "unknown";
		}
	}

	bool IsPositiveFinite(float value)
	{
		return std::isfinite(value) && value > 0.0f;
	}

	float SanitizePositive(float value, float fallback)
	{
		return IsPositiveFinite(value) ? value : fallback;
	}

	SceneOverlayPreference SanitizePreference(SceneOverlayPreference preference)
	{
		preference.translationSnap = SanitizePositive(preference.translationSnap, 0.5f);
		preference.rotationSnapDegrees = SanitizePositive(preference.rotationSnapDegrees, 15.0f);
		preference.scaleSnap = SanitizePositive(preference.scaleSnap, 0.1f);
		if (preference.toolbarCompactMode != "auto" && preference.toolbarCompactMode != "compact" && preference.toolbarCompactMode != "expanded")
		{
			preference.toolbarCompactMode = "auto";
		}
		return preference;
	}

	SceneViewToolState BuildToolStateFromPreference(const SceneOverlayPreference& preference, SceneViewTool activeTool, PivotMode pivotMode, OrientationMode orientationMode)
	{
		const SceneOverlayPreference sanitized = SanitizePreference(preference);
		SceneViewToolState state;
		state.activeTool = activeTool;
		state.pivotMode = pivotMode;
		state.orientationMode = orientationMode;
		state.snapEnabled = sanitized.snapEnabled;
		state.snapValues = { sanitized.translationSnap, sanitized.rotationSnapDegrees, sanitized.scaleSnap };
		state.gridVisible = sanitized.gridVisible;
		state.gizmosVisible = sanitized.gizmosVisible;
		state.iconsVisible = sanitized.iconsVisible;
		state.projectionMode = sanitized.projectionMode;
		state.lastChangedBy = LastChangedBy::RestoredPreference;
		return state;
	}

	SceneViewSchedulerDeclaration BuildSceneViewOverlaySchedulerDeclaration()
	{
		SceneViewSchedulerDeclaration declaration;
		declaration.readResources = {
			"EditorSelection",
			"EditorPreferences.SceneView",
			"TransformSnapshot",
			"CameraComponentSnapshot",
			"LightComponentSnapshot",
			"RendererRecoveryStateSnapshot"
		};
		declaration.writeResources = {
			"EditorPreferences.SceneView",
			"SceneViewDiagnosticsSnapshot",
			"UndoRedoTransformCommand"
		};
		declaration.workerSafePreparation = "Prepare immutable marker, toolbar, and orientation snapshots from editor-owned read state.";
		declaration.protectedBoundary = "Draw ImGui overlays, own pointer input, and commit undoable transform edits on the editor UI boundary.";
		declaration.visibilityBoundary = "Display-only state is visible immediately in Scene View; durable scene mutations are visible after protected editor commit or the declared transactional flush.";
		return declaration;
	}

	void to_json(nlohmann::json& json, const SceneOverlayPreference& preference)
	{
		const SceneOverlayPreference sanitized = SanitizePreference(preference);
		json = nlohmann::json{
			{"iconsVisible", sanitized.iconsVisible},
			{"cameraIconsVisible", sanitized.cameraIconsVisible},
			{"lightIconsVisible", sanitized.lightIconsVisible},
			{"emptyObjectIconsVisible", sanitized.emptyObjectIconsVisible},
			{"gridVisible", sanitized.gridVisible},
			{"gizmosVisible", sanitized.gizmosVisible},
			{"diagnosticsVisible", sanitized.diagnosticsVisible},
			{"snapEnabled", sanitized.snapEnabled},
			{"translationSnap", sanitized.translationSnap},
			{"rotationSnapDegrees", sanitized.rotationSnapDegrees},
			{"scaleSnap", sanitized.scaleSnap},
			{"projectionMode", ToString(sanitized.projectionMode)},
			{"orientationWidgetVisible", sanitized.orientationWidgetVisible},
			{"toolbarCompactMode", sanitized.toolbarCompactMode}
		};
	}

	void from_json(const nlohmann::json& json, SceneOverlayPreference& preference)
	{
		SceneOverlayPreference restored;
		restored.iconsVisible = json.value("iconsVisible", restored.iconsVisible);
		restored.cameraIconsVisible = json.value("cameraIconsVisible", restored.cameraIconsVisible);
		restored.lightIconsVisible = json.value("lightIconsVisible", restored.lightIconsVisible);
		restored.emptyObjectIconsVisible = json.value("emptyObjectIconsVisible", restored.emptyObjectIconsVisible);
		restored.gridVisible = json.value("gridVisible", restored.gridVisible);
		restored.gizmosVisible = json.value("gizmosVisible", restored.gizmosVisible);
		restored.diagnosticsVisible = json.value("diagnosticsVisible", restored.diagnosticsVisible);
		restored.snapEnabled = json.value("snapEnabled", restored.snapEnabled);
		restored.translationSnap = json.value("translationSnap", restored.translationSnap);
		restored.rotationSnapDegrees = json.value("rotationSnapDegrees", restored.rotationSnapDegrees);
		restored.scaleSnap = json.value("scaleSnap", restored.scaleSnap);
		restored.orientationWidgetVisible = json.value("orientationWidgetVisible", restored.orientationWidgetVisible);
		restored.toolbarCompactMode = json.value("toolbarCompactMode", restored.toolbarCompactMode);

		const std::string projectionMode = json.value("projectionMode", std::string(ToString(restored.projectionMode)));
		restored.projectionMode = projectionMode == "orthographic" ? ProjectionMode::Orthographic : ProjectionMode::Perspective;
		preference = SanitizePreference(restored);
	}

	nlohmann::json SerializeDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot)
	{
		return {
			{"surface", "SceneView"},
			{"rendererAvailable", snapshot.rendererAvailable},
			{"activeTool", ToString(snapshot.toolState.activeTool)},
			{"pivotMode", ToString(snapshot.toolState.pivotMode)},
			{"orientationMode", ToString(snapshot.toolState.orientationMode)},
			{"snapEnabled", snapshot.toolState.snapEnabled},
			{"snapValues", { snapshot.toolState.snapValues[0], snapshot.toolState.snapValues[1], snapshot.toolState.snapValues[2] }},
			{"gridVisible", snapshot.toolState.gridVisible},
			{"gizmosVisible", snapshot.toolState.gizmosVisible},
			{"iconsVisible", snapshot.toolState.iconsVisible},
			{"projectionMode", ToString(snapshot.projectionMode)},
			{"orientationWidgetVisible", snapshot.orientationWidget.visible},
			{"orientationHoveredAxis", snapshot.orientationWidget.hoveredAxis},
			{"visibleMarkerCountsByCategory", {
				{"camera", snapshot.cameraMarkerCount},
				{"directionalLight", snapshot.directionalLightMarkerCount},
				{"pointLight", snapshot.pointLightMarkerCount},
				{"spotLight", snapshot.spotLightMarkerCount},
				{"emptyObject", snapshot.emptyObjectMarkerCount},
				{"selectedObject", snapshot.selectedObjectMarkerCount}
			}},
			{"hoveredMarkerId", snapshot.hoveredMarkerId},
			{"selectedMarkerId", snapshot.selectedMarkerId},
			{"lastInteractionOwner", ToString(snapshot.lastInteractionOwner)},
			{"lastRejectedMutationReason", snapshot.lastRejectedMutationReason},
			{"disabledOverlayReason", snapshot.disabledOverlayReason},
			{"gameViewAffected", snapshot.gameViewAffected}
		};
	}

	void PublishLatestDiagnosticsSnapshot(const SceneViewDiagnosticsSnapshot& snapshot)
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
		g_LatestDiagnosticsSnapshot = snapshot;
	}

	SceneViewDiagnosticsSnapshot GetLatestDiagnosticsSnapshot()
	{
		std::lock_guard<std::mutex> lock(g_DiagnosticsMutex);
		return g_LatestDiagnosticsSnapshot;
	}
}
