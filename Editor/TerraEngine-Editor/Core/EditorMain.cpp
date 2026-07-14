// EditorMain.cpp — Native editor entry point
//
// Contains the main function for the native editor executable, responsible for initializing the engine and handing off control to the EditorSystem. it establishes the primary loop and ensures the editor mode is correctly registered with the rendering backend.

#include <iostream>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorCommandLine.h"
#include "EditorStartupRendererConfiguration.h"
#include "EditorSystem.h"
#include "Engine/Resource/ResourceManager.h"

int main(int argc, char** argv) {
	const EditorCommandLineOptions commandLine = ParseEditorCommandLine(argc, argv);
	const TerraEngine::Editor::EditorStartupRendererConfiguration startupRenderer =
		TerraEngine::Editor::ResolveEditorStartupRendererConfiguration(commandLine.startupProjectPath);
	ResourceManager::Instance().ConfigureEditorStartupProfile(
		startupRenderer.renderSettings,
		startupRenderer.validationEnabled,
		startupRenderer.debugEnabled,
		startupRenderer.projectRoot,
		startupRenderer.origin == TerraEngine::Editor::StartupRendererOrigin::DirectProject
			? "DirectProject" : "HubDefaults");
	for (const std::string& diagnostic : startupRenderer.diagnostics) {
		std::cerr << "[Editor startup renderer] " << diagnostic << '\n';
	}
	Engine engine;
	
	EditorSystem editorSystem;
	if (commandLine.startupProjectPath.has_value()) {
		editorSystem.SetStartupProjectPath(*commandLine.startupProjectPath);
	}
	if (commandLine.physicsInvariantOutputPath.has_value()) {
		editorSystem.SetPhysicsInvariantOutputPath(*commandLine.physicsInvariantOutputPath);
	}
	// Pass the engine pointer to the editor system so it can access engine state/systems
	editorSystem.Setup(&engine);
	
	// Register the editor system with the engine so the render loop calls EditorSystem::Draw()
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	return editorSystem.RequestedProcessExitCode();
}
