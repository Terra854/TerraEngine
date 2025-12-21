#include "SceneViewWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include <imgui.h>

SceneViewWindow::SceneViewWindow(MainEditorSubsystem* owner)
    : EditorWindow("Scene View"), m_Owner(owner) {
}

void SceneViewWindow::OnGui() {
    void* texID = m_Owner->GetSceneTextureID();
    
    if (texID) {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        
        // Constrain to 16:9
        float targetRatio = 16.0f / 9.0f;
        float currentRatio = contentSize.x / contentSize.y;
        ImVec2 imageSize = contentSize;
        
        if (currentRatio > targetRatio) {
            // Window is wider than target, limit width
            imageSize.x = contentSize.y * targetRatio;
        } else {
            // Window is taller than target, limit height
            imageSize.y = contentSize.x / targetRatio;
        }
        
        // Center image
        float cursorX = ImGui::GetCursorPosX() + (contentSize.x - imageSize.x) * 0.5f;
        float cursorY = ImGui::GetCursorPosY() + (contentSize.y - imageSize.y) * 0.5f;
        ImGui::SetCursorPos(ImVec2(cursorX, cursorY));
        
        ImGui::Image((ImTextureID)texID, imageSize);
    } else {
        ImGui::Text("Scene rendering not available.");
    }
}
