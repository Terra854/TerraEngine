// SceneViewWindow.h — Scene View Window Definition
//
// Defines the SceneViewWindow class, which provides the primary 3D viewport for scene editing and navigation.
// It manages the camera used for editor rendering and handles input for spatial manipulation of entities.

#pragma once
#include "../Framework/EditorWindow.h"
#include "Engine/Utilities/UUID.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <unordered_set>

class MainEditorSubsystem;

// class SceneViewWindow groups related state and operations so callers have one documented contract.
class SceneViewWindow : public EditorWindow {
public:
	SceneViewWindow(MainEditorSubsystem* owner);

protected:
	virtual void OnGui() override;

private:
	void HandleCameraInput(float deltaTime);
	void DrawGizmos(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
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
	std::pair<double, double> m_StoredCursorPos;

	// Gizmo State
	int m_CurrentGizmoOperation = -1; // -1 for none, or ImGuizmo::TRANSLATE etc.
	bool m_UsingSnap = false;
	float m_SnapValue[3] = { 0.5f, 0.5f, 0.5f };

	bool m_WasUsingGizmo = false;
	glm::vec3 m_GizmoStartPos;
	glm::quat m_GizmoStartRot;
	glm::vec3 m_GizmoStartScale;

	void HandlePicking(const glm::mat4& view, const glm::mat4& proj, const ImVec2& viewportPos, const ImVec2& viewportSize);
};
