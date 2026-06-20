// MainEngine.cpp — Implementation of the core engine facade and main loop.
//
// This file implements the primary Engine class methods, including the main Run
// loop, system initialization, and per-frame update logic. It coordinates the
// lifecycle of all major engine subsystems such as rendering, scripting, input,
// and the windowing system.

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
#include "Runtime/BootSummary.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scheduling/EcsQueryTaskGraph.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scripting/ScriptingSystem.h"
#include "Scripting/ScriptGlue.h"
#include "UI/UISystem.h"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
	TerraEngine::Scheduling::SystemDeclaration MakeEngineSystemDeclaration(std::string name, std::string phase, std::vector<std::string> reads, std::vector<std::string> writes, bool requiresFlushedSceneMutations = false)
	{
		TerraEngine::Scheduling::SystemDeclaration declaration;
		declaration.name = std::move(name);
		declaration.phase = std::move(phase);
		declaration.reads = std::move(reads);
		declaration.writes = std::move(writes);
		declaration.mainThread = true;
		declaration.requiresFlushedSceneMutations = requiresFlushedSceneMutations;
		declaration.profilingLabel = declaration.name;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	void RegisterEngineSystem(std::vector<SystemsBase*>& systems, TerraEngine::Scheduling::SystemScheduler& scheduler, SystemsBase& system, TerraEngine::Scheduling::SystemDeclaration declaration)
	{
		// Keep the legacy vector for initialization, shutdown, and GetSystem()
		// while the scheduler owns only ordering metadata for frame updates.
		systems.push_back(&system);
		scheduler.RegisterSystem(system, std::move(declaration));
	}

}

void Engine::Run()
{
	Initialize();
	
	while(true) {
		const double deltaSeconds = frameTimer.Tick();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!Update((float)deltaSeconds)) break;
	}
	
	Shutdown();
}

// Engine::SignalShutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Engine::SignalShutdown()
{
	running = false;
}

void Engine::ResetFrameTimer()
{
	// Editor actions such as entering Play Mode can perform blocking work after
	// the engine frame has already updated. Resetting here lets the next runtime
	// tick measure only gameplay frame time instead of editor transition cost.
	frameTimer = FrameTimer();
}

// Engine::SetEditorMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Engine::SetEditorMode(SystemsBase* system)
{
	editorSystem = system;
	editorMode = true;
}

