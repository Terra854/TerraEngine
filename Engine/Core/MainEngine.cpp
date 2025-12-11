#include "MainEngine.h"
#include "SystemBase.h"
#include "Logger/Logger.h"
#include "Timer.h"
#include "Input/GLFW/GlfwInputBackend.h"
#include "Input/InputSystem.h"
#include "Scene/SceneManager.h"
#include "Renderer/RenderSystem.h"
#include "Window/GLFW/GlfwWindowBackend.h"
#include "Window/WindowSystem.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

void Engine::Run()
{
	Initialize();
	
	while(Update()) {}
	
	Shutdown();
}

void Engine::SignalShutdown()
{
	running = false;
}

void Engine::SetEditorMode(SystemsBase* system)
{
	editorSystem = system;
	editorMode = true;
}

void Engine::Initialize()
{
	pool.initialize();
	
	// Reset the frame timer so the first tick is measured from initialization time.
	frameTimer = FrameTimer();

	// The window system is initialized first so other systems can depend on
	// a valid window context if they need one later.
	WindowConfig windowConfig{};
	WindowSystem& windowSystem = WindowSystem::Instance();
	windowSystem.Configure(std::make_unique<GlfwWindowBackend>(), windowConfig, [this]() {
		SignalShutdown();
		});
	systems.push_back(&windowSystem);

	// Set up the input system after the window so the backend can attach to the
	// native window handle without duplicating platform specific code here.
	InputSystem& inputSystem = InputSystem::Instance();
	inputSystem.Configure(std::make_unique<GlfwInputBackend>());
	systems.push_back(&inputSystem);

	// The scene manager owns authored .tescene and .teprefab JSON documents so
	// gameplay code can request them without re-reading disk on every access.
	SceneManager& sceneManager = SceneManager::Instance();
	systems.push_back(&sceneManager);

	// RenderSystem consumes scene data and produces frames.
	RenderSystem& renderSystem = RenderSystem::Instance();
	systems.push_back(&renderSystem);
	
	for (auto* system : systems) {
		system->Initialize();
	}

	if (editorMode) {
		editorSystem->Initialize();
	}
	
	// Confirm engine initialization finished so later logs have a baseline.
	TERRAENGINE_LOG(LogLevel::Info, "Setup complete!");
}

bool Engine::Update()
{
	if (!running) {
		return false;
	}

	const double deltaSeconds = frameTimer.Tick();
	
	// Run each registered system. Systems can consult the engine's
	// state or the delta time if they need it for frame dependent
	// calculations.
	for (auto* system : systems) {
		system->Update();
	}
	
	if (editorMode) {
		editorSystem->Update();
	}

	// If a system requested shutdown (e.g., the window was closed),
	// exit the loop after this iteration.
	if (!running) {
		return false;
	}
	
	// Simple pacing to avoid a busy loop when no systems perform a
	// blocking wait. Sleep for a tiny duration based on the measured
	// delta time to yield time slices back to the OS.
	const auto sleepDuration = std::chrono::duration<double>(std::max(0.0, 0.001 - deltaSeconds));
	if (sleepDuration.count() > 0.0) {
		std::this_thread::sleep_for(sleepDuration);
	}

	return true;
}

void Engine::Shutdown()
{
	if (editorMode) {
		editorSystem->Shutdown();
	}

	for (auto it = systems.rbegin(); it != systems.rend(); ++it) {
		(*it)->Shutdown();
	}
	
	pool.shutdown();
	
	// Notify that the engine shutdown path completed and logging can be stopped soon.
	TERRAENGINE_LOG(LogLevel::Info, "Shutdown complete!");
	
	// Stop the logger thread so queued messages flush before the application exits.
	Logger::Instance().Shutdown();
}
