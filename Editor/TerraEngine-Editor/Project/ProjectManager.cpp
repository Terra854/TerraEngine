// ProjectManager.cpp — Editor project lifecycle implementation
//
// Implements the project management logic, including project creation from templates and project loading from disk. It manages the cleanup of legacy interop scripts and ensures that project manifests are correctly synchronized with the active editor state.

#include "ProjectManager.h"
#include "Core/FatalErrorReport.h"
#include <fstream>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include "Engine/Core/Logger/Logger.h"
#include "Resource/ResourceStreaming.h"
#include "Utilities/PlatformUtils.h"
#include "Window/WindowSystem.h"

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

	std::string NormalizePathForRecentComparison(const fs::path& path)
	{
		std::error_code errorCode;
		fs::path normalized = fs::weakly_canonical(path, errorCode);
		if (errorCode)
		{
			normalized = fs::absolute(path, errorCode).lexically_normal();
			if (errorCode)
			{
				normalized = path.lexically_normal();
			}
		}

		std::string value = normalized.make_preferred().string();
#ifdef _WIN32
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
#endif
		while (value.size() > 1 && (value.back() == '\\' || value.back() == '/'))
		{
			value.pop_back();
		}
		return value;
	}

	bool IsSameOrChildPath(const fs::path& candidatePath, const fs::path& rootPath)
	{
		const std::string candidate = NormalizePathForRecentComparison(candidatePath);
		const std::string root = NormalizePathForRecentComparison(rootPath);
		if (candidate.empty() || root.empty())
		{
			return false;
		}
		if (candidate == root)
		{
			return true;
		}

		std::string rootWithSeparator = root;
		if (rootWithSeparator.back() != '\\' && rootWithSeparator.back() != '/')
		{
			rootWithSeparator.push_back(fs::path::preferred_separator);
		}
		return candidate.rfind(rootWithSeparator, 0) == 0;
	}

	bool IsRecentProjectSuppressedByEnvironment()
	{
		if (const char* value = std::getenv("TERRAENGINE_SUPPRESS_PROJECT_RECENTS"))
		{
			std::string text = value;
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return text == "1" || text == "true" || text == "yes" || text == "on";
		}
		return false;
	}

	bool IsTemporaryProjectPath(const fs::path& projectPath)
	{
		if (projectPath.empty())
		{
			return true;
		}

		std::error_code errorCode;
		const fs::path tempRoot = fs::temp_directory_path(errorCode);
		if (!errorCode && !tempRoot.empty() && IsSameOrChildPath(projectPath, tempRoot))
		{
			return true;
		}

		return false;
	}

	bool ShouldTrackRecentProjectPath(const fs::path& projectPath)
	{
		return !IsRecentProjectSuppressedByEnvironment() && !IsTemporaryProjectPath(projectPath);
	}

	bool PruneTransientRecentProjects(std::vector<RecentProjectEntry>& recentProjects)
	{
		const auto originalSize = recentProjects.size();
		recentProjects.erase(
			std::remove_if(
				recentProjects.begin(),
				recentProjects.end(),
				[](const RecentProjectEntry& entry) {
					// Automation and unit tests create disposable projects under the
					// OS temp directory. Those paths are useful for the current test
					// process, but they should never occupy the user's project hub.
					return IsTemporaryProjectPath(fs::path(entry.path));
				}),
			recentProjects.end());
		return recentProjects.size() != originalSize;
	}

	// EnsureProjectManifest is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	nlohmann::json BuildDefaultStreamingPolicyManifest()
	{
		const TerraEngine::Resource::StreamingPolicy policy = TerraEngine::Resource::MakeDefaultStreamingPolicy();
		nlohmann::json object;
		object["enabled"] = policy.enabled;
		object["deferScenePayloadsOnLoad"] = policy.deferScenePayloadsOnLoad;
		object["streamingDistance"] = policy.streamingDistance;
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

	std::vector<std::string> BuildCrashSummaryProductCandidates(const fs::path& projectDir)
	{
		std::vector<std::string> candidates;
		std::set<std::string> uniqueCandidates;
		const auto addCandidate = [&](std::string value) {
			if (!value.empty() && uniqueCandidates.insert(value).second)
			{
				candidates.push_back(std::move(value));
			}
		};

		addCandidate("TerraEngine");
		addCandidate(projectDir.filename().string());

		const fs::path manifestPath = projectDir / "game.json";
		if (fs::exists(manifestPath))
		{
			try
			{
				std::ifstream manifestFile(manifestPath);
				nlohmann::json manifest;
				manifestFile >> manifest;
				if (auto nameIt = manifest.find("name"); nameIt != manifest.end() && nameIt->is_string())
				{
					addCandidate(nameIt->get<std::string>());
				}
			}
			catch (const std::exception& exception)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Could not read game.json while checking previous crash summaries: " << exception.what());
			}
		}
		return candidates;
	}

	std::optional<fs::path> FindLatestUnshownCrashSummary(const std::vector<std::string>& productCandidates)
	{
		std::optional<fs::path> latestSummary;
		fs::file_time_type latestWriteTime = (fs::file_time_type::min)();
		for (const std::string& productName : productCandidates)
		{
			const fs::path crashDirectory = TerraEngine::PlatformUtils::GetCrashDumpDirectory(productName);
			std::error_code directoryError;
			if (crashDirectory.empty() || !fs::exists(crashDirectory, directoryError))
			{
				continue;
			}

			std::error_code iteratorError;
			for (const fs::directory_entry& entry : fs::directory_iterator(crashDirectory, iteratorError))
			{
				std::error_code entryError;
				if (!entry.is_regular_file(entryError) || entry.path().extension() != ".txt")
				{
					continue;
				}
				const std::string filename = entry.path().filename().string();
				if (filename.find(".shown") != std::string::npos)
				{
					continue;
				}

				const fs::file_time_type writeTime = fs::last_write_time(entry.path(), entryError);
				if (!entryError && (!latestSummary || writeTime > latestWriteTime))
				{
					latestSummary = entry.path();
					latestWriteTime = writeTime;
				}
			}
		}
		return latestSummary;
	}

	void MarkCrashSummaryShown(const fs::path& summaryPath)
	{
		const fs::path shownPath = summaryPath.parent_path() / (summaryPath.stem().string() + ".shown" + summaryPath.extension().string());
		std::error_code removeError;
		fs::remove(shownPath, removeError);
		std::error_code renameError;
		fs::rename(summaryPath, shownPath, renameError);
		if (renameError)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Could not mark previous crash summary as shown: " << renameError.message());
		}
	}

	void ShowPreviousHardCrashSummaryIfPresent(const fs::path& projectDir, RendererRecoveryProjectActionState& actionState)
	{
		actionState.previousCrashSummaryShown = false;
		actionState.previousCrashSummaryPath.clear();
		actionState.previousCrashSummaryMessage.clear();

		const std::optional<fs::path> summaryPath = FindLatestUnshownCrashSummary(BuildCrashSummaryProductCandidates(projectDir));
		if (!summaryPath)
		{
			return;
		}

		TerraEngine::FatalErrorDialogReport report;
		std::string error;
		if (!TerraEngine::LoadFatalErrorSummarySidecar(*summaryPath, report, &error))
		{
			TERRAENGINE_LOG(LogLevel::Warning, "[ProjectManager] Previous crash summary could not be loaded: " << error);
			return;
		}

		MarkCrashSummaryShown(*summaryPath);
		report.category = TerraEngine::FatalErrorCategory::PreviousHardCrash;
		report.title = "Previous TerraEngine crash detected";
		report.canContinue = true;
		actionState.previousCrashSummaryShown = WindowSystem::Instance().ShowFatalErrorDialog(report);
		actionState.previousCrashSummaryPath = report.crashSummaryPath;
		actionState.previousCrashSummaryMessage = report.shortReason;
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
		rendererRecoveryActionState.canSaveProjectData = true;
		rendererRecoveryActionState.lastSafeSaveRoot = projectRoot;
		rendererRecoveryActionState.lastSafeSaveMessage.clear();
		ShowPreviousHardCrashSummaryIfPresent(projectDir, rendererRecoveryActionState);

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
		rendererRecoveryActionState.canSaveProjectData = false;
		rendererRecoveryActionState.lastSafeSaveMessage = "No active project is available for renderer-recovery save.";
		rendererRecoveryActionState.previousCrashSummaryShown = false;
		rendererRecoveryActionState.previousCrashSummaryPath.clear();
		rendererRecoveryActionState.previousCrashSummaryMessage.clear();
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

const RendererRecoveryProjectActionState& ProjectManager::GetRendererRecoveryActionState() const {
	return rendererRecoveryActionState;
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
void ProjectManager::SetEditorMcpPreferences(bool enabled, const std::string& writeModeDefault, const std::string& recentSession, std::uint16_t listenPort) {
	editorPrefs.editorMcpEnabled = enabled;
	editorPrefs.editorMcpWriteModeDefault = writeModeDefault;
	editorPrefs.editorMcpRecentSession = recentSession;
	// The bridge validates this value before calling ProjectManager. Keep the
	// persistence layer defensive so no other caller can save an ephemeral or
	// privileged endpoint accidentally.
	if (TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(listenPort)) {
		editorPrefs.editorMcpListenPort = listenPort;
	}
	SaveEditorPreferences();
}

void ProjectManager::SetRendererDiagnosticPreferences(bool vulkanValidationLayers, bool renderDebugDiagnostics) {
	// Vulkan validation and render debug overlays are local editor diagnostics.
	// Store them with user preferences so toggling them does not dirty project
	// configuration or silently affect teammate/export defaults.
	editorPrefs.vulkanValidationLayers = vulkanValidationLayers;
	editorPrefs.renderDebugDiagnostics = renderDebugDiagnostics;
	editorPrefs.rendererDiagnosticsPreferencesInitialized = true;
	SaveEditorPreferences();
}

void ProjectManager::SetSceneViewOverlayPreference(const TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference& preference) {
	// Scene View overlay options are editor preferences, not scene data, so
	// changing gizmo/icon visibility never dirties the active project or export.
	editorPrefs.sceneViewOverlay = TerraEngine::Editor::SceneViewOverlay::SanitizePreference(preference);
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

bool ProjectManager::SaveProjectDataForRendererRecovery(std::string* outError) {
	// Device-loss recovery may run while Vulkan objects are invalid, so this
	// path intentionally writes only CPU-owned project/editor metadata through
	// the existing save functions. Scene documents, imports, cooks, and asset
	// database rewrites remain outside this emergency save to avoid hidden work.
	if (!isProjectLoaded || !currentSettings.has_value() || projectRoot.empty()) {
		const std::string message = "No active project data is available for renderer recovery.";
		rendererRecoveryActionState.canSaveProjectData = false;
		rendererRecoveryActionState.lastSafeSaveMessage = message;
		if (outError != nullptr) {
			*outError = message;
		}
		return false;
	}

	try {
		SaveProjectSettings();
		SaveEditorPreferences();
		rendererRecoveryActionState.canSaveProjectData = true;
		rendererRecoveryActionState.lastSafeSaveRoot = projectRoot;
		rendererRecoveryActionState.lastSafeSaveMessage = "Project settings and editor preferences saved without renderer state.";
		return true;
	} catch (const std::exception& exception) {
		const std::string message = std::string("Renderer-recovery project save failed: ") + exception.what();
		rendererRecoveryActionState.lastSafeSaveMessage = message;
		if (outError != nullptr) {
			*outError = message;
		}
		return false;
	}
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
			const bool prunedTransientRecents = PruneTransientRecentProjects(editorPrefs.recentProjects);
			if (pathToLoad != preferencesPath) {
				SaveEditorPreferences();
			}
			else if (prunedTransientRecents)
			{
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

	// Remove existing entries first so a disposable automation project that was
	// saved by an older editor build is cleaned up as soon as it is opened again.
	list.erase(std::remove_if(list.begin(), list.end(), [&](const RecentProjectEntry& e) {
		return e.path == pathStr;
	}), list.end());

	if (!ShouldTrackRecentProjectPath(path))
	{
		SaveEditorPreferences();
		return;
	}

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
