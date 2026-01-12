#include "SceneViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Input/InputSystem.h"
#include "Engine/Input/InputCodes.h"
#include "Engine/Window/WindowSystem.h"
#include <GLFW/glfw3.h>
#include "Engine/Core/Object/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

SceneViewWindow::SceneViewWindow(MainEditorSubsystem* owner)
    : EditorWindow("Scene View"), m_Owner(owner) {
    m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
}

void SceneViewWindow::OnGui() {
    ImGuizmo::BeginFrame();

    void* texID = m_Owner->GetSceneTextureID();
    
    // We draw the image filling the content region
    ImVec2 contentSize = ImGui::GetContentRegionAvail();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 cursorPos = ImGui::GetCursorScreenPos(); // Absolute position for ImGuizmo
    
    if (texID) {
        ImGui::Image((ImTextureID)texID, contentSize);
    } else {
        ImGui::Text("Scene rendering not available.");
    }

    // Prepare for Gizmos
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(cursorPos.x, cursorPos.y, contentSize.x, contentSize.y);

    // Calculate Camera Matrices
    glm::mat4 view = glm::mat4(1.0f);
    glm::mat4 proj = glm::mat4(1.0f);
    
    // m_CameraRot.x = pitch, .y = yaw
    glm::quat orientation = glm::quat(glm::vec3(m_CameraRot.x, m_CameraRot.y, 0.0f));
    glm::vec3 forward = orientation * glm::vec3(0, 0, -1);
    glm::vec3 up = orientation * glm::vec3(0, 1, 0);
    view = glm::lookAt(m_CameraPos, m_CameraPos + forward, up);
    
    float aspect = contentSize.x / contentSize.y;
    if (aspect <= 0.0f) aspect = 1.0f;
    glm::mat4 projNoFlip = glm::perspective(glm::radians(m_Fov), aspect, 0.1f, 1000.0f);
    proj = projNoFlip;
    proj[1][1] *= -1; // Vulkan flip

    bool isFocused = ImGui::IsWindowFocused();
    bool isHovered = ImGui::IsWindowHovered();

    if (isFocused || isHovered) {
        HandleCameraInput(ImGui::GetIO().DeltaTime);
    }
    
    // Set Scene View Camera for next render
    RenderSystem::Instance().SetSceneViewCamera(view, proj, m_CameraPos);

    DrawGizmos(view, projNoFlip);
}

void SceneViewWindow::HandleCameraInput(float deltaTime) {
    // Need to include WindowSystem and GLFW for this
    auto* window = static_cast<GLFWwindow*>(WindowSystem::Instance().GetNativeHandle());

    if (m_IsFlyCamActive) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            m_IsFlyCamActive = false;
            
            // Restore cursor visibility
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        } else {
            // Calculate delta manually to avoid ImGui delta issues with re-centering
            auto currentPos = InputSystem::Instance().GetCursorPosition();
            double deltaX = currentPos.first - m_StoredCursorPos.first;
            double deltaY = currentPos.second - m_StoredCursorPos.second;

            // Reset cursor to stored position to keep it locked
            if (deltaX != 0.0 || deltaY != 0.0) {
                InputSystem::Instance().SetCursorPosition(m_StoredCursorPos.first, m_StoredCursorPos.second);
            }

            // Fly Cam Movement
            glm::quat orientation = glm::quat(glm::vec3(m_CameraRot.x, m_CameraRot.y, 0.0f));
            glm::vec3 forward = orientation * glm::vec3(0, 0, -1);
            glm::vec3 right = orientation * glm::vec3(1, 0, 0);
            glm::vec3 up = orientation * glm::vec3(0, 1, 0);

            if (ImGui::IsKeyDown(ImGuiKey_W)) m_CameraPos += forward * m_CameraSpeed * deltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_S)) m_CameraPos -= forward * m_CameraSpeed * deltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_D)) m_CameraPos += right * m_CameraSpeed * deltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_A)) m_CameraPos -= right * m_CameraSpeed * deltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_E)) m_CameraPos += up * m_CameraSpeed * deltaTime;
            if (ImGui::IsKeyDown(ImGuiKey_Q)) m_CameraPos -= up * m_CameraSpeed * deltaTime;

            // Mouse Look
            if (deltaX != 0.0 || deltaY != 0.0) {
                m_CameraRot.y -= (float)deltaX * m_MouseSensitivity * 0.01f;
                m_CameraRot.x -= (float)deltaY * m_MouseSensitivity * 0.01f;
                
                // Clamp pitch
                if (m_CameraRot.x > glm::radians(89.0f)) m_CameraRot.x = glm::radians(89.0f);
                if (m_CameraRot.x < glm::radians(-89.0f)) m_CameraRot.x = glm::radians(-89.0f);
            }
        }
    } else {
        // Only activate if clicked THIS frame while hovered and not using gizmo
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered() && !ImGuizmo::IsUsing()) {
            m_IsFlyCamActive = true;
            m_StoredCursorPos = InputSystem::Instance().GetCursorPosition();
            
            // Hide the cursor explicitly since we tell ImGui not to touch it
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        }
    }
    
    // Gizmo Shortcuts
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) m_CurrentGizmoOperation = ImGuizmo::TRANSLATE;
        if (ImGui::IsKeyPressed(ImGuiKey_E)) m_CurrentGizmoOperation = ImGuizmo::ROTATE;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) m_CurrentGizmoOperation = ImGuizmo::SCALE;
    }
}

void SceneViewWindow::DrawGizmos(const glm::mat4& view, const glm::mat4& proj) {
    // Grid
    // ImGuizmo grid removed to be replaced by engine renderer grid
    // glm::mat4 identity = glm::mat4(1.0f);
    // ImGuizmo::DrawGrid(glm::value_ptr(view), glm::value_ptr(proj), glm::value_ptr(identity), 100.0f);

    // Selection
    auto selectedEntity = m_Owner->GetSelectedEntity();
    if (selectedEntity.has_value()) {
        auto& registry = ObjectManager::instance().getRegistry();
        if (registry.valid(*selectedEntity)) {
             Transform* transform = registry.try_get<Transform>(*selectedEntity);
             if (transform) {
                 glm::mat4 model = glm::mat4(1.0f);
                 model = glm::translate(model, transform->position);
                 model = model * glm::mat4_cast(transform->rotation);
                 model = glm::scale(model, transform->scale);

                 if (ImGuizmo::Manipulate(
                     glm::value_ptr(view), 
                     glm::value_ptr(proj), 
                     (ImGuizmo::OPERATION)m_CurrentGizmoOperation, 
                     ImGuizmo::LOCAL, 
                     glm::value_ptr(model),
                     nullptr, 
                     m_UsingSnap ? m_SnapValue : nullptr)) 
                 {
                     // Decompose and update
                     glm::vec3 scale;
                     glm::quat rotation;
                     glm::vec3 translation;
                     glm::vec3 skew;
                     glm::vec4 perspective;
                     glm::decompose(model, scale, rotation, translation, skew, perspective);
                     
                     transform->position = translation;
                     transform->rotation = rotation;
                     transform->scale = scale;
                 }
             }
        }
    }
}
