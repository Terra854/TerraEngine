// SceneViewWindow.h — Scene View Window Definition
//
// Defines the SceneViewWindow class, which provides the primary 3D viewport for scene editing and navigation.
// It manages the camera used for editor rendering and handles input for spatial manipulation of entities.

#pragma once
#include "../Framework/EditorWindow.h"
#include "SceneViewOrientationWidget.h"
#include "SceneViewOverlayIcons.h"
#include "SceneViewOverlayMarkers.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Utilities/UUID.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <unordered_set>
#include <vector>

class MainEditorSubsystem;

// class SceneViewWindow groups related state and operations so callers have one documented contract.
class SceneViewWindow : public EditorWindow {
public:
	SceneViewWindow(MainEditorSubsystem* owner);

	// Scene View automation uses semantic values instead of screen coordinates.
	// These methods run on the editor's protected update boundary, so they may
	// safely share the same state and validation rules as the visible toolbar.
	[[nodiscard]] nlohmann::json GetAutomationState() const;
	bool SetAutomationCamera(const glm::vec3& position, const glm::vec3& focus, std::string& outError);
	bool FrameAutomationObject(entt::entity entity, std::string& outError);
	bool SetAutomationTool(std::string_view tool, std::string& outError);
	bool SetAutomationOrientation(std::string_view orientation, std::string& outError);
	bool SetAutomationSnapping(bool enabled, const std::optional<glm::vec3>& values, std::string& outError);
	bool SetAutomationVisualization(std::string_view mode, std::string& outError);

protected:
	virtual void OnGui() override;

private:
	void LoadOverlayPreferencesIfNeeded();
	void PersistOverlayPreferences();
	void DrawSceneToolbar();
	void BuildSceneObjectMarkers(const glm::mat4& viewProjection, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawSceneObjectMarkers();
	bool DrawOrientationWidget(glm::mat4& view, const glm::mat4& projection, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void PublishSceneViewDiagnostics(bool rendererAvailable, const std::string& disabledOverlayReason = std::string());
	glm::vec3 GetCameraForward() const;
	glm::vec3 GetCameraUp() const;
	glm::vec3 ResolveSceneViewFocus(float fallbackDistance) const;
	float ResolveSceneViewFocusDistance(const glm::vec3& focus, float fallbackDistance) const;
	void RepositionCameraAroundFocus(const glm::vec3& focus, float distance);
	bool IsPointerInsideOrientationWidget(const ImVec2& viewportPos, const ImVec2& viewportSize) const;
	bool HandleSceneViewScrollZoom();
	bool HandleSceneViewPan(const ImVec2& viewportSize);
	void HandleCameraInput(float deltaTime, const ImVec2& viewportSize);
	void DrawGizmos(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void DrawPhysicsColliderEditHandles(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void BeginPhysicsColliderPreview(entt::entity entity, const TerraEngine::UUID& recordId);
	void RequestPhysicsColliderFitToRenderMesh(entt::entity entity);
	void CommitPhysicsColliderPreview();
	void CancelPhysicsColliderPreview();
	void DrawPhysicsDebugOverlay(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
	void ApplyPrefabContextTintForNextRender();
	void CollectFocusedPrefabObjectIds(entt::entity entity, std::unordered_set<TerraEngine::UUID>& objectIds);

	MainEditorSubsystem* m_Owner;

	// Camera State
	glm::vec3 m_CameraPos{ 0.0f, 5.0f, 10.0f };
	glm::vec3 m_CameraRot{ -0.35f, 0.0f, 0.0f }; // ~20 degrees down in radians
	float m_Fov = 60.0f;
	float m_CameraSpeed = 10.0f;
	float m_MouseSensitivity = 0.1f;
	bool m_IsFlyCamActive = false;
	bool m_IsSceneViewPanning = false;
	std::pair<double, double> m_StoredCursorPos;
	// Scene View camera snaps orbit around this authoring focus so orthographic
	// axis clicks rotate the scene instead of spinning the camera in place.
	glm::vec3 m_CameraFocus{ 0.0f, 0.0f, 0.0f };
	float m_CameraFocusDistance = 18.0f;
	bool m_CameraFocusInitialized = false;

	// Gizmo State
	int m_CurrentGizmoOperation = -1; // -1 for none, or ImGuizmo::TRANSLATE etc.
	bool m_UsingSnap = false;
	float m_SnapValue[3] = { 0.5f, 0.5f, 0.5f };
	float m_RotationSnapValue[3] = { 15.0f, 15.0f, 15.0f };
	float m_ScaleSnapValue[3] = { 0.1f, 0.1f, 0.1f };

	bool m_WasUsingGizmo = false;
	glm::vec3 m_GizmoStartPos;
	glm::quat m_GizmoStartRot;
	glm::vec3 m_GizmoStartScale;
	// Scene View keeps its debug mode separate from Game View so artists can
	// inspect editor-only culling while gameplay preview stays in another mode.
	VirtualGeometryDebugVisualizationMode m_VirtualGeometryDebugMode = VirtualGeometryDebugVisualizationMode::None;
	bool m_VirtualGeometryDebugModeInitialized = false;
	// Keep the temporary virtual-geometry counter panel opt-in until Scene View
	// gets a fuller diagnostics toolbar that can present this data cleanly.
	bool m_ShowVirtualGeometryDiagnostics = false;
	bool m_OverlayPreferencesLoaded = false;
	bool m_OverlayPreferenceDirty = false;
	float m_OrthographicHeight = 10.0f;
	TerraEngine::Editor::SceneViewOverlay::SceneViewToolState m_ToolState;
	TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference m_OverlayPreference;
	TerraEngine::Editor::SceneViewOverlay::ViewportInteractionContext m_InteractionContext;
	TerraEngine::Editor::SceneViewOverlay::OrientationWidgetState m_OrientationWidgetState;
	std::vector<TerraEngine::Editor::SceneViewOverlay::OverlayIconDefinition> m_IconDefinitions;
	std::vector<TerraEngine::Editor::SceneViewOverlay::SceneObjectMarker> m_ObjectMarkers;
	TerraEngine::Physics::PhysicsDebugDisplaySelection m_PhysicsDebugSelection;
	std::size_t m_PhysicsDebugPacketCount = 0U;
	std::size_t m_PhysicsDebugPrimitiveCount = 0U;
	bool m_PhysicsDebugVisible = false;
	// Collider handles edit this copy only. It deliberately outlives mouse
	// movement but never aliases a live ColliderSetComponent record.
	std::optional<TerraEngine::Editor::SceneViewOverlay::PhysicsShapeEditPreview> m_PhysicsShapePreview;
	TerraEngine::UUID m_SelectedColliderRecordId = TerraEngine::EMPTY_UUID;
	int m_PhysicsActiveHandle = -1;
	ImVec2 m_PhysicsHandleDragStart{};
	glm::vec3 m_PhysicsPreviewDragStartCenter{0.0f};
	glm::vec3 m_PhysicsPreviewDragStartExtents{0.5f};
	std::string m_PhysicsPreviewStatus;
	std::uint64_t m_PhysicsShapeCommitSequence = 0U;

	void HandlePicking(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
};
