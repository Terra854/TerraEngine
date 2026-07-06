// ProjectManager.cpp — Editor project lifecycle implementation
//
// Implements the project management logic, including project creation from templates and project loading from disk. It manages the cleanup of legacy interop scripts and ensures that project manifests are correctly synchronized with the active editor state.

#include "ProjectManager.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "Engine/Core/Logger/Logger.h"
#include "Resource/ResourceStreaming.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
	constexpr std::string_view kDefaultProjectVersion = "1.0.0";
	constexpr std::string_view kEditorPreferencesFileName = ".terra_editor_prefs.json";

	fs::path ResolveEditorPreferencesPath()
	{
		// Tests and developer tools can pin the preferences file explicitly, while
		// normal editor runs use a user-level path so settings do not follow the
		// active project or the process launch directory.
		if (const char* overridePath = std::getenv("TERRAENGINE_EDITOR_PREFS_PATH"))
		{
			if (overridePath[0] != '\0')
			{
				return fs::path(overridePath);
			}
		}

#ifdef _WIN32
		if (const char* appData = std::getenv("APPDATA"))
		{
			if (appData[0] != '\0')
			{
				return fs::path(appData) / "TerraEngine" / "Editor" / kEditorPreferencesFileName;
			}
		}
		if (const char* localAppData = std::getenv("LOCALAPPDATA"))
		{
			if (localAppData[0] != '\0')
			{
				return fs::path(localAppData) / "TerraEngine" / "Editor" / kEditorPreferencesFileName;
			}
		}
#else
		if (const char* xdgConfigHome = std::getenv("XDG_CONFIG_HOME"))
		{
			if (xdgConfigHome[0] != '\0')
			{
				return fs::path(xdgConfigHome) / "TerraEngine" / "Editor" / kEditorPreferencesFileName;
			}
		}
		if (const char* home = std::getenv("HOME"))
		{
			if (home[0] != '\0')
			{
				return fs::path(home) / ".config" / "TerraEngine" / "Editor" / kEditorPreferencesFileName;
			}
		}
#endif

		return fs::current_path() / kEditorPreferencesFileName;
	}

	fs::path ResolveLegacyEditorPreferencesPath()
	{
		return fs::current_path() / kEditorPreferencesFileName;
	}

	// EnsureProjectManifest is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	nlohmann::json BuildDefaultStreamingPolicyManifest()
	{
		const TerraEngine::Resource::StreamingPolicy policy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["deferScenePayloadsOnLoad"] = policy.deferScenePayloadsOnLoad;
		object["preloadDistance"] = policy.preloadDistance;
		object["visibleRetentionSeconds"] = policy.visibleRetentionSeconds;
		object["neededRetentionSeconds"] = policy.neededRetentionSeconds;
		object["maxLoadsPerUpdate"] = policy.maxLoadsPerUpdate;
		// New projects should make the texture residency behavior explicit in
		// game.json so editor previews and exported runtime builds start from
		// the same mip-tail streaming defaults.
		object["textureMipStreamingEnabled"] = policy.textureMipStreamingEnabled;
		object["textureMinStreamMip"] = policy.textureMinStreamMip;
		object["textureMaxStreamMip"] = policy.textureMaxStreamMip;
		object["virtualGeometryPageMemoryBudgetBytes"] = policy.virtualGeometryPageMemoryBudgetBytes;
		object["virtualGeometryPageUploadBudgetBytes"] = policy.virtualGeometryPageUploadBudgetBytes;
		object["virtualGeometryPageRetirementBudgetBytes"] = policy.virtualGeometryPageRetirementBudgetBytes;
		object["memoryBudgetInteraction"] = std::string(TerraEngine::Resource::ToString(policy.memoryBudgetInteraction));
		object["diagnosticsEnabled"] = policy.diagnosticsEnabled;
		return object;
	}

	bool EnsureProjectManifest(const fs::path& projectRoot, std::string_view requestedName, std::string* outError = nullptr)
	{
		const fs::path manifestPath = projectRoot / "game.json";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fs::exists(manifestPath))
		{
			return true;
		}

		nlohmann::json manifest;
		manifest["name"] = requestedName.empty() ? projectRoot.filename().string() : std::string(requestedName);
		manifest["version"] = kDefaultProjectVersion;
		manifest["streamingPolicy"] = BuildDefaultStreamingPolicyManifest();

		std::ofstream manifestFile(manifestPath);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!manifestFile)
		{
			const std::string message = "Failed to create project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		manifestFile << manifest.dump(4);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!manifestFile.good())
		{
			const std::string message = "Failed to write project manifest: " + manifestPath.string();
			TERRAENGINE_LOG(LogLevel::Error, message);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (outError != nullptr)
			{
				*outError = message;
			}
			return false;
		}

		TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Created default project manifest at " << manifestPath.string());
		return true;
	}

	bool IsBlankDisplayName(const std::string& value)
	{
		return std::all_of(value.begin(), value.end(), [](unsigned char character) {
			return std::isspace(character) != 0;
		});
	}
}

