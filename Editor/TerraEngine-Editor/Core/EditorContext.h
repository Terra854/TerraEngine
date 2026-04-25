#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
// Forward declarations
class Engine;
class ProjectManager;
class AssetDatabase;
class EditorProfilerService;

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

struct EditorContext {
	Engine* engine = nullptr;
	std::shared_ptr<ProjectManager> projectManager;
	std::shared_ptr<AssetDatabase> assetDatabase;
	std::shared_ptr<EditorProfilerService> editorProfiler;
	ScriptEditorState scripting;
};
