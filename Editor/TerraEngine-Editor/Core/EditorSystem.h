// EditorSystem.h — Main editor system orchestrator
//
// Defines the primary system responsible for managing the editor's lifecycle, UI rendering, and managed code integration. it coordinates between the project hub, the main editor workspace, and the .NET host.

#pragma once

#include "Core/SystemBase.h"
#include "EditorContext.h"
#include <filesystem>
#include <memory>
#include <optional>

class ProjectHubSubsystem;
class MainEditorSubsystem;
// class Engine groups related state and operations so callers have one documented contract.
class Engine;

class EditorSystem : public SystemsBase {
public:
	void SetStartupProjectPath(const std::filesystem::path& projectPath);
	// Configures the opt-in installed-editor acceptance run. The normal editor
	// path leaves this unset and therefore never writes validation evidence.
	void SetPhysicsInvariantOutputPath(const std::filesystem::path& outputPath);
	int RequestedProcessExitCode() const noexcept;
	void Setup(Engine* engine);
	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;
	TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;

	void SaveWorkspaceLayout();
	void LoadWorkspaceLayout();

	void Draw();

private:
	EditorContext context;
	std::shared_ptr<ProjectHubSubsystem> projectHub;
	std::shared_ptr<MainEditorSubsystem> mainEditor;
	std::optional<std::filesystem::path> startupProjectPath;
	std::optional<std::filesystem::path> physicsInvariantOutputPath;

	// Managed Editor Hooks
	typedef void (*ManagedInitFn)(void*);
	typedef void (*ManagedUpdateFn)(float);
	typedef void (*ManagedShutdownFn)();
	typedef void (*ManagedDrawWindowMenuFn)();
	typedef void (*ManagedGetWindowsFn)(void (*callback)(const char*, int));
	typedef void (*ManagedSetWindowVisibilityFn)(const char*, int);
	typedef void (*ManagedHideAllWindowsFn)();

	ManagedInitFn managedInitialize = nullptr;
	ManagedUpdateFn managedUpdate = nullptr;
	ManagedShutdownFn managedShutdown = nullptr;
	ManagedDrawWindowMenuFn managedDrawWindowMenu = nullptr;
	ManagedGetWindowsFn managedGetWindows = nullptr;
	ManagedSetWindowVisibilityFn managedSetWindowVisibility = nullptr;
	ManagedHideAllWindowsFn managedHideAllWindows = nullptr;

	bool initialized = false;
	bool pendingLoadLayout = false;
	bool pendingSaveLayout = false;
	bool startupProjectOpenAttempted = false;
	bool physicsInvariantRunAttempted = false;
	// The installed acceptance command waits for the normal asynchronous
	// Assembly-CSharp coordinator instead of canceling it during early shutdown.
	float physicsInvariantManagedWaitSeconds = 0.0f;
	int requestedProcessExitCode = 0;
	std::uint64_t imguiPresentationGeneration = 0;
	void HandleProjectLoaded();
	void ProcessStartupProjectOpen();
	void ProcessPhysicsInvariantValidation(float deltaTime);
public:
	void DrawWindowMenu();
	void HideAllWindows();
	void GetManagedWindows(void (*callback)(const char*, int));
};
