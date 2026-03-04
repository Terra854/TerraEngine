#include "ProjectManager.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include "Engine/Core/Logger/Logger.h"

namespace fs = std::filesystem;

ProjectManager::ProjectManager() {
    LoadEditorPreferences();
}

ProjectManager::~ProjectManager() {
    CloseProject();
    SaveEditorPreferences();
}

bool ProjectManager::CreateProject(const fs::path& location, const std::string& name, ProjectTemplate tmpl, std::string* outError) {
    if (fs::exists(location) && !fs::is_empty(location)) {
        std::string msg = "Project location must be empty or new: " + location.string();
        TERRAENGINE_LOG(LogLevel::Error, msg);
        if (outError) *outError = msg;
        return false;
    }

    try {
        fs::create_directories(location);
        fs::create_directories(location / "Assets");
        fs::create_directories(location / "Assets" / "Scenes");
        fs::create_directories(location / "Library");
        fs::create_directories(location / "Build");
        fs::create_directories(location / "Temp");
        fs::create_directories(location / "ProjectSettings");
        fs::create_directories(location / "UserSettings");

        // Create Default Scene
        {
            fs::path scenePath = location / "Assets" / "Scenes" / "Scene.tescene";
            std::ofstream sceneFile(scenePath);
            sceneFile << R"({
    "objects": [
        {
            "name": "Main Camera",
            "components": {
                "Transform": {
                    "Position": [0.0, 1.0, -10.0],
                    "Rotation": [1.0, 0.0, 0.0, 0.0],
                    "Scale": [1.0, 1.0, 1.0]
                },
                "CameraComponent": {
                    "verticalFovDegrees": 45.0,
                    "aspectRatio": 1.777777,
                    "nearPlane": 0.1,
                    "farPlane": 100.0,
                    "up": [0.0, 1.0, 0.0]
                }
            },
            "children": []
        }
    ]
})";
        }

        ProjectSettings settings;
        // settings.projectName = name; // Field removed

        currentSettings = settings;
        projectRoot = location;
        SaveProjectSettings();

        // Load it immediately
        return LoadProject(location, outError);
    } catch (const std::exception& e) {
        std::string msg = std::string("Failed to create project: ") + e.what();
        TERRAENGINE_LOG(LogLevel::Error, msg);
        if (outError) *outError = msg;
        return false;
    }
}

bool ProjectManager::LoadProject(const fs::path& location, std::string* outError) {
    // The user must select the project folder itself, not the settings file inside it.
    fs::path projectDir = location;
    fs::path settingsPath = projectDir / "ProjectSettings" / "ProjectSettings.terra";
    // Trace project discovery through the shared logger for consistent editor diagnostics.
    TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Checking for settings at: " << fs::absolute(settingsPath).string());

    if (!fs::exists(settingsPath)) {
        std::string msg = "Not a valid project folder (missing ProjectSettings/ProjectSettings.terra): " + fs::absolute(settingsPath).string();
        TERRAENGINE_LOG(LogLevel::Error, msg);
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

        // Derive name from directory
        std::string projectName = projectDir.filename().string();
        AddToRecentProjects(projectDir, projectName);
        return true;
    } catch (const std::exception& e) {
        std::string msg = std::string("Failed to load project settings: ") + e.what();
        TERRAENGINE_LOG(LogLevel::Error, msg);
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

    fs::path settingsPath = projectRoot / "ProjectSettings" / "ProjectSettings.terra";
    if (!fs::exists(settingsPath.parent_path())) {
        fs::create_directories(settingsPath.parent_path());
    }

    std::ofstream file(settingsPath);
    nlohmann::json j = *currentSettings;
    file << j.dump(4);
}

void ProjectManager::SaveEditorPreferences() {
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
