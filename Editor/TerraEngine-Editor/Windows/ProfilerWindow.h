// ProfilerWindow.h — Profiler Window Definition
//
// Defines the ProfilerWindow class, which manages the UI state and rendering logic for the editor's profiling interface.
// It inherits from EditorWindow and integrates with the engine's profiling systems to visualize performance data.
// The class handles capturing and displaying timing information for various engine subsystems.

#pragma once

#include "../Framework/EditorWindow.h"
#include "Engine/Profiling/ProfilerCapture.h"

#include <memory>

class MainEditorSubsystem;
struct ProfilerWindowCache;


class ProfilerWindow : public EditorWindow
{
public:
	explicit ProfilerWindow(MainEditorSubsystem* owner);
	~ProfilerWindow() override;

protected:
	void OnBeforeBegin() override;
	void OnGui() override;

private:
	void RefreshLiveSnapshot();
	void RefreshSampleAnalysis(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawToolbar();
	void DrawCaptureList();
	void DrawFrameList();
	void DrawFrameDetails();
	void DrawCaptureEvidenceSummary();
	void DrawRendererRecoverySummary();
	void DrawPhysicsSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawFrameOverview(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawModuleSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawDiagnosticSummary(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawHotSamples(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawTimeline(const TerraEngine::Profiling::ProfilerFrameRecord& frame);
	void DrawSamples(const TerraEngine::Profiling::ProfilerFrameRecord& frame);

	MainEditorSubsystem* owner = nullptr;
	TerraEngine::Profiling::ProfilerModuleId selectedModule = TerraEngine::Profiling::ProfilerModuleId::CpuEngine;
	// The profiler window owns presentation snapshots instead of deep-copying
	// mutable engine capture state while every ImGui frame is being drawn.
	std::unique_ptr<ProfilerWindowCache> cache;
};
