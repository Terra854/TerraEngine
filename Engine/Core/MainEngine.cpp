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
#include "Object/ComponentRegistry.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Resource/ResourceManager.h"
#include "Scripting/ScriptingSystem.h"
#include "Scripting/ScriptGlue.h"
#include "UI/UISystem.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

void Engine::Run()
{
	Initialize();
	
	while(true) {
		const double deltaSeconds = frameTimer.Tick();
		if (!Update((float)deltaSeconds)) break;
	}
	
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
	// Register the currently booting engine instance so script glue can signal
	// the outer run loop when Application.Quit() is invoked in exports.
	SetApplicationHostEngine(this);
	pool.initialize();
	
	// Reset the frame timer so the first tick is measured from initialization time.
	frameTimer = FrameTimer();

	// Register all components so they are available for reflection/editor
	ComponentRegistry::RegisterAllComponents();

	// ResourceManager is not a frame-updated system, but export builds still
	// need a deterministic startup hook so the TEPAK archive is mounted and the
	// startup scene is discovered before SceneManager begins loading content.
	ResourceManager& resourceManager = ResourceManager::Instance();

	// The window system is initialized first so other systems can depend on
	// a valid window context if they need one later.
	WindowConfig windowConfig{};
	windowConfig.resizable = editorMode;
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

	// ScriptingSystem manages the .NET runtime and gameplay scripts.
	ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
	systems.push_back(&scriptingSystem);

	// UISystem parses `.tuiml`/`.tuss` assets and builds CPU-side UI batches
	// after gameplay and scene state have been updated for the current frame.
	UISystem& uiSystem = UISystem::Instance();
	systems.push_back(&uiSystem);

	// RenderSystem consumes scene data and produces frames.
	RenderSystem& renderSystem = RenderSystem::Instance();
	renderSystem.SetEditorModeEnabled(editorMode);
	systems.push_back(&renderSystem);

	std::size_t initializedSystemCount = 0;
	bool editorSystemInitialized = false;
	bool resourceManagerInitialized = false;
	try {
		resourceManager.Initialize();
		resourceManagerInitialized = true;
		sceneManager.SetSceneRoot(resourceManager.GetAssetRoot());
		sceneManager.SetStartupScene(resourceManager.GetConfiguredStartupScene().value_or(std::filesystem::path{}));

		for (auto* system : systems) {
			system->Initialize();
			initializedSystemCount++;
		}

		if (editorMode) {
			editorSystem->Initialize();
			editorSystemInitialized = true;
		}
	}
	catch (...) {
		// If any system throws during startup, unwind only the systems that did
		// finish initialization so exported builds do not leave windowing,
		// rendering, or threading resources half alive after a fatal error.
		if (editorSystemInitialized && editorSystem != nullptr) {
			editorSystem->Shutdown();
		}

		while (initializedSystemCount > 0) {
			systems[--initializedSystemCount]->Shutdown();
		}

		if (resourceManagerInitialized) {
			resourceManager.Shutdown();
		}

		pool.shutdown();
		SetApplicationHostEngine(nullptr);
		throw;
	}
	
	// Confirm engine initialization finished so later logs have a baseline.
	TERRAENGINE_LOG(LogLevel::Info, "Setup complete!");
}

bool Engine::Update(float deltaTime)
{
	if (!running) {
		return false;
	}

	// The profiler UI is editor-only, but captures measure engine systems only.
	// Editor UI/update work runs after EndFrame so editor overhead is not mixed
	// into engine frame analysis.
	TerraEngine::Profiling::Profiler::Instance().BeginFrame();
	{
		TERRAENGINE_PROFILE_SCOPE("Engine::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);

		// Run each registered system with an individual named scope. The system
		// name is copied by the profiler at scope construction time, so the
		// temporary string remains valid for the marker label.
		for (auto* system : systems) {
			system->Update(deltaTime);
		}
	}
	TerraEngine::Profiling::Profiler::Instance().EndFrame();

	if (editorMode) {
		editorSystem->Update(deltaTime);
	}

	// If a system requested shutdown (e.g., the window was closed),
	// exit the loop after this iteration.
	if (!running) {
		return false;
	}
	
	// Simple pacing to avoid a busy loop when no systems perform a
	// blocking wait. Sleep for a tiny duration based on the measured
	// delta time to yield time slices back to the OS.
	const auto sleepDuration = std::chrono::duration<double>(std::max(0.0, 0.001 - (double)deltaTime));
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

	ResourceManager::Instance().Shutdown();
	
	pool.shutdown();
	
	// Notify that the engine shutdown path completed and logging can be stopped soon.
	TERRAENGINE_LOG(LogLevel::Info, "Shutdown complete!");
	
	// Stop the logger thread so queued messages flush before the application exits.
	Logger::Instance().Shutdown();

	// Clear the non-owning quit hook after every shutdown so a future engine
	// session in the same process cannot target a stale instance pointer.
	SetApplicationHostEngine(nullptr);
}
