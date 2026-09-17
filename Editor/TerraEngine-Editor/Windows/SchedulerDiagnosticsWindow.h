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
	void OnBeforeBegin() override;
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	void DrawSchedulerSummary();
	void DrawWorkerCoverageSummary();
	void DrawParallelFrameSummary();
	void DrawExecutionTelemetrySummary();
	void DrawHardBoundarySummary();
	void DrawSafetyViolationSummary();
	void DrawRuntimeLayoutSummary();
	void DrawScriptingSummary();
	void DrawRenderGraphSummary();
	void DrawEcsGraphSummaries();

	MainEditorSubsystem* owner = nullptr;
};
