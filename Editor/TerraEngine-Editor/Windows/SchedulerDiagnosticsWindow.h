// SchedulerDiagnosticsWindow.h - editor surface for scheduler and ECS graph diagnostics.
//
// The window is intentionally hidden by default. Opening it asks the runtime to
// publish compact ECS graph metadata; closing it returns play mode to the cheap
// path where diagnostics are skipped.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class SchedulerDiagnosticsWindow : public EditorWindow
{
public:
	explicit SchedulerDiagnosticsWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	void DrawSchedulerSummary();
	void DrawScriptingSummary();
	void DrawEcsGraphSummaries();

	MainEditorSubsystem* owner = nullptr;
};
