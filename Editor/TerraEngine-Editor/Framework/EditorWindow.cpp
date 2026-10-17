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


void EditorWindow::Draw() {
	if (!m_IsVisible) return;

	// ImGui placement constraints must be submitted before Begin. Keeping this
	// hook in the common wrapper lets transient windows provide safe first-use
	// dimensions without reimplementing visibility and close handling.
	OnBeforeBegin();
	bool nextVisible = m_IsVisible;
	bool expanded = ImGui::Begin(m_Title.c_str(), &nextVisible, m_Flags);

	if (!nextVisible && m_IsVisible) {

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

		for (const auto& tab : tabBar->Tabs) {

			if (tab.ID == window->TabId) {
				ImVec2 tabMin = tabBar->BarRect.Min;
				tabMin.x += tab.Offset - tabBar->ScrollingAnim;
				ImVec2 tabMax = ImVec2(tabMin.x + tab.Width, tabBar->BarRect.Max.y);

				ImVec2 mousePos = ImGui::GetMousePos();

				if (mousePos.x >= tabMin.x && mousePos.x <= tabMax.x &&
					mousePos.y >= tabMin.y && mousePos.y <= tabMax.y) {
					tabHovered = true;
				}
				break;
			}
		}
	}

	bool titleBarHovered = false;

	if (!window->DockNode && (window->Flags & ImGuiWindowFlags_NoTitleBar) == 0) {

		if (window->TitleBarRect().Contains(ImGui::GetMousePos())) {
			titleBarHovered = true;
		}
	}


	if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
		if (titleBarHovered || tabHovered) {
			ImGui::OpenPopup("WindowContextMenu");
		}
	}


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
