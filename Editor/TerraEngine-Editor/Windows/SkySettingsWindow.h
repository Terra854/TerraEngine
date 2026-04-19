#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class SkySettingsWindow : public EditorWindow
{
public:
	SkySettingsWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	MainEditorSubsystem* m_Owner = nullptr;
};
