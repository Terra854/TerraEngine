#include <iostream>
#include <vector>

#include "Engine/Core/MainEngine.h"
#include "EditorSystem.h"

int main(int argc, char** argv) {
	Engine engine;
	
	// Register default systems first (Window, Input, Scene, Render)
	// Actually, we want to inject EditorSystem BEFORE RenderSystem but AFTER Window/Input?
	// Or we can just add them all manually.
	// Engine::RegisterDefaultSystems() adds: Window, Input, Scene, Render.
	// If we add EditorSystem AFTER, it runs AFTER Render.
	// But RenderSystem::Update() does the Presentation.
	// We need EditorSystem::Update() to run BEFORE Presentation.
	// Because EditorSystem::Update calls rhi->PresentEditor(), which does Presentation.
	// Wait, RenderSystem::Update also calls rhi->RenderFrame().
	// If Editor Mode is on, rhi->RenderFrame() renders scene but DOES NOT Present.
	// rhi->PresentEditor() DOES Present.
	// So EditorSystem must run AFTER RenderSystem?
	// 1. RenderSystem::Update -> Renders Scene to Texture.
	// 2. EditorSystem::Update -> Draws UI using Texture -> Calls PresentEditor -> Presents.
	// Yes, this order works.
	// So appending EditorSystem to the list is correct.

	engine.RegisterDefaultSystems();
	
	EditorSystem editorSystem(engine);
	engine.AddSystem(&editorSystem);

	engine.Run();

	return 0;
}