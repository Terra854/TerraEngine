#include "GameViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/CameraComponent.h"
#include "Engine/Core/Object/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

#include "GameViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Scene/CameraComponent.h"
#include "Engine/Core/Object/Transform.h"
#include "Engine/Core/Object/ObjectManager.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>

GameViewWindow::GameViewWindow(MainEditorSubsystem* owner)
    : EditorWindow("Game View"), m_Owner(owner) {
    m_AspectRatios.push_back({ "Free Aspect", 0.0f, 0.0f, false });
    m_AspectRatios.push_back({ "16:9 Aspect", 16.0f, 9.0f, false });
    m_AspectRatios.push_back({ "16:10 Aspect", 16.0f, 10.0f, false });
    m_AspectRatios.push_back({ "Full HD (1920x1080)", 1920.0f, 1080.0f, true });
    m_AspectRatios.push_back({ "4K UHD (3840x2160)", 3840.0f, 2160.0f, true });
    m_AspectRatios.push_back({ "Stand. (4:3)", 4.0f, 3.0f, false });
}

void GameViewWindow::OnGui() {
    // --- Toolbar ---
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##aspect", m_AspectRatios[m_SelectedAspectRatioIndex].Name.c_str())) {
        for (int i = 0; i < m_AspectRatios.size(); i++) {
            bool isSelected = (m_SelectedAspectRatioIndex == i);
            if (ImGui::Selectable(m_AspectRatios[i].Name.c_str(), isSelected)) {
                m_SelectedAspectRatioIndex = i;
            }
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("Scale", &m_Scale, 1.0f, 5.0f, "%.1fx");
    
    ImGui::SameLine();
    ImGui::Checkbox("Maximize On Play", &m_MaximizeOnPlay);
    ImGui::SameLine();
    ImGui::Checkbox("Mute Audio", &m_MuteAudio);
    ImGui::SameLine();
    ImGui::Checkbox("Stats", &m_Stats);
    ImGui::SameLine();
    ImGui::Checkbox("Gizmos", &m_Gizmos);
    
    // --- Calculate Render Area ---
    ImVec2 availableSize = ImGui::GetContentRegionAvail();
    // Clamp to 1.0 minimal to avoid div by zero
    if (availableSize.x < 1.0f) availableSize.x = 1.0f;
    if (availableSize.y < 1.0f) availableSize.y = 1.0f;

    ImVec2 imageSize = availableSize;
    AspectRatio& option = m_AspectRatios[m_SelectedAspectRatioIndex];
    
    if (option.Width > 0 && option.Height > 0) {
        float targetRatio = option.Width / option.Height;
        float currentRatio = availableSize.x / availableSize.y;
        
        if (currentRatio > targetRatio) {
            // Window is wider than target ratio: Pillarbox (limit width)
            imageSize.x = availableSize.y * targetRatio;
            imageSize.y = availableSize.y;
        } else {
            // Window is taller than target ratio: Letterbox (limit height)
            imageSize.x = availableSize.x;
            imageSize.y = availableSize.x / targetRatio;
        }
    }
    
    // Apply Zoom Scale
    imageSize.x *= m_Scale;
    imageSize.y *= m_Scale;

    // --- Update Cameras ---
    // Use the calculated imageSize aspect ratio for the projection matrix
    float aspect = imageSize.x / imageSize.y;
    if (aspect <= 0.0f) aspect = 1.0f;

    auto cameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
    for (auto obj : cameras) {
        Transform* transform = obj.getComponent<Transform>();
        CameraComponent* camera = obj.getComponent<CameraComponent>();
        
        glm::vec3 pos = transform->position;
        glm::quat rot = transform->rotation;
        
        const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
        const glm::vec3 forward = rot * defaultForward;
        glm::vec3 up = rot * camera->up;
        if (glm::length(up) < 0.0001f) up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::mat4 view = glm::lookAt(pos, pos + forward, glm::normalize(up));
        glm::mat4 proj = glm::perspective(glm::radians(camera->verticalFovDegrees), aspect, camera->nearPlane, camera->farPlane);
        proj[1][1] *= -1; // Vulkan flip
        
        RenderSystem::Instance().SetGameViewCamera(view, proj, pos);
    }

    // --- Draw Render ---
    void* texID = m_Owner->GetGameTextureID();
    ImVec2 cursorPos = ImGui::GetCursorScreenPos();
    
    // Draw Background filling available area (using window background color)
    ImGui::GetWindowDrawList()->AddRectFilled(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), ImGui::GetColorU32(ImGuiCol_WindowBg));

    if (texID) {
        // Center the image
        float xOffset = (availableSize.x - imageSize.x) * 0.5f;
        float yOffset = (availableSize.y - imageSize.y) * 0.5f;
        
        // Clip to available area if scaled up
        ImGui::PushClipRect(cursorPos, ImVec2(cursorPos.x + availableSize.x, cursorPos.y + availableSize.y), true);
        
        ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + xOffset, cursorPos.y + yOffset));
        ImGui::Image((ImTextureID)texID, imageSize);
        
        ImGui::PopClipRect();
    } else {
        // Centered text
        const char* text = "No Camera Rendering";
        ImVec2 textSize = ImGui::CalcTextSize(text);
        ImGui::SetCursorScreenPos(ImVec2(cursorPos.x + (availableSize.x - textSize.x) * 0.5f, cursorPos.y + (availableSize.y - textSize.y) * 0.5f));
        ImGui::Text("%s", text);
    }
}
