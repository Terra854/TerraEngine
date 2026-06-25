// EditorMain.cpp — Native editor entry point
//
// Contains the main function for the native editor executable, responsible for initializing the engine and handing off control to the EditorSystem. it establishes the primary loop and ensures the editor mode is correctly registered with the rendering backend.

#include <iostream>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorCommandLine.h"
#include "EditorSystem.h"

int main(int argc, char** argv) {
	const EditorCommandLineOptions commandLine = ParseEditorCommandLine(argc, argv);
	Engine engine;
	
	EditorSystem editorSystem;
	if (commandLine.startupProjectPath.has_value()) {
		editorSystem.SetStartupProjectPath(*commandLine.startupProjectPath);
	}
	// Pass the engine pointer to the editor system so it can access engine state/systems
	editorSystem.Setup(&engine);
	
	// Register the editor system with the engine so the render loop calls EditorSystem::Draw()
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	return 0;
}
