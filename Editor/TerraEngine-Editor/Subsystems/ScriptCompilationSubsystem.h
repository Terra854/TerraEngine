#pragma once

#include "IEditorSubsystem.h"
#include "../Project/ScriptWorkspaceService.h"

#include <filesystem>
#include <future>
#include <string>

enum class ScriptBuildProfile
{
	Release,
	Debug
};

enum class ScriptCoordinatorState
{
	Idle,
	Queued,
	Building,
	Reloading,
	Ready,
	Failed
};

enum class ScriptBuildReason
{
	ProjectOpen,
	FileChange,
	ManualRefresh
};

struct ScriptCompilationStatus
{
	// High-level state mirrored into the editor UI and asset metadata so the
	// user can see whether Assembly-CSharp is queued, compiling, or ready.
	ScriptCoordinatorState state = ScriptCoordinatorState::Idle;
	ScriptBuildProfile activeProfile = ScriptBuildProfile::Release;
	bool gameplayAssemblyLoaded = false;
	bool queuedBuild = false;
	std::filesystem::path activeAssemblyPath;
	std::string lastSummary;
	std::string lastBuildOutput;
};

// Centralizes project-script workspace generation, dotnet builds, and hot
// reload requests so the editor always serializes script work through one
// single-flight coordinator.
class ScriptCompilationSubsystem : public IEditorSubsystem
{
public:
	static ScriptCompilationSubsystem& Instance();

	void Initialize(EditorContext& context) override;
	void Update(float deltaTime) override;
	void DrawUI() override;
	void Shutdown() override;
	std::string GetName() const override;

	void OnProjectOpened();
	void OnProjectClosed();

	void RequestBuild(ScriptBuildReason reason);
	void NotifyScriptSourceChanged(const std::filesystem::path& scriptPath);

	bool CreateScriptAsset(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError = nullptr);
	bool OpenScript(const std::filesystem::path& scriptPath, std::string* outError = nullptr);
	bool OpenWorkspace(std::string* outError = nullptr);
	bool RegenerateWorkspace(std::string* outError = nullptr);

	[[nodiscard]] const ScriptCompilationStatus& GetStatus() const;
	[[nodiscard]] const ScriptWorkspaceService& GetWorkspaceService() const;
	[[nodiscard]] bool IsBusy() const;
	[[nodiscard]] bool ShouldBlockEditorInteraction() const;

private:
	struct ScriptBuildResult
	{
		// Result returned from the async dotnet build worker back onto the editor
		// thread, including stdout/stderr for logging and inspector diagnostics.
		bool success = false;
		ScriptBuildProfile profile = ScriptBuildProfile::Release;
		std::filesystem::path assemblyPath;
		std::string buildOutput;
		std::string failureMessage;
	};

	ScriptCompilationSubsystem() = default;

	void PrepareForBuild();
	void RestorePausedGameplayState();
	bool TryReloadWorkspaceAssembly(std::string* outError);
	void SynchronizeBoundScriptTypesFromAssets();

	void BeginBuild();
	void HandleCompletedBuild(ScriptBuildResult result);
	void UpdateSharedState();

	[[nodiscard]] ScriptBuildResult RunBuildJob(ScriptBuildProfile profile);

	EditorContext* m_Context = nullptr;
	ScriptWorkspaceService m_WorkspaceService;
	ScriptCompilationStatus m_Status;
	// Only one build may be active at a time so Assembly-CSharp reloads remain
	// serialized and never race against each other.
	std::future<ScriptBuildResult> m_ActiveBuild;
	bool m_BuildInFlight = false;
	ScriptBuildReason m_LastQueuedReason = ScriptBuildReason::ProjectOpen;
	// Small debounce window that collapses save bursts into one build without
	// delaying project-open or already-idle file-change rebuilds for long.
	float m_DebounceTimer = 0.0f;
	bool m_ShouldRestorePausedGameplayState = false;
	bool m_WasRunningBeforeBuild = false;
	bool m_WasPausedBeforeBuild = false;
};
