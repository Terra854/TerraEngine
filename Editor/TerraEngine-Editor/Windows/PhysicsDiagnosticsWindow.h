// PhysicsDiagnosticsWindow.h - opt-in read-only physics diagnostics surface.
//
// The window consumes immutable snapshots only. It never starts a physics job,
// requests extraction, or reaches into a provider, so opening it is safe while
// paused, shutting down, or running without a renderer.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class PhysicsDiagnosticsWindow final : public EditorWindow
{
public:
	explicit PhysicsDiagnosticsWindow(MainEditorSubsystem* owner);

protected:
	void OnGui() override;

private:
	void DrawCounters();
	void DrawTimings();
	void DrawSchedulerDeclarations();

	MainEditorSubsystem* owner = nullptr;
};
