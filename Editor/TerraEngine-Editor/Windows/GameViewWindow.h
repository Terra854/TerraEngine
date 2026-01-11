#pragma once
#include "../Framework/EditorWindow.h"
#include <vector>
#include <string>

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
};
