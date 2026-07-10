// EditorWindow.cpp — Native editor window implementation
//
// Implements the common drawing logic for editor windows, including the ImGui Begin/End cycle and tab-aware context menu handling. It ensures that window visibility state is correctly synchronized and provides a framework for specialized window subclasses.

#include "EditorWindow.h"
#include <imgui_internal.h>
#include <utility>

EditorWindow::EditorWindow(const std::string& title, bool startVisible, ImGuiWindowFlags flags)
	: m_Title(title), m_IsVisible(startVisible), m_Flags(flags) {
}

EditorWindow::EditorWindow(const std::string& title, bool startVisible, std::string windowMenuCategoryPath, ImGuiWindowFlags flags)
	: m_Title(title), m_WindowMenuCategoryPath(std::move(windowMenuCategoryPath)), m_IsVisible(startVisible), m_Flags(flags) {
	// The base stores the declaration only. MainEditorSubsystem interprets the
	// path generically, so categories remain owned by each concrete window.
}

// EditorWindow::Draw is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void EditorWindow::Draw() {
	if (!m_IsVisible) return;

	bool nextVisible = m_IsVisible;
	bool expanded = ImGui::Begin(m_Title.c_str(), &nextVisible, m_Flags);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!nextVisible && m_IsVisible) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (OnCloseRequested()) {
			m_IsVisible = false;
		} else {
			nextVisible = true;
		}
	}
	m_IsVisible = nextVisible;

	// Context Menu Logic (mimicking BeginWindowWithContextMenu)
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	bool tabHovered = false;
	if (window->DockNode && window->DockNode->TabBar) {
		ImGuiTabBar* tabBar = window->DockNode->TabBar;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& tab : tabBar->Tabs) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (tab.ID == window->TabId) {
				ImVec2 tabMin = tabBar->BarRect.Min;
				tabMin.x += tab.Offset - tabBar->ScrollingAnim;
				ImVec2 tabMax = ImVec2(tabMin.x + tab.Width, tabBar->BarRect.Max.y);

				ImVec2 mousePos = ImGui::GetMousePos();
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (mousePos.x >= tabMin.x && mousePos.x <= tabMax.x &&
					mousePos.y >= tabMin.y && mousePos.y <= tabMax.y) {
					tabHovered = true;
				}
				break;
			}
		}
	}

	bool titleBarHovered = false;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!window->DockNode && (window->Flags & ImGuiWindowFlags_NoTitleBar) == 0) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (window->TitleBarRect().Contains(ImGui::GetMousePos())) {
			titleBarHovered = true;
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
		if (titleBarHovered || tabHovered) {
			ImGui::OpenPopup("WindowContextMenu");
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopup("WindowContextMenu")) {
		OnContextMenu();
		if (ImGui::MenuItem("Close") && OnCloseRequested()) m_IsVisible = false;
		ImGui::EndPopup();
	}

	// A close request can run custom side effects such as disabling live
	// diagnostics. Do not call OnGui again on that same frame after visibility
	// changed, otherwise a window can immediately re-enable the state it just
	// disabled.
	if (expanded && m_IsVisible) {
		OnGui();
	}
	ImGui::End();
}
