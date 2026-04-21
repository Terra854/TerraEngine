#include "ProjectManager.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include "Engine/Core/Logger/Logger.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr std::string_view kDefaultProjectVersion = "1.0.0";

	bool EnsureProjectManifest(const fs::path& projectRoot, std::string_view requestedName, std::string* outError = nullptr)
	{
		const fs::path manifestPath = projectRoot / "game.json";
		if (fs::exists(manifestPath))
		{
			return true;
		}

		nlohmann::json manifest;
		manifest["name"] = requestedName.empty() ? projectRoot.filename().string() : std::string(requestedName);
		manifest["version"] = kDefaultProjectVersion;

		std::ofstream manifestFile(manifestPath);
		if (!manifestFile)
		{
			const std::string message = "Failed to create project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		manifestFile << manifest.dump(4);
		if (!manifestFile.good())
		{
			const std::string message = "Failed to write project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Created default project manifest at " << manifestPath.string());
		return true;
	}
}

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
	"name": "Scene",
	"sceneSettings": {
		"sky": {
			"enabled": true,
			"sourceAsset": "",
			"intensity": 1.0,
			"exposure": 1.0,
			"rotationOffsetDegrees": 0.0,
			"horizonTint": [0.92, 0.96, 1.0],
			"zenithTint": [0.20, 0.38, 0.80],
			"groundTint": [0.10, 0.12, 0.18],
			"directionalLightObjectId": "00000000000000000000000000000000"
		}
	},
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
		if (!EnsureProjectManifest(location, name, outError)) {
			return false;
		}

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
		if (!EnsureProjectManifest(projectDir, projectName)) {
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Continuing without a freshly written game.json for project " << projectDir.string());
		}
		AddToRecentProjects(projectDir, projectName);

		// CLEAN OBSOLETE INTEROP SCRIPTS:
		// The export interop layer (ExportBootstrap, ExportVTable, etc.) is now
		// provided entirely by the engine DLL and the Roslyn Source Generator.
		// We forcefully delete any legacy script files that might exist in the
		// project to ensure they do not cause "CS0436: Type conflict" errors.
		try {
			fs::path obsoleteDir = projectDir / "Project" / "Internal" / "Export";
			if (fs::exists(obsoleteDir)) {
				for (const auto& entry : fs::recursive_directory_iterator(obsoleteDir)) {
					fs::permissions(entry, fs::perms::all);
				}
				fs::remove_all(obsoleteDir);
				TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Deleted obsolete interop folder: " << obsoleteDir.string());
			}
		} catch (const std::exception& e) {
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Failed to delete obsolete interop folder: " << e.what());
		}

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

const EditorPreferences& ProjectManager::GetEditorPreferences() const {
	return editorPrefs;
}

std::string ProjectManager::GetExternalScriptEditorCommand() const {
	return editorPrefs.externalScriptEditorCommand;
}

bool ProjectManager::ShouldAutoBuildScripts() const {
	return editorPrefs.autoBuildScripts;
}

void ProjectManager::SetExternalScriptEditorCommand(const std::string& commandTemplate) {
	editorPrefs.externalScriptEditorCommand = commandTemplate;
	SaveEditorPreferences();
}

void ProjectManager::SetAutoBuildScripts(bool enabled) {
	editorPrefs.autoBuildScripts = enabled;
	SaveEditorPreferences();
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
