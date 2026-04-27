// SkySettingsWindow.h — Sky Settings Window Definition
//
// Defines the SkySettingsWindow class, providing an interface for configuring environmental rendering parameters.
// It allows users to adjust lighting, atmospheric effects, and skybox configurations for the current scene.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

// class SkySettingsWindow groups related state and operations so callers have one documented contract.
class SkySettingsWindow : public EditorWindow
{
public:
	SkySettingsWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	MainEditorSubsystem* m_Owner = nullptr;
};
