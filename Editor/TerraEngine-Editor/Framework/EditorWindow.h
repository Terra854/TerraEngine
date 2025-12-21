#pragma once
#include <string>
#include <imgui.h>

class EditorWindow {
public:
    EditorWindow(const std::string& title, bool startVisible = true, ImGuiWindowFlags flags = 0);
    virtual ~EditorWindow() = default;

    void Draw();
    
    bool IsVisible() const { return m_IsVisible; }
    void SetVisible(bool visible) { m_IsVisible = visible; }
    const std::string& GetTitle() const { return m_Title; }

protected:
    virtual void OnGui() = 0;
    virtual void OnContextMenu() {}

    std::string m_Title;
    bool m_IsVisible;
    ImGuiWindowFlags m_Flags;
};
