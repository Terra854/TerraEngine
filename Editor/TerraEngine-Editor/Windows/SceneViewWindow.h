#pragma once
#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class SceneViewWindow : public EditorWindow {
public:
    SceneViewWindow(MainEditorSubsystem* owner);

protected:
    virtual void OnGui() override;

private:
    MainEditorSubsystem* m_Owner;
};
