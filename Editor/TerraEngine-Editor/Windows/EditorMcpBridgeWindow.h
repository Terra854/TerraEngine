// EditorMcpBridgeWindow.h - Native ImGui status window for local assistant access.
//
// The window surfaces bridge enablement, write mode, approvals, and recent
// activity so assistant automation is visible and controllable from the editor.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class EditorMcpBridgeWindow : public EditorWindow
{
public:
	explicit EditorMcpBridgeWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	MainEditorSubsystem* owner = nullptr;
};