ProjectManager::ProjectManager() {
	LoadEditorPreferences();
}

ProjectManager::~ProjectManager() {
	CloseProject();
	SaveEditorPreferences();
}

// ProjectManager::CreateProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProjectManager::CreateProject(const fs::path& location, const std::string& name, ProjectTemplate tmpl, std::string* outError) {
	if (fs::exists(location) && !fs::is_empty(location)) {
		std::string msg = "Project location must be empty or new: " + location.string();
		TERRAENGINE_LOG(LogLevel::Error, msg);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		"indirectStrength": 1.0,
		"ambientOcclusionStrength": 1.0,
		"ambientOcclusionQuality": 2
	},
	"objects": [
		{
			"name": "Sky",
			"components": {
				"SkyComponent": {
					"sourceAsset": "",
					"exposure": 0.7,
					"rotationOffsetDegrees": 0.0,
					"horizonTint": [0.45, 0.58, 0.72],
					"zenithTint": [0.08, 0.16, 0.36],
					"groundTint": [0.06, 0.07, 0.09]
				},
				"LightComponent": {
					"type": 0,
					"unit": 1,
					"color": [1.0, 1.0, 1.0],
					"intensity": 2.0,
					"castsShadows": false
				}
			},
			"children": []
		},
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!EnsureProjectManifest(location, name, outError)) {
			return false;
		}

		// Load it immediately
		return LoadProject(location, outError);
	} catch (const std::exception& e) {
		std::string msg = std::string("Failed to create project: ") + e.what();
		TERRAENGINE_LOG(LogLevel::Error, msg);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError) *outError = msg;
		return false;
	}
}

// ProjectManager::LoadProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProjectManager::LoadProject(const fs::path& location, std::string* outError) {
	// The user must select the project folder itself, not the settings file inside it.
	fs::path projectDir = location;
	fs::path settingsPath = projectDir / "ProjectSettings" / "ProjectSettings.terra";
	// Trace project discovery through the shared logger for consistent editor diagnostics.
	TERRAENGINE_LOG(LogLevel::Info, "[ProjectManager] Checking for settings at: " << fs::absolute(settingsPath).string());

	if (!fs::exists(settingsPath)) {
		std::string msg = "Not a valid project folder (missing ProjectSettings/ProjectSettings.terra): " + fs::absolute(settingsPath).string();
		TERRAENGINE_LOG(LogLevel::Error, msg);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError) *outError = msg;
		return false;
	}
}

// ProjectManager::CloseProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::CloseProject() {
	if (isProjectLoaded) {
		SaveProjectSettings();
		isProjectLoaded = false;
		currentSettings.reset();
		projectRoot.clear();
	}
}

// ProjectManager::HasActiveProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProjectManager::HasActiveProject() const {
	return isProjectLoaded;
}

// ProjectManager::GetSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const ProjectSettings& ProjectManager::GetSettings() const {
	static ProjectSettings empty;
	return currentSettings.has_value() ? *currentSettings : empty;
}

