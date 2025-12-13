#include "ProjectManager.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace fs = std::filesystem;

ProjectManager::ProjectManager() {
    LoadEditorPreferences();
}

ProjectManager::~ProjectManager() {
    SaveEditorPreferences();
}

bool ProjectManager::CreateProject(const fs::path& location, const std::string& name, ProjectTemplate tmpl, std::string* outError) {
    if (fs::exists(location) && !fs::is_empty(location)) {
        std::string msg = "Project location must be empty or new: " + location.string();
        std::cerr << msg << std::endl;
        if (outError) *outError = msg;
        return false;
    }

    try {
        fs::create_directories(location);
        fs::create_directories(location / "Assets");
        fs::create_directories(location / "Library");
        fs::create_directories(location / "Builds");
        fs::create_directories(location / "Temp");

        ProjectSettings settings;
        settings.projectName = name;
        // Template logic could go here to adjust settings or copy starter assets

        currentSettings = settings;
        projectRoot = location;
        SaveProjectSettings();

        // Load it immediately
        return LoadProject(location, outError);
    } catch (const std::exception& e) {
        std::string msg = std::string("Failed to create project: ") + e.what();
        std::cerr << msg << std::endl;
        if (outError) *outError = msg;
        return false;
    }
}

bool ProjectManager::LoadProject(const fs::path& location, std::string* outError) {
    fs::path projectDir = location;
    if (location.filename() == "ProjectSettings.terra") {
        projectDir = location.parent_path();
    }

    fs::path settingsPath = projectDir / "ProjectSettings.terra";
    std::cout << "[ProjectManager] Checking for settings at: " << fs::absolute(settingsPath).string() << std::endl;

    if (!fs::exists(settingsPath)) {
        std::string msg = "Not a valid project folder (missing ProjectSettings.terra): " + fs::absolute(settingsPath).string();
        std::cerr << msg << std::endl;
        if (outError) *outError = msg;
        return false;
    }

    try {
        std::ifstream file(settingsPath);
        nlohmann::json j;
        file >> j;
        currentSettings = j.get<ProjectSettings>();
        projectRoot = projectDir;
        isProjectLoaded = true;

        AddToRecentProjects(projectDir, currentSettings->projectName);
        return true;
    } catch (const std::exception& e) {
        std::string msg = std::string("Failed to load project settings: ") + e.what();
        std::cerr << msg << std::endl;
        if (outError) *outError = msg;
        return false;
    }
}

void ProjectManager::CloseProject() {
    if (isProjectLoaded) {
        SaveProjectSettings();
        isProjectLoaded = false;
        currentSettings.reset();
        projectRoot.clear();
    }
}

bool ProjectManager::HasActiveProject() const {
    return isProjectLoaded;
}

const ProjectSettings& ProjectManager::GetSettings() const {
    static ProjectSettings empty;
    return currentSettings.has_value() ? *currentSettings : empty;
}

std::filesystem::path ProjectManager::GetProjectRoot() const {
    return projectRoot;
}

const std::vector<RecentProjectEntry>& ProjectManager::GetRecentProjects() const {
    return editorPrefs.recentProjects;
}

void ProjectManager::SaveProjectSettings() {
    if (!currentSettings.has_value() || projectRoot.empty()) return;

    fs::path settingsPath = projectRoot / "ProjectSettings.terra";
    std::ofstream file(settingsPath);
    nlohmann::json j = *currentSettings;
    file << j.dump(4);
}

void ProjectManager::SaveEditorPreferences() {
    // Save to user directory (e.g. AppData or .config)
    // For now, let's save to a local file near the executable or in a generic temp spot for this CLI environment
    // In a real app, use generic platform paths.
    // For this context, we'll save to ".terra_editor_prefs.json" in the current working directory for simplicity
    std::ofstream file(".terra_editor_prefs.json");
    nlohmann::json j = editorPrefs;
    file << j.dump(4);
}

void ProjectManager::LoadEditorPreferences() {
    if (fs::exists(".terra_editor_prefs.json")) {
        try {
            std::ifstream file(".terra_editor_prefs.json");
            nlohmann::json j;
            file >> j;
            editorPrefs = j.get<EditorPreferences>();
        } catch (...) {
            // ignore corrupt prefs
        }
    }
}

void ProjectManager::AddToRecentProjects(const fs::path& path, const std::string& name) {
    auto& list = editorPrefs.recentProjects;
    std::string pathStr = fs::absolute(path).string();

    // Remove existing
    list.erase(std::remove_if(list.begin(), list.end(), [&](const RecentProjectEntry& e) {
        return e.path == pathStr;
    }), list.end());

    // Add to front
    RecentProjectEntry entry;
    entry.name = name;
    entry.path = pathStr;
    entry.lastOpened = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    list.insert(list.begin(), entry);

    // Trim
    if (list.size() > 10) {
        list.resize(10);
    }
    SaveEditorPreferences();
}