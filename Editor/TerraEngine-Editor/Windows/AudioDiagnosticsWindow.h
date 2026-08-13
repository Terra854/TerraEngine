// AudioDiagnosticsWindow.h - read-only immutable audio telemetry.

#pragma once

#include "../Framework/EditorWindow.h"

class MainEditorSubsystem;

class AudioDiagnosticsWindow final : public EditorWindow {
public:
	explicit AudioDiagnosticsWindow(MainEditorSubsystem* owner);
protected:
	void OnGui() override;
private:
	MainEditorSubsystem* owner = nullptr;
};
