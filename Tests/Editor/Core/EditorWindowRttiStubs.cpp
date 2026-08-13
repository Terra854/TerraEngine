// EditorWindowRttiStubs.cpp - headless key functions for automation tests.

#include "Editor/TerraEngine-Editor/Windows/ProfilerWindow.h"
#include "Editor/TerraEngine-Editor/Windows/SceneViewWindow.h"

#if defined(TERRAENGINE_EDITOR_TESTS)
	// EditorAutomationService uses dynamic_pointer_cast to locate these window
	// types. The test binary intentionally excludes their GPU/ImGui-heavy
	// production translation units, so portable toolchains need headless key
	// functions to own the vtables and RTTI used by that lookup.
	struct ProfilerWindowCache {};

	ProfilerWindow::~ProfilerWindow() = default;
	void ProfilerWindow::OnBeforeBegin() {}
	void ProfilerWindow::OnGui() {}
	void SceneViewWindow::OnGui() {}
#endif
