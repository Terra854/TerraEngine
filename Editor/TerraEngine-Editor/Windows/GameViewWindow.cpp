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
}

void GameViewWindow::OnGui() {
    // Collect cameras from scene and register them for Game View
    auto cameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
    for (auto obj : cameras) {
        Transform* transform = obj.getComponent<Transform>();
        CameraComponent* camera = obj.getComponent<CameraComponent>();
        
        // Compute world transform (simplified here, should ideally use hierarchical)
        // For now, let's assume root or get the helper
        // Since we are in the editor, we can use ObjectManager helper
        
        // Actually, we can just use the same logic as RenderSystem but here
        // or just let RenderSystem handle it if no GameViewCameras are set.
        // But if we want Game View to be "what game cameras see", and Scene View to be "what editor sees",
        // we should explicitly tell RenderSystem.
        
        glm::vec3 pos = transform->position;
        glm::quat rot = transform->rotation;
        
        const glm::vec3 defaultForward(0.0f, 0.0f, -1.0f);
        const glm::vec3 forward = rot * defaultForward;
        glm::vec3 up = rot * camera->up;
        if (glm::length(up) < 0.0001f) up = glm::vec3(0.0f, 1.0f, 0.0f);

        glm::mat4 view = glm::lookAt(pos, pos + forward, glm::normalize(up));
        
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        float aspect = contentSize.x / contentSize.y;
        if (aspect <= 0.0f) aspect = 1.0f;
        
        glm::mat4 proj = glm::perspective(glm::radians(camera->verticalFovDegrees), aspect, camera->nearPlane, camera->farPlane);
        proj[1][1] *= -1; // Vulkan flip
        
        RenderSystem::Instance().SetGameViewCamera(view, proj, pos);
    }

    void* texID = m_Owner->GetGameTextureID();
    
    if (texID) {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        ImGui::Image((ImTextureID)texID, contentSize);
    } else {
        ImGui::Text("Scene rendering not available.");
    }
}
