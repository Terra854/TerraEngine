// EditorWindow.h — Native editor window base class
//
// Defines the base class for all native ImGui-based windows in the editor. It manages common window state such as visibility and title, and provides the virtual hooks for rendering GUI content and handling context menus.

#pragma once
#include <string>
#include <imgui.h>

class EditorWindow {
public:
	EditorWindow(const std::string& title, bool startVisible = true, ImGuiWindowFlags flags = 0);
	virtual ~EditorWindow() = default;

	void Draw();

	// IsVisible is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsVisible() const { return m_IsVisible; }
	void SetVisible(bool visible) { m_IsVisible = visible; }
	const std::string& GetTitle() const { return m_Title; }

protected:
	virtual void OnGui() = 0;
	// OnContextMenu is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual void OnContextMenu() {}
	// OnCloseRequested is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	virtual bool OnCloseRequested() { return true; }

	std::string m_Title;
	bool m_IsVisible;
	ImGuiWindowFlags m_Flags;
};
