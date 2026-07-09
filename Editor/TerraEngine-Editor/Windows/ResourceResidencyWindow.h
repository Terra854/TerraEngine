// ResourceResidencyWindow.h - editor diagnostics for loaded RAM/VRAM residency.
//
// This window is hidden by default. Opening it enables detailed per-asset
// residency diagnostics; closing it returns ResourceManager to summary-only
// collection so normal editor frames do not pay table-building costs.

#pragma once

#include "../Framework/EditorWindow.h"

#include <memory>

class MainEditorSubsystem;

class ResourceResidencyWindow : public EditorWindow
{
public:
	explicit ResourceResidencyWindow(MainEditorSubsystem* owner);
	~ResourceResidencyWindow() override;

protected:
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	struct DiagnosticsCache;

	void DrawRendererRecoverySummary();
	void DrawSummary();
	void DrawStreamingSummary();
	void DrawVirtualGeometrySummary();
	void DrawCandidates();
	void DrawReleaseRecords();
	void DrawDiagnosticsFilters();
	void RefreshDiagnosticsCacheIfNeeded(bool forceRefresh);
	void InvalidateDiagnosticsCache();

	MainEditorSubsystem* owner = nullptr;
	std::unique_ptr<DiagnosticsCache> m_DiagnosticsCache;
	char m_FilterText[128] = {};
	int m_TypeFilter = 0;
	int m_MemoryFilter = 0;
	int m_LoadStateFilter = 0;
	bool m_ShowOnlyReleaseEligible = false;
};
