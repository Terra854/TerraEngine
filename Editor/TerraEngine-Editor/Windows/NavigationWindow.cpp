// NavigationWindow.cpp - thin presentation state over engine navigation APIs.

#include "NavigationWindow.h"

#include <utility>

namespace TerraEngine::Editor::Navigation {
	NavigationWindow::NavigationWindow()
		: EditorWindow("Navigation", false, "Navigation") {}

	void NavigationWindow::SetDiagnosticsWindowOpen(bool open) noexcept { SetVisible(open); }
	bool NavigationWindow::IsDiagnosticsWindowOpen() const noexcept { return IsVisible(); }
	void NavigationWindow::SetOverlayInput(NavigationSceneOverlayInput input) { overlayInput_ = std::move(input); }

	void NavigationWindow::Update() {
		// Closed windows perform no geometry extraction, preserving the editor's
		// idle cost while navigation diagnostics continue publishing independently.
		const bool diagnosticsWindowOpen = IsDiagnosticsWindowOpen();
		if(!diagnosticsWindowOpen) return;
		overlay_ = ExtractNavigationOverlay(overlayInput_);
	}

	const NavigationSceneOverlaySnapshot& NavigationWindow::Overlay() const noexcept { return overlay_; }

	void NavigationWindow::OnGui() {
		// The same open-state gate is used by Scene View. Rendering this small
		// summary therefore never requests data while the diagnostics window is off.
		Update();
		const NavigationSceneOverlaySnapshot& overlay = Overlay();
		ImGui::TextUnformatted("Ground navigation diagnostics");
		ImGui::Separator();
		ImGui::Text("Coverage: %zu", overlay.coverage.size());
		ImGui::Text("Connections: %zu", overlay.connectionEndpoints.size() / 2U);
		ImGui::Text("Route points: %zu", overlay.routePoints.size());
		ImGui::Text("Dirty coverage: %zu", overlay.dirtyCoverage.size());
		if(overlay.truncated)
			ImGui::TextUnformatted("Display is bounded; additional diagnostics were omitted.");
	}
}
