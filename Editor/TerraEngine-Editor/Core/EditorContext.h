// EditorContext.h — Shared editor state and context
//
// Centralizes the shared handles and lightweight state snapshots used across the editor's subsystems and windows. it provides a unified context for accessing the engine instance, project manager, and active scripting state.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
// Forward declarations
class Engine;
class ProjectManager;
class AssetDatabase;
class EditorExportOperationController;
// class EditorProfilerService groups related state and operations so callers have one documented contract.
class EditorProfilerService;
namespace TerraEngine::Editor::Automation
{
	class EditorMcpBridge;
}

// Lightweight scripting snapshot shared across editor windows. This avoids
// having every window query multiple subsystems during each ImGui draw pass.
struct ScriptEditorState {
	std::string coordinatorState = "Idle";
	std::string activeProfile = "Release";
	std::string lastSummary;
	std::filesystem::path activeAssemblyPath;
	bool gameplayAssemblyLoaded = false;
	// Bump whenever the inspector target changes so the live watch path knows it
	// should refresh immediately for the newly selected object.
	std::uint64_t selectionVersion = 0;
	bool hasWatchedObject = false;
	std::uint32_t watchedObjectId = 0;
	// While a field is actively being edited, the inspector can use this to
	// avoid background refreshes fighting the user's in-progress input.
	std::string activeEditedBindingId;
	std::string activeEditedFieldPath;
};

// struct EditorContext groups related state and operations so callers have one documented contract.
struct EditorContext {
	Engine* engine = nullptr;
	std::shared_ptr<ProjectManager> projectManager;
	std::shared_ptr<AssetDatabase> assetDatabase;
	// Project-owned exports survive a hidden window and drain before scripts,
	// scene/resources or the shared pool can be released.
	std::shared_ptr<EditorExportOperationController> exportController;
	std::shared_ptr<EditorProfilerService> editorProfiler;
	// The MCP bridge is editor-owned and opt-in; keeping it in shared context
	// lets windows display status while subsystems route updates through one
	// lifecycle-managed object.
	std::shared_ptr<TerraEngine::Editor::Automation::EditorMcpBridge> editorMcpBridge;
	ScriptEditorState scripting;
};
