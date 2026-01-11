#pragma once
#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class GameViewWindow : public EditorWindow {
public:
    GameViewWindow(MainEditorSubsystem* owner);

protected:
    virtual void OnGui() override;

private:
    MainEditorSubsystem* m_Owner;
};
