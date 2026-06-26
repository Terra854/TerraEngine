// ProjectBrowserWindow.h — Project Browser Window Definition
//
// Defines the ProjectBrowserWindow class, providing a filesystem-like interface for managing project assets.
// It supports file navigation, asset importing, and organization within the project's directory structure.

#pragma once
#include "../Framework/EditorWindow.h"
#include <array>
#include <string>
#include <filesystem>
#include <unordered_set>

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
	void OpenCreateAssetModal(const std::string& title, const std::string& defaultName, const std::string& extension);
	void DrawCreateAssetModal();
	void CreateNamedAsset();

	MainEditorSubsystem* m_Owner;
	std::filesystem::path m_CurrentDirectory;
	std::string m_SearchFilter;
	std::unordered_set<std::string> m_CollapsedPackageGroups;
	std::string m_SelectedPackageChildGuid;
	float m_ThumbnailSize = 64.0f;
	float m_Padding = 16.0f;

	std::filesystem::path m_AssetToDelete;
	bool m_ShowDeleteModal = false;
	bool m_ShowCreateAssetModal = false;
	bool m_ShowCreateScriptModal = false;
	std::string m_CreateAssetTitle;
	std::string m_CreateAssetExtension;
	std::array<char, 128> m_NewAssetName{};
	std::array<char, 128> m_NewScriptName{};
	std::string m_CreateAssetError;
	std::string m_CreateScriptError;
};
