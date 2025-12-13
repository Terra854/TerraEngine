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
    std::filesystem::path GetProjectRoot() const;
    const std::vector<RecentProjectEntry>& GetRecentProjects() const;

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
