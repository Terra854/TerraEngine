#pragma once

#include "../Framework/EditorWindow.h"
#include "Engine/Profiling/ProfilerCapture.h"

class MainEditorSubsystem;

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
	void DrawModuleSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawSamples(const TerraEngine::Profiling::ProfilerFrameRecord& frame);

	MainEditorSubsystem* owner = nullptr;
	TerraEngine::Profiling::ProfilerModuleId selectedModule = TerraEngine::Profiling::ProfilerModuleId::CpuEngine;
};
