#include <iostream>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorSystem.h"

int main(int argc, char** argv) {
	Engine engine;
	
	EditorSystem editorSystem;
    // Pass the engine pointer to the editor system so it can access engine state/systems
	editorSystem.Setup(&engine);
	
    // Register the editor system with the engine so the render loop calls EditorSystem::Draw()
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	return 0;
}