// ProjectManager::SetProjectDisplayName validates and persists the editable
// project label used by Project Settings without exposing mutable settings
// storage to every editor window.
bool ProjectManager::SetProjectDisplayName(const std::string& displayName) {
	if (!currentSettings.has_value() || IsBlankDisplayName(displayName)) {
		return false;
	}

	currentSettings->projectDisplayName = displayName;
	SaveProjectSettings();
	return true;
}

// ProjectManager::GetProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ProjectManager::GetProjectRoot() const {
	return projectRoot;
}

// ProjectManager::GetRecentProjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const std::vector<RecentProjectEntry>& ProjectManager::GetRecentProjects() const {
	return editorPrefs.recentProjects;
}

// ProjectManager::GetEditorPreferences is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const EditorPreferences& ProjectManager::GetEditorPreferences() const {
	return editorPrefs;
}

// ProjectManager::GetExternalScriptEditorCommand is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ProjectManager::GetExternalScriptEditorCommand() const {
	return editorPrefs.externalScriptEditorCommand;
}

// ProjectManager::ShouldAutoBuildScripts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ProjectManager::ShouldAutoBuildScripts() const {
	return editorPrefs.autoBuildScripts;
}

// ProjectManager::SetExternalScriptEditorCommand is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::SetExternalScriptEditorCommand(const std::string& commandTemplate) {
	editorPrefs.externalScriptEditorCommand = commandTemplate;
	SaveEditorPreferences();
}

// ProjectManager::SetAutoBuildScripts is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::SetAutoBuildScripts(bool enabled) {
	editorPrefs.autoBuildScripts = enabled;
	SaveEditorPreferences();
}

// ProjectManager::SetEditorMcpPreferences stores the bridge defaults that the editor reapplies during startup.
void ProjectManager::SetEditorMcpPreferences(bool enabled, const std::string& writeModeDefault, const std::string& recentSession) {
	editorPrefs.editorMcpEnabled = enabled;
	editorPrefs.editorMcpWriteModeDefault = writeModeDefault;
	editorPrefs.editorMcpRecentSession = recentSession;
	SaveEditorPreferences();
}

// ProjectManager::SaveProjectSettings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::SaveProjectSettings() {
	if (!currentSettings.has_value() || projectRoot.empty()) return;

	fs::path settingsPath = projectRoot / "ProjectSettings" / "ProjectSettings.terra";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(settingsPath.parent_path())) {
		fs::create_directories(settingsPath.parent_path());
	}

	std::ofstream file(settingsPath);
	nlohmann::json j = *currentSettings;
	file << j.dump(4);
}

// ProjectManager::SaveEditorPreferences is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::SaveEditorPreferences() {
	const fs::path preferencesPath = ResolveEditorPreferencesPath();
	if (!preferencesPath.parent_path().empty()) {
		fs::create_directories(preferencesPath.parent_path());
	}
	std::ofstream file(preferencesPath);
	nlohmann::json j = editorPrefs;
	file << j.dump(4);
}

// ProjectManager::LoadEditorPreferences is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ProjectManager::LoadEditorPreferences() {
	const fs::path preferencesPath = ResolveEditorPreferencesPath();
	const fs::path legacyPreferencesPath = ResolveLegacyEditorPreferencesPath();
	fs::path pathToLoad;
	if (fs::exists(preferencesPath)) {
		pathToLoad = preferencesPath;
	} else if (fs::exists(legacyPreferencesPath)) {
		pathToLoad = legacyPreferencesPath;
	}

	if (!pathToLoad.empty()) {
		try {
			std::ifstream file(pathToLoad);
			nlohmann::json j;
			file >> j;
			editorPrefs = j.get<EditorPreferences>();
			if (pathToLoad != preferencesPath) {
				SaveEditorPreferences();
			}
		} catch (...) {
			// ignore corrupt prefs
		}
	}
}

// ProjectManager::AddToRecentProjects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (list.size() > 10) {
		list.resize(10);
	}
	SaveEditorPreferences();
}
