// ProjectManager.h — Editor project lifecycle management
//
// Defines the primary manager responsible for creating, loading, and tracking the active editor project. it maintains the project root path, handles the persistence of project-specific settings, and manages the global list of recent projects.

#pragma once

#include "ProjectSettings.h"
#include "EditorPreferences.h"
#include <filesystem>
#include <optional>
#include <vector>

enum class ProjectTemplate {
	Blank,
	TwoD,
	ThreeD
};

// class ProjectManager groups related state and operations so callers have one documented contract.
class ProjectManager {
public:
	ProjectManager();
	~ProjectManager();

	// Lifecycle
	bool CreateProject(const std::filesystem::path& location, const std::string& name, ProjectTemplate tmpl, std::string* outError = nullptr);
	bool LoadProject(const std::filesystem::path& location, std::string* outError = nullptr);
	void CloseProject();
	
	// Accessors
	bool HasActiveProject() const;
	const ProjectSettings& GetSettings() const;
	bool SetProjectDisplayName(const std::string& displayName);
	std::filesystem::path GetProjectRoot() const;
	const std::vector<RecentProjectEntry>& GetRecentProjects() const;
	const EditorPreferences& GetEditorPreferences() const;
	std::string GetExternalScriptEditorCommand() const;
	bool ShouldAutoBuildScripts() const;
	void SetExternalScriptEditorCommand(const std::string& commandTemplate);
	void SetAutoBuildScripts(bool enabled);

	// Persist changes
	void SaveProjectSettings();
	void SaveEditorPreferences();

private:
	void LoadEditorPreferences();
	void AddToRecentProjects(const std::filesystem::path& path, const std::string& name);

	std::filesystem::path projectRoot;
	std::optional<ProjectSettings> currentSettings;
	EditorPreferences editorPrefs;
	bool isProjectLoaded = false;
};
