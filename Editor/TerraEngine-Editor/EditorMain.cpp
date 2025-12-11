#include <iostream>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorSystem.h"

int main(int argc, char** argv) {
	Engine engine;
	
	EditorSystem editorSystem(engine);
	engine.SetEditorMode(&editorSystem);

	engine.Run();

	return 0;
}
