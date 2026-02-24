#pragma once
#include "../Framework/EditorWindow.h"
#include <vector>
#include <string>
#include <imgui.h>

class MainEditorSubsystem;

class GameViewWindow : public EditorWindow {
public:
    GameViewWindow(MainEditorSubsystem* owner);

protected:
    virtual void OnGui() override;

private:
    MainEditorSubsystem* m_Owner;

    struct AspectRatio {
        std::string Name;
        float Width;
        float Height;
        bool IsFixedResolution; // If true, Width/Height are pixels. If false, they form a ratio.
    };

    std::vector<AspectRatio> m_AspectRatios;
    int m_SelectedAspectRatioIndex = 0; // Default to Free Aspect

    float m_Scale = 1.0f;
    bool m_MaximizeOnPlay = false;
    bool m_MuteAudio = false;
    bool m_Stats = false;
    bool m_Gizmos = false;
	ImVec4 m_StatsToggleInactiveBg = ImVec4(0.26f, 0.26f, 0.26f, 0.35f);
	ImVec4 m_StatsToggleHoverBg = ImVec4(0.36f, 0.36f, 0.36f, 0.65f);
	ImVec4 m_StatsToggleActiveBg = ImVec4(0.41f, 0.41f, 0.41f, 0.95f);
	ImVec4 m_StatsToggleText = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);
	float m_StatsToggleRounding = 3.0f;
	ImVec2 m_StatsTogglePadding = ImVec2(10.0f, 4.0f);
};
