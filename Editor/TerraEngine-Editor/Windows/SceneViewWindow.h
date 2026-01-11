#pragma once
#include "../Framework/EditorWindow.h"
#include <glm/glm.hpp>

class MainEditorSubsystem;

class SceneViewWindow : public EditorWindow {
public:
    SceneViewWindow(MainEditorSubsystem* owner);

protected:
    virtual void OnGui() override;

private:
    void HandleCameraInput(float deltaTime);
    void DrawGizmos(const glm::mat4& view, const glm::mat4& proj);

    MainEditorSubsystem* m_Owner;

    // Camera State
    glm::vec3 m_CameraPos{ 0.0f, 5.0f, 10.0f };
    glm::vec3 m_CameraRot{ -0.35f, 0.0f, 0.0f }; // ~20 degrees down in radians
    float m_Fov = 60.0f;
    float m_CameraSpeed = 10.0f;
    float m_MouseSensitivity = 0.1f;
    bool m_IsFlyCamActive = false;

    // Gizmo State
    int m_CurrentGizmoOperation = -1; // -1 for none, or ImGuizmo::TRANSLATE etc.
    bool m_UsingSnap = false;
    float m_SnapValue[3] = { 0.5f, 0.5f, 0.5f };
};
