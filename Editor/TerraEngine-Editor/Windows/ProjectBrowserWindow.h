// ProjectBrowserWindow.h — Project Browser Window Definition
//
// Defines the ProjectBrowserWindow class, providing a filesystem-like interface for managing project assets.
// It supports file navigation, asset importing, and organization within the project's directory structure.

#pragma once
#include "../Framework/EditorWindow.h"
#include <array>
#include <string>
#include <filesystem>

class MainEditorSubsystem;

// class ProjectBrowserWindow groups related state and operations so callers have one documented contract.
class ProjectBrowserWindow : public EditorWindow {
public:
	ProjectBrowserWindow(MainEditorSubsystem* owner);
	void Reset();

protected:
	virtual void OnGui() override;

private:
	void DrawDirectoryTree(const std::filesystem::path& path);
	void DrawDirectoryContent(const std::filesystem::path& path);

	MainEditorSubsystem* m_Owner;
	std::filesystem::path m_CurrentDirectory;
	std::string m_SearchFilter;
	float m_ThumbnailSize = 64.0f;
	float m_Padding = 16.0f;

	std::filesystem::path m_AssetToDelete;
	bool m_ShowDeleteModal = false;
	bool m_ShowCreateScriptModal = false;
	std::array<char, 128> m_NewScriptName{};
	std::string m_CreateScriptError;
};
