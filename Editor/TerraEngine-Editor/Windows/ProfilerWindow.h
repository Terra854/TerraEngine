// ProfilerWindow.h — Profiler Window Definition
//
// Defines the ProfilerWindow class, which manages the UI state and rendering logic for the editor's profiling interface.
// It inherits from EditorWindow and integrates with the engine's profiling systems to visualize performance data.
// The class handles capturing and displaying timing information for various engine subsystems.

#pragma once

#include "../Framework/EditorWindow.h"
#include "Engine/Profiling/ProfilerCapture.h"

class MainEditorSubsystem;

// class ProfilerWindow groups related state and operations so callers have one documented contract.
class ProfilerWindow : public EditorWindow
{
public:
	explicit ProfilerWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	void DrawToolbar();
	void DrawCaptureList();
	void DrawFrameList();
	void DrawFrameDetails();
	void DrawFrameOverview(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawModuleSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawHotSamples(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawSamples(const TerraEngine::Profiling::ProfilerFrameRecord& frame);

	MainEditorSubsystem* owner = nullptr;
	TerraEngine::Profiling::ProfilerModuleId selectedModule = TerraEngine::Profiling::ProfilerModuleId::CpuEngine;
};
