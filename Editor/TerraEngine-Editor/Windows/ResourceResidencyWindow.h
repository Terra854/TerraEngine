// ResourceResidencyWindow.h - editor diagnostics for loaded RAM/VRAM residency.
//
// This window is hidden by default. Opening it enables detailed per-asset
// residency diagnostics; closing it returns ResourceManager to summary-only
// collection so normal editor frames do not pay table-building costs.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class ResourceResidencyWindow : public EditorWindow
{
public:
	explicit ResourceResidencyWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;
	bool OnCloseRequested() override;

private:
	void DrawSummary();
	void DrawCandidates();
	void DrawReleaseRecords();

	MainEditorSubsystem* owner = nullptr;
};
