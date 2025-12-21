#include "EditorWindow.h"
#include <imgui_internal.h>

EditorWindow::EditorWindow(const std::string& title, bool startVisible, ImGuiWindowFlags flags)
    : m_Title(title), m_IsVisible(startVisible), m_Flags(flags) {
}

void EditorWindow::Draw() {
    if (!m_IsVisible) return;

    bool expanded = ImGui::Begin(m_Title.c_str(), nullptr, m_Flags);

    // Context Menu Logic (mimicking BeginWindowWithContextMenu)
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool tabHovered = false;
    if (window->DockNode && window->DockNode->TabBar) {
        ImGuiTabBar* tabBar = window->DockNode->TabBar;
        for (const auto& tab : tabBar->Tabs) {
            if (tab.ID == window->TabId) {
                ImVec2 tabMin = tabBar->BarRect.Min;
                tabMin.x += tab.Offset - tabBar->ScrollingAnim;
                ImVec2 tabMax = ImVec2(tabMin.x + tab.Width, tabBar->BarRect.Max.y);

                ImVec2 mousePos = ImGui::GetMousePos();
                if (mousePos.x >= tabMin.x && mousePos.x <= tabMax.x &&
                    mousePos.y >= tabMin.y && mousePos.y <= tabMax.y) {
                    tabHovered = true;
                }
                break;
            }
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup) || tabHovered) {
             ImGui::OpenPopup("WindowContextMenu");
        }
    }

    if (ImGui::BeginPopup("WindowContextMenu")) {
        OnContextMenu();
        if (ImGui::MenuItem("Close")) m_IsVisible = false;
        ImGui::EndPopup();
    }

    if (expanded) {
        OnGui();
    }
    ImGui::End();
}
