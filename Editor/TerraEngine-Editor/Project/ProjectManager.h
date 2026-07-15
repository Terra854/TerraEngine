// ProjectManager.h — Editor project lifecycle management
//
// Defines the primary manager responsible for creating, loading, and tracking the active editor project. it maintains the project root path, handles the persistence of project-specific settings, and manages the global list of recent projects.

#pragma once

#include "ProjectSettings.h"
#include "EditorPreferences.h"
#include "Scene/Serializer.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ProjectTemplate {
	Blank,
	TwoD,
	ThreeD
};

// RendererRecoveryProjectActionState is CPU-only editor state used by explicit
// automation requests. The terminal device-loss dialog does not expose this as
// a user action because acknowledging that dialog immediately ends the process.
struct RendererRecoveryProjectActionState
{
	bool canSaveProjectData = false;
	std::filesystem::path lastSafeSaveRoot;
	std::string lastSafeSaveMessage;
};

// ProjectSceneLoadDecision describes how one authoritative scene became a
// worker-prepared publication packet. The parsed document remains value-only;
// ObjectManager is not touched until MainEditorSubsystem reaches its protected
// scene activation boundary.
struct ProjectSceneLoadDecision
{
	bool prepared = false;
	bool cacheHit = false;
	bool usedAuthoritativeSource = false;
	bool regenerationPublished = false;
	bool cancelled = false;
	std::uint64_t projectGeneration = 0U;
	std::filesystem::path sourcePath;
	std::filesystem::path derivedPath;
	std::string cacheState;
	std::string reason;
	nlohmann::json sceneDocument;
	TerraEngine::Scene::PreparedSceneDecode preparedScene;
};

// ProjectReadinessState separates gameplay-critical publication from editor
// metadata refresh. Callers may enable simulation only after the complete
// prepared scene is visible; editor-only panels can refresh afterward.
struct ProjectReadinessState
{
	std::uint64_t projectGeneration = 0U;
	bool projectSettingsReady = false;
	bool simulationEssentialReady = false;
	bool editorMetadataDeferred = false;
	bool editorMetadataReady = false;
	std::string sceneReadinessSource;
	std::string sceneReadinessReason;
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
	bool PrepareSceneForLoading(const std::filesystem::path& scenePath, ProjectSceneLoadDecision& outDecision, std::string* outError = nullptr);
	std::uint64_t GetProjectLoadGeneration() const;
	bool IsProjectLoadGenerationCurrent(std::uint64_t generation) const;
	ProjectSceneLoadDecision GetLastSceneLoadDecision() const;
	ProjectReadinessState GetReadinessState() const;
	void MarkSimulationEssentialReady(const ProjectSceneLoadDecision& decision);
	void MarkEditorMetadataReady();
	
	// Accessors
	bool HasActiveProject() const;
	const ProjectSettings& GetSettings() const;
	bool SetProjectDisplayName(const std::string& displayName);
	bool ReadAutomationSetting(bool projectScope, std::string_view key, nlohmann::json& value, std::string& error) const;
	bool WriteAutomationSetting(bool projectScope, std::string_view key, const nlohmann::json& value, std::string& error);
	std::filesystem::path GetProjectRoot() const;
	const std::vector<RecentProjectEntry>& GetRecentProjects() const;
	const EditorPreferences& GetEditorPreferences() const;
	const RendererRecoveryProjectActionState& GetRendererRecoveryActionState() const;
	std::string GetExternalScriptEditorCommand() const;
	bool ShouldAutoBuildScripts() const;
	void SetExternalScriptEditorCommand(const std::string& commandTemplate);
	void SetAutoBuildScripts(bool enabled);
	void SetEditorMcpPreferences(bool enabled, const std::string& writeModeDefault, const std::string& recentSession, std::uint16_t listenPort);
	void SetRendererDiagnosticPreferences(bool vulkanValidationLayers, bool renderDebugDiagnostics);
	bool SetSceneViewOverlayPreference(const TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference& preference, std::string* outError = nullptr);

	// Persist changes
	void SaveProjectSettings();
	bool SaveEditorPreferences(std::string* outError = nullptr);
	bool SaveProjectDataForRendererRecovery(std::string* outError = nullptr);

private:
	void LoadEditorPreferences();
	void AddToRecentProjects(const std::filesystem::path& path, const std::string& name);

	std::filesystem::path projectRoot;
	std::optional<ProjectSettings> currentSettings;
	EditorPreferences editorPrefs;
	RendererRecoveryProjectActionState rendererRecoveryActionState;
	// Generation and closing are atomic because worker decode/regeneration jobs
	// sample them without taking ownership of editor lifecycle state.
	std::atomic_uint64_t projectLoadGeneration{1U};
	std::atomic_bool projectClosing{true};
	mutable std::mutex readinessMutex;
	ProjectSceneLoadDecision lastSceneLoadDecision;
	ProjectReadinessState readinessState;
	bool isProjectLoaded = false;
};
