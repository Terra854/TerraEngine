// EditorWindow.h — Native editor window base class
//
// Defines the base class for all native ImGui-based windows in the editor. It manages common window state such as visibility and title, and provides the virtual hooks for rendering GUI content and handling context menus.

#pragma once
#include <string>
#include <imgui.h>

class EditorWindow {
public:
	EditorWindow(const std::string& title, bool startVisible = true, ImGuiWindowFlags flags = 0);
	EditorWindow(const std::string& title, bool startVisible, std::string windowMenuCategoryPath, ImGuiWindowFlags flags = 0);
	virtual ~EditorWindow() = default;

	void Draw();


	bool IsVisible() const { return m_IsVisible; }
	void SetVisible(bool visible) { m_IsVisible = visible; }
	const std::string& GetTitle() const { return m_Title; }
	// WindowMenuCategoryPath is an explicit opt-in location relative to the
	// Windows menu. Slash-separated segments create nested subcategories, and
	// the window title becomes the leaf item. An empty path omits the window.
	const std::string& GetWindowMenuCategoryPath() const { return m_WindowMenuCategoryPath; }

protected:
	// Derived windows may publish first-frame placement constraints before ImGui
	// begins the window. The default keeps existing docking/layout behavior.
	virtual void OnBeforeBegin() {}
	virtual void OnGui() = 0;

	virtual void OnContextMenu() {}

	virtual bool OnCloseRequested() { return true; }

	std::string m_Title;
	std::string m_WindowMenuCategoryPath;
	bool m_IsVisible;
	ImGuiWindowFlags m_Flags;
};