// Engine::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Engine::Initialize()
{
	// Register the currently booting engine instance so script glue can signal
	// the outer run loop when Application.Quit() is invoked in exports.
	SetApplicationHostEngine(this);
	pool.initialize();
	
	systems.clear();
	systemScheduler.Clear();
	systemScheduler.SetMutationFlushPoints(TerraEngine::Scheduling::DefaultMutationFlushPoints());

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
	RegisterEngineSystem(systems, systemScheduler, windowSystem, MakeEngineSystemDeclaration("WindowSystem", "PreFrame", {}, { "Window" }));

	// Set up the input system after the window so the backend can attach to the
	// native window handle without duplicating platform specific code here.
	InputSystem& inputSystem = InputSystem::Instance();
	inputSystem.Configure(std::make_unique<GlfwInputBackend>());
	RegisterEngineSystem(systems, systemScheduler, inputSystem, MakeEngineSystemDeclaration("InputSystem", "Input", { "Window" }, { "Input" }));

	// The scene manager owns authored .tescene and .teprefab JSON documents so
	// gameplay code can request them without re-reading disk on every access.
	SceneManager& sceneManager = SceneManager::Instance();
	RegisterEngineSystem(systems, systemScheduler, sceneManager, MakeEngineSystemDeclaration("SceneManager", "Scene", { "Resources" }, { "SceneGraph", "ObjectManager" }));

	// ScriptingSystem manages the .NET runtime and gameplay scripts.
	ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
	RegisterEngineSystem(systems, systemScheduler, scriptingSystem, MakeEngineSystemDeclaration("ScriptingSystem", "Scripting", { "SceneGraph", "Resources" }, { "Scripting", "ObjectManager", "SceneMutationQueue" }));

	// UISystem parses `.tuiml`/`.tuss` assets and builds CPU-side UI batches
	// after gameplay and scene state have been updated for the current frame.
	UISystem& uiSystem = UISystem::Instance();
	RegisterEngineSystem(systems, systemScheduler, uiSystem, MakeEngineSystemDeclaration("UISystem", "UI", { "SceneGraph", "ObjectManager", "Scripting" }, { "UI" }, true));

	// RenderSystem consumes scene data and produces frames.
	RenderSystem& renderSystem = RenderSystem::Instance();
	renderSystem.SetEditorModeEnabled(editorMode);
	if (const auto configuredRenderSettings = resourceManager.GetConfiguredRenderSettings()) {
		// Export profiles are resolved before runtime starts, so applying them
		// here gives the renderer one stable quality contract for the session.
		renderSystem.SetRenderSettings(*configuredRenderSettings);
	}
	RegisterEngineSystem(systems, systemScheduler, renderSystem, MakeEngineSystemDeclaration("RenderSystem", "Render", { "Window", "SceneGraph", "ObjectManager", "UI", "Resources" }, { "Renderer" }, true));

	const TerraEngine::Scheduling::ScheduleSummary& schedulerSummary = systemScheduler.BuildSchedule();
	TERRAENGINE_LOG(LogLevel::Info, schedulerSummary.formatted);
	if (!schedulerSummary.valid) {
		throw std::runtime_error("System scheduler validation failed. See scheduler summary diagnostics in the log.");
	}
	TerraEngine::Scheduling::JobGraphSchedulerLoweringInput loweringInput;
	loweringInput.graphId = "engine-system-scheduler";
	loweringInput.label = "Engine System Scheduler";
	loweringInput.phases = systemScheduler.PhaseDefinitions();
	loweringInput.orderedSystems = systemScheduler.OrderedDeclarations();
	loweringInput.mutationFlushPoints = systemScheduler.MutationFlushPoints();
	TerraEngine::Scheduling::JobGraph loweredGraph = TerraEngine::Scheduling::JobGraph::FromSchedulerLowering(loweringInput);
	latestJobGraphSummary = loweredGraph.Validate();
	TERRAENGINE_LOG(LogLevel::Info, latestJobGraphSummary.formatted);
	if (!latestJobGraphSummary.valid) {
		throw std::runtime_error("System scheduler job graph validation failed. See job graph summary diagnostics in the log.");
	}

	std::size_t initializedSystemCount = 0;
	bool editorSystemInitialized = false;
	bool resourceManagerInitialized = false;
	try {
		resourceManager.Initialize();
		resourceManagerInitialized = true;
		sceneManager.SetSceneRoot(resourceManager.GetAssetRoot());
		sceneManager.SetStartupScene(resourceManager.GetConfiguredStartupScene().value_or(std::filesystem::path{}));

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto* system : systems) {
			system->Initialize();
			initializedSystemCount++;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (initializedSystemCount > 0) {
			systems[--initializedSystemCount]->Shutdown();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (resourceManagerInitialized) {
			resourceManager.Shutdown();
		}

		pool.shutdown();
		SetApplicationHostEngine(nullptr);
		throw;
	}
	
	TerraEngine::Runtime::ExportStartupCoordinatorInput startupInput;
	startupInput.exportRuntime = resourceManager.IsExportRuntime();
	startupInput.contentMounted = !startupInput.exportRuntime || ContentMount::Instance().IsMounted();
	startupInput.metadataLoaded = !resourceManager.GetProductName().empty() && !resourceManager.GetVersion().empty();
	startupInput.startupSceneConfigured = resourceManager.GetConfiguredStartupScene().has_value();
	startupInput.startupSceneLoaded = !startupInput.startupSceneConfigured || sceneManager.GetActiveScene() != nullptr;
	startupInput.projectName = resourceManager.GetProductName();
	startupInput.version = resourceManager.GetVersion();
	startupInput.contentSource = startupInput.exportRuntime ? "export-archive" : resourceManager.GetProjectRoot().generic_string();
	startupInput.startupScene = resourceManager.GetConfiguredStartupScene().has_value()
		? resourceManager.GetConfiguredStartupScene()->generic_string()
		: std::string("<none>");
	startupInput.renderProfile = resourceManager.GetRenderProfileSummary();
	startupInput.validationEnabled = resourceManager.IsRenderValidationEnabled();
	startupInput.debugEnabled = resourceManager.IsRenderDebugEnabled();
	if (sceneManager.GetActiveScene() != nullptr) {
		auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
		auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
		TERRAENGINE_LOG(LogLevel::Info, "Export startup scene summary: objects=" << summary.objects.size() << ", diagnostics=" << summary.diagnostics.size());
	}
	for (auto* system : systems) {
		startupInput.requiredSystems.push_back({
			system->SystemName(),
			true,
			true,
			"Initialized"
		});
	}
	const TerraEngine::Runtime::ExportStartupCoordinatorResult startupResult = TerraEngine::Runtime::CoordinateExportStartup(startupInput);
	const TerraEngine::Runtime::BootSummary& bootSummary = startupResult.summary;
	TERRAENGINE_LOG(LogLevel::Info, TerraEngine::Runtime::FormatBootSummary(bootSummary));
	if (!startupResult.CanEnterRuntime()) {
		throw std::runtime_error(bootSummary.runtimeEntryReason);
	}

	// Reset after all startup/editor setup has finished so the first frame delta
	// represents frame time, not project loading, script compilation, or shader
	// preparation that happened before the runtime loop could begin.
	ResetFrameTimer();
}

// Engine::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

		// Real systems publish ECS query graph summaries as they execute. Clear
		// the frame collector here so scheduler diagnostics piggyback on actual
		// system scans instead of running a duplicate global diagnostic pass.
		const bool ecsGraphDiagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		if (ecsGraphDiagnosticsEnabled)
		{
			TerraEngine::Scheduling::ClearPublishedEcsGraphFrameSummaries();
		}

		const TerraEngine::Scheduling::ScheduleSummary& schedulerSummary = systemScheduler.ExecuteFrame(deltaTime, [](const TerraEngine::Scheduling::MutationFlushPoint&) {
			TerraEngine::Scheduling::MutationFlushRuntimeState state;
			TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			if (!queue.Empty()) {
				// Structural scene mutations are applied only at declared flush
				// points so systems that iterate hierarchy/component data never
				// observe the ObjectManager changing midway through their update.
				const TerraEngine::Scene::SceneMutationBatchResult result = queue.Apply(ObjectManager::instance());
				state.message = result.applied ? "Scene mutation queue flushed." : "Scene mutation queue rejected one or more requests.";
			}
			state.pendingAfterFlush = queue.Size();
			return state;
		});
		if (!schedulerSummary.valid) {
			TERRAENGINE_LOG(LogLevel::Error, schedulerSummary.formatted);
		}
		if (ecsGraphDiagnosticsEnabled)
		{
			systemScheduler.SetLatestComponentQueryGroups(TerraEngine::Scheduling::PublishedEcsComponentQueryGroups());
			systemScheduler.SetLatestEcsGraphSummary(TerraEngine::Scheduling::FormatPublishedEcsGraphFrameSummaries());
		}
	}
	TerraEngine::Profiling::Profiler::Instance().EndFrame();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// Engine::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void Engine::Shutdown()
{
	if (editorMode) {
		editorSystem->Shutdown();
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
