// MainEngine.cpp — Implementation of the core engine facade and main loop.
//
// This file implements the primary Engine class methods, including the main Run
// loop, system initialization, and per-frame update logic. It coordinates the
// lifecycle of all major engine subsystems such as rendering, scripting, input,
// and the windowing system.

#include "MainEngine.h"
#include "SystemBase.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/AnimationSystem.h"
#include "Logger/Logger.h"
#include "Timer.h"
#include "Input/GLFW/GlfwInputBackend.h"
#include "Input/InputSystem.h"
#include "Scene/SceneManager.h"
#include "Renderer/RenderSystem.h"
#include "Core/FatalErrorReport.h"
#include "Window/GLFW/GlfwWindowBackend.h"
#include "Window/WindowSystem.h"
#include "Object/ComponentRegistry.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainStreamingSystem.h"
#include "Profiling/Profiler.h"
#include "Profiling/ProfilerScope.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryStreamingSystem.h"
#include "Runtime/BootSummary.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scheduling/EcsQueryTaskGraph.h"
#include "Scene/SceneMutationQueue.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/RuntimeLayout.h"
#include "Scene/WorldSpatialSystem.h"
#include "Scripting/ScriptingSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Audio/AudioSystem.h"
#include "Scripting/ScriptGlue.h"
#include "UI/UISystem.h"
#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
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

	TerraEngine::Scheduling::SystemDeclaration MakeSceneManagerDeclaration()
	{
		TerraEngine::Scheduling::SystemDeclaration declaration = MakeEngineSystemDeclaration(
			"SceneManager",
			"Scene",
			{ "Resources", "HierarchySnapshot", "DirtyTransformState" },
			{ "SceneGraph", "ObjectManager", "TransformWorkUnits", "RuntimeLayout" });
		// SceneManager still owns live scene documents and ObjectManager-facing
		// commits on the coordinator thread. The worker-safe part is now named
		// separately so diagnostics can distinguish immutable hierarchy/runtime
		// layout preparation from protected scene mutation.
		declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.safeSubwork = "Hierarchy snapshots, dirty transform work plans, and compact runtime layout records from immutable scene copies.";
		declaration.protectedCommit = "Active scene document ownership, scene load activation, and live ObjectManager hierarchy/component mutation.";
		declaration.protectedReason = declaration.protectedCommit;
		return declaration;
	}

	void RegisterEngineSystem(std::vector<SystemsBase*>& systems, TerraEngine::Scheduling::SystemScheduler& scheduler, SystemsBase& system, TerraEngine::Scheduling::SystemDeclaration declaration)
	{
		// Keep the legacy vector for initialization, shutdown, and GetSystem()
		// while the scheduler owns only ordering metadata for frame updates.
		systems.push_back(&system);
		scheduler.RegisterSystem(system, std::move(declaration));
	}

	[[noreturn]] void ShowControlledFatalEngineDialog(std::string reason, std::string details, bool exportRuntime)
	{
		// This path deliberately avoids renderer/ImGui dependencies. It is used
		// after startup, scheduler, or renderer-recovery failures when Vulkan
		// output may already be unavailable.
		TerraEngine::FatalErrorDialogReport report = TerraEngine::BuildFatalErrorDialogReport(
			exportRuntime ? TerraEngine::FatalErrorCategory::ExportFatal : TerraEngine::FatalErrorCategory::ControlledEngineFatal,
			std::move(reason),
			std::move(details));
		const std::string crashProductName = exportRuntime
			? ResourceManager::Instance().GetProductName()
			: std::string(TerraEngine::kEditorFatalErrorProductName);
		report = TerraEngine::PlatformUtils::WriteCrashDumpForReport(report, crashProductName);
		WindowSystem::Instance().ShowFatalErrorDialog(report);
	}

}

void Engine::Run()
{
	bool initializedForRun = false;
	try
	{
		Initialize();
		initializedForRun = true;
		
		while(true) {
			const double deltaSeconds = frameTimer.Tick();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!Update((float)deltaSeconds)) break;
		}
		
		Shutdown();
	}
	catch (const std::exception& exception)
	{
		ShowControlledFatalEngineDialog(
			exception.what(),
			initializedForRun ? "Engine runtime stopped after a controlled fatal error." : "Engine startup stopped before normal runtime entry.",
			ResourceManager::Instance().IsExportRuntime());
	}
	catch (...)
	{
		ShowControlledFatalEngineDialog(
			"Unknown fatal engine error.",
			initializedForRun ? "Engine runtime stopped after an unknown fatal error." : "Engine startup stopped before normal runtime entry.",
			ResourceManager::Instance().IsExportRuntime());
	}
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

void Engine::ConfigureHeadlessPhysicsValidation()
{
	// This flag is intentionally set before Initialize(). The normal Engine
	// lifecycle still owns resource and worker teardown after the validation.
	headlessPhysicsValidation = true;
}

void Engine::ConfigureHeadlessCpuFallback()
{
	// Keep one headless lifecycle flag so physics parity and animation export
	// validation cannot diverge in their scheduler membership. With RenderSystem
	// absent, Auto animation resolution observes renderer unavailability and
	// selects its already-published CPU deterministic fallback at a safe frame
	// boundary instead of attempting a late device creation.
	headlessPhysicsValidation = true;
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

	if (!headlessPhysicsValidation) {
		// Interactive runtimes require a window and input before any system that
		// consumes platform state. The headless parity mode deliberately omits both.
		WindowConfig windowConfig{};
		windowConfig.resizable = true;
		WindowSystem& windowSystem = WindowSystem::Instance();
		windowSystem.Configure(std::make_unique<GlfwWindowBackend>(), windowConfig, [this]() {
			SignalShutdown();
			});
		auto windowDeclaration = MakeEngineSystemDeclaration("WindowSystem", "PreFrame", {}, { "Window" });
		windowDeclaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		windowDeclaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
		windowDeclaration.hardBoundary = true;
		windowDeclaration.safeSubwork = "Immutable window state snapshots for worker readers after protected event polling.";
		windowDeclaration.protectedCommit = "Platform event polling, close/resize handling, backend callback ingestion, cursor mutation, and swap-buffer calls.";
		windowDeclaration.protectedReason = windowDeclaration.protectedCommit;
		RegisterEngineSystem(systems, systemScheduler, windowSystem, std::move(windowDeclaration));

		InputSystem& inputSystem = InputSystem::Instance();
		inputSystem.Configure(std::make_unique<GlfwInputBackend>());
		auto inputDeclaration = MakeEngineSystemDeclaration("InputSystem", "Input", { "Window" }, { "Input" });
		inputDeclaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
		inputDeclaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::Protected;
		inputDeclaration.hardBoundary = true;
		inputDeclaration.safeSubwork = "Immutable post-poll input/window snapshots for worker readers.";
		inputDeclaration.protectedCommit = "Platform event polling, backend callback ingestion, close/resize handling, cursor mutation, and swap-buffer calls.";
		inputDeclaration.protectedReason = inputDeclaration.protectedCommit;
		RegisterEngineSystem(systems, systemScheduler, inputSystem, std::move(inputDeclaration));
	}

	// The scene manager owns authored .tescene and .teprefab JSON documents so
	// gameplay code can request them without re-reading disk on every access.
	SceneManager& sceneManager = SceneManager::Instance();
	RegisterEngineSystem(systems, systemScheduler, sceneManager, MakeSceneManagerDeclaration());

	// Spatial candidates are prepared as immutable worker data during Scene and
	// become visible only when the scheduler invokes an explicit protected edge.
	TerraEngine::Scene::WorldSpatialSystem& worldSpatialSystem = TerraEngine::Scene::WorldSpatialSystem::Instance();
	RegisterEngineSystem(systems, systemScheduler, worldSpatialSystem, worldSpatialSystem.SchedulerDeclaration());

	// ScriptingSystem manages the .NET runtime and gameplay scripts.
	ScriptingSystem& scriptingSystem = ScriptingSystem::Instance();
	RegisterEngineSystem(systems, systemScheduler, scriptingSystem, scriptingSystem.SchedulerDeclaration());

	// Scripts publish typed animation commands before this phase. AnimationSystem
	// captures those inputs once, prepares/evaluates worker ranges, then commits
	// authoritative root-motion/physics handoffs at its protected boundary.
	TerraEngine::Animation::AnimationSystem& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
	animationSystem.SetHeadlessExecutionRequired(headlessPhysicsValidation);
	RegisterEngineSystem(systems, systemScheduler, animationSystem, animationSystem.SchedulerDeclaration());

	// Editor Play Mode owns world creation/restoration, while exported games
	// start the same shared system during ordinary engine initialization.
	// Terrain collision prepares heightfield candidates on workers and publishes
	// the complete replacement immediately before physics simulation.
	TerraEngine::Physics::Terrain::TerrainCollisionSystem& terrainCollisionSystem =
		TerraEngine::Physics::Terrain::TerrainCollisionSystem::Instance();
	RegisterEngineSystem(systems, systemScheduler, terrainCollisionSystem, terrainCollisionSystem.SchedulerDeclaration());

	TerraEngine::Physics::PhysicsSystem& physicsSystem = TerraEngine::Physics::PhysicsSystem::Instance();
	physicsSystem.SetEditorLifecycleControlled(editorMode);
	auto physicsDeclaration = physicsSystem.SchedulerDeclaration();
	if (headlessPhysicsValidation) {
		// The normal declaration orders physics before presentation. Headless parity
		// deliberately has no UISystem, so remove only that absent optional edge while
		// retaining every Physics phase/resource/boundary declaration.
		std::erase(physicsDeclaration.before, "UISystem");
	}
	RegisterEngineSystem(systems, systemScheduler, physicsSystem, std::move(physicsDeclaration));

	// Audio follows gameplay/physics authoring, prepares spatial voices from an
	// immutable snapshot, and commits provider state before presentation.
	TerraEngine::Audio::AudioSystem& audioSystem = TerraEngine::Audio::AudioSystem::Instance();
	// Exported runtimes begin gameplay immediately. The editor instead opens this
	// gate from StartPlayMode so authored playOnStart sources remain silent while
	// the user edits the scene.
	audioSystem.SetRuntimePlaybackEnabled(!editorMode);
	RegisterEngineSystem(systems, systemScheduler, audioSystem, audioSystem.SchedulerDeclaration());

	// Presentation is a separate post-physics owner so current-step ragdoll
	// snapshots can shape the visual plan without rewriting controller authority.
	// Headless runtimes keep the phase for recording/deterministic handoffs but
	// remove dependencies on presentation-only systems that are not registered.
	TerraEngine::Animation::AnimationPresentationSystem& animationPresentationSystem =
		TerraEngine::Animation::AnimationPresentationSystem::Instance();
	auto animationPresentationDeclaration = animationPresentationSystem.SchedulerDeclaration();
	if(headlessPhysicsValidation) {
		std::erase(animationPresentationDeclaration.before, "UISystem");
		std::erase(animationPresentationDeclaration.before, "RenderSystem");
	}
	RegisterEngineSystem(
		systems,
		systemScheduler,
		animationPresentationSystem,
		std::move(animationPresentationDeclaration));

	if (!headlessPhysicsValidation) {
		// UISystem and RenderSystem are presentation-only. Omitting them keeps the
		// installed parity run independent of a graphics driver and window server.
		UISystem& uiSystem = UISystem::Instance();
		RegisterEngineSystem(systems, systemScheduler, uiSystem, uiSystem.SchedulerDeclaration());

		// Terrain has independent worker preparation and protected publication
		// boundaries. Register it before virtual-geometry and render consumers.
		TerraEngine::Terrain::TerrainStreamingSystem& terrainStreamingSystem =
			TerraEngine::Terrain::TerrainStreamingSystem::Instance();
		RegisterEngineSystem(systems, systemScheduler, terrainStreamingSystem, terrainStreamingSystem.SchedulerDeclaration());
		TerraEngine::Terrain::TerrainDerivedSystem& terrainDerivedSystem =
			TerraEngine::Terrain::TerrainDerivedSystem::Instance();
		RegisterEngineSystem(systems, systemScheduler, terrainDerivedSystem, terrainDerivedSystem.SchedulerDeclaration());

		// Streaming publication is a separate protected boundary. Register it before
		// RenderSystem so accepted page state is complete before the renderer captures
		// this frame's immutable snapshot; it never adds a scene-mutation flush.
		TerraEngine::Resource::VirtualGeometryStreamingSystem& virtualGeometryStreamingSystem =
			TerraEngine::Resource::VirtualGeometryStreamingSystem::Instance();
		RegisterEngineSystem(
			systems,
			systemScheduler,
			virtualGeometryStreamingSystem,
			virtualGeometryStreamingSystem.SchedulerDeclaration());

		RenderSystem& renderSystem = RenderSystem::Instance();
		renderSystem.SetEditorModeEnabled(editorMode);
		RegisterEngineSystem(systems, systemScheduler, renderSystem, renderSystem.SchedulerDeclaration());
	}
	if (editorMode && editorSystem != nullptr) {
		// Editor UI owns the real protected PostFrame boundary. It remains outside
		// the lifecycle vector because setup/teardown have editor-specific ordering,
		// but frame execution is scheduler-owned rather than manually duplicated.
		systemScheduler.RegisterSystem(*editorSystem, editorSystem->SchedulerDeclaration());
	}

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
	std::optional<TerraEngine::Runtime::PackagedPhysicsRuntime> packagedPhysics;
	try {
		resourceManager.Initialize();
		resourceManagerInitialized = true;
		sceneManager.SetSceneRoot(resourceManager.GetAssetRoot());
		sceneManager.SetStartupScene(resourceManager.GetConfiguredStartupScene().value_or(std::filesystem::path{}));
		// Exported physics content is validated and copied from the mounted archive
		// before any system initializes. This prevents a corrupt collider, material,
		// managed binding, or provider revision from creating a live Jolt world.
		if(resourceManager.IsExportRuntime() && ContentMount::Instance().IsMounted()) {
			// The launcher already configured the NativeAOT library from its
			// executable-relative Managed directory. Keep this early packaged-content
			// validation on the same platform-specific ABI filename so headless Linux
			// startup never falls back to a Windows-only extension.
#ifdef _WIN32
			const std::filesystem::path managedRuntimePath = resourceManager.GetEngineRoot()
				/ "Managed" / "Assembly-CSharp.dll";
#else
			const std::filesystem::path managedRuntimePath = resourceManager.GetEngineRoot()
				/ "Managed" / "Assembly-CSharp.so";
#endif
			packagedPhysics = TerraEngine::Runtime::LoadPackagedPhysicsRuntime(
				ContentMount::Instance(), true,
				managedRuntimePath);
			if(packagedPhysics->declared && !packagedPhysics->loaded) {
				throw std::runtime_error(packagedPhysics->diagnostic);
			}
			if(packagedPhysics->declared && !physicsSystem.QueueSettingsUpdate(packagedPhysics->settings)) {
				throw std::runtime_error("Packaged Physics Settings could not be queued before provider initialization.");
			}
			if(packagedPhysics->loaded) {
				// The loader already verified every mounted payload and GUID. Register
				// their logical paths before PhysicsSystem captures the startup scene so
				// normal asset resolution can read them through ContentMount.
				for(const auto& material : packagedPhysics->materials) {
					resourceManager.RegisterAssetPath(material.assetId, material.packagePath, AssetType::PhysicsMaterial);
				}
				for(const auto& collider : packagedPhysics->colliders) {
					resourceManager.RegisterAssetPath(collider.assetId, collider.packagePath, AssetType::CookedCollider);
				}
			}
		}

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
	startupInput.packagedPhysics = std::move(packagedPhysics);
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
	// A profiler session may be armed before Play Mode begins. Sample only frames
	// whose runtime world was active at frame start; editor-preview updates never
	// enter game performance captures.
	TerraEngine::Profiling::Profiler& profiler = TerraEngine::Profiling::Profiler::Instance();
	profiler.BeginLatestEngineFrameTiming();
	const bool playModeFrame = ScriptingSystem::Instance().IsRunning();
	profiler.BeginFrame(playModeFrame);
	{
		TERRAENGINE_PROFILE_SCOPE("Engine::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Runtime time-scale requests are published once before the scheduler builds
		// immutable frame workloads. Every system therefore receives the same
		// scaled delta, while Time.UnscaledDeltaTime retains the host frame value.
		const float scheduledDeltaTime = ScriptingSystem::Instance().BeginFrameTiming(deltaTime);

		// Real systems publish ECS query graph summaries as they execute. Clear
		// the frame collector here so scheduler diagnostics piggyback on actual
		// system scans instead of running a duplicate global diagnostic pass.
		const bool ecsGraphDiagnosticsEnabled = TerraEngine::Scheduling::EcsGraphFrameDiagnosticsEnabled();
		if (ecsGraphDiagnosticsEnabled)
		{
			TerraEngine::Scheduling::ClearPublishedEcsGraphFrameSummaries();
		}

		const TerraEngine::Scheduling::ScheduleSummary& schedulerSummary = systemScheduler.ExecuteFrame(scheduledDeltaTime, [](const TerraEngine::Scheduling::MutationFlushPoint& flushPoint) {
			TerraEngine::Scheduling::MutationFlushRuntimeState state;
			TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			TerraEngine::Scene::SceneMutationBatchResult result;
			const bool sameFramePhysicsConsumer = TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive();
			const bool deferredAfterPhysics = flushPoint.name == "After Physics" && !sameFramePhysicsConsumer;
			const bool spatialOnlyBoundary = flushPoint.name == "Before Physics";
			if (!deferredAfterPhysics && !spatialOnlyBoundary && !queue.Empty()) {
				// Structural scene mutations are applied only at declared flush
				// points so systems that iterate hierarchy/component data never
				// observe the ObjectManager changing midway through their update.
				result = SceneManager::Instance().ApplySceneMutations("system-scheduler-flush");
				state.message = result.applied ? "Scene mutation queue flushed." : "Scene mutation queue rejected one or more requests.";
			}
			if (spatialOnlyBoundary || flushPoint.name == "Frame End") {
				// The Before Physics edge publishes only explicitly urgent spatial
				// candidates. Ordinary candidates retain the higher-throughput Frame End edge.
				TerraEngine::Scene::WorldSpatialSystem::Instance().PublishPreparedAtProtectedBoundary(
					ObjectManager::instance(), flushPoint.name);
			}
			if (flushPoint.name == "Frame End") {
				// Scene metadata, prefab lifecycle, and explicit resource-release
				// requests from managed callbacks publish only at this protected edge.
				FlushManagedRuntimeRequests();
			}
			const bool physicsPublicationBoundary = (flushPoint.name == "After Physics" && sameFramePhysicsConsumer)
				|| (flushPoint.name == "Frame End" && !sameFramePhysicsConsumer);
			if (physicsPublicationBoundary) {
				// A physics step becomes publication-complete only after this protected
				// boundary returns the receipts for its typed Transform transactions.
				TerraEngine::Physics::PhysicsSystem::Instance().OnAfterPhysicsFlush(
					result.transactionResults, result.publicationGroupResults);
				// Managed subscribers observe the immutable packet only after physics
				// transform transactions are visible and provider callbacks have ended.
				ScriptingSystem::Instance().DeliverPhysicsEventsAfterFlush();
			}
			state.pendingAfterFlush = queue.Size();
			return state;
		});
		if (!schedulerSummary.valid) {
			TERRAENGINE_LOG(LogLevel::Error, schedulerSummary.formatted);
		}
		{
			// Reuse the frame plan that actually executed. Revalidating the same
			// declarations here would add per-frame overhead and could report a
			// different coverage story than the work the scheduler just ran.
			latestParallelFramePlan = systemScheduler.LatestFrameExecutionPlan();
			latestWorkerCoverageReport = schedulerSummary.workerCoverageReport;
			latestArchitectureSummary = latestParallelFramePlan.migrationSummary;
			const auto& runtimeMembership = ObjectManager::instance().GetRuntimeActiveMembershipSnapshot();
			latestRuntimeLayoutDiagnostics = {};
			latestRuntimeLayoutDiagnostics.totalObjectCount = runtimeMembership.size();
			for (const TerraEngine::Scene::RuntimeActiveMembership& membership : runtimeMembership)
			{
				if (membership.objectActive)
				{
					++latestRuntimeLayoutDiagnostics.activeObjectCount;
				}
			}
			latestRuntimeLayoutDiagnostics.smallSceneGuardTriggered = latestRuntimeLayoutDiagnostics.totalObjectCount <= 8u;
		}
		if (ecsGraphDiagnosticsEnabled)
		{
			systemScheduler.SetLatestComponentQueryGroups(TerraEngine::Scheduling::PublishedEcsComponentQueryGroups());
			systemScheduler.SetLatestEcsGraphSummary(TerraEngine::Scheduling::FormatPublishedEcsGraphFrameSummaries());
		}
	}
	profiler.PublishLatestEngineFrameTiming();
	if (playModeFrame)
	{
		profiler.EndFrame();
	}

	// Subsystems own the interpretation of their terminal states. MainEngine
	// only coordinates the common lifecycle decision after all frame work and
	// profiling boundaries have completed.
	for (const SystemsBase* system : systems) {
		if (system != nullptr && system->RequestsEngineShutdown()) {
			running = false;
			return false;
		}
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

TerraEngine::Runtime::AnimationParityReport Engine::RunAnimationParityFixture(
	const TerraEngine::Runtime::AnimationParityValidationOptions& options)
{
	TerraEngine::Runtime::AnimationParityReport report;
	report.requestedTicks = options.tickCount;
	report.fixedDeltaSeconds = options.fixedDeltaSeconds;
	report.mode = options.mode;
	report.visualCapture = {
		{"required", options.requireRenderedVisualCapture},
		{"available", false}
	};
	if (options.tickCount == 0U || !std::isfinite(options.fixedDeltaSeconds) || options.fixedDeltaSeconds <= 0.0F) {
		report.diagnostic = "Animation parity requires a positive fixed tick configuration.";
		return report;
	}
	// Export startup resolves animation payloads asynchronously. The first
	// scheduler update may therefore publish the deliberately safe placeholder
	// while the skeleton and clip finish loading. Give that normal dependency
	// transition a bounded window, but start the requested parity sequence only
	// after a complete sealed pose exists. A missing or corrupt dependency still
	// fails closed once this limit is exhausted.
	constexpr std::uint32_t kMaximumStartupWarmupTicks = 120U;
	bool capturedFirstAuthoritativeTick = false;
	for (std::uint32_t warmupTick = 0U;
		warmupTick < kMaximumStartupWarmupTicks;
		++warmupTick) {
		if (!Update(options.fixedDeltaSeconds)) {
			report.diagnostic = "The scheduler stopped during animation parity validation.";
			return report;
		}
		if (TerraEngine::Runtime::CaptureAnimationParityTick(
			TerraEngine::Animation::AnimationSystem::Instance(), 0U, report)) {
			report.diagnostic.clear();
			capturedFirstAuthoritativeTick = true;
			break;
		}
	}
	if (!capturedFirstAuthoritativeTick) {
		report.ticks.clear();
		report.ticksEvaluated = 0U;
		report.diagnostic =
			"The startup scene did not publish a complete sealed animator pose within "
			"120 fixed warm-up ticks.";
		return report;
	}
	for (std::uint32_t tick = 1U; tick < options.tickCount; ++tick) {
		if (!Update(options.fixedDeltaSeconds)) {
			report.diagnostic = "The scheduler stopped during animation parity validation.";
			return report;
		}
		if (!TerraEngine::Runtime::CaptureAnimationParityTick(
			TerraEngine::Animation::AnimationSystem::Instance(), tick, report)) return report;
	}
	if (!report.ticks.empty()) report.animators = report.ticks.back().at("animators");
	if (options.requireRenderedVisualCapture) {
		// A rendered lane is only accepted when the installed Vulkan backend
		// returns real pixels. The deterministic one-pixel fallback is useful for
		// API availability tests but cannot prove exported visual deformation.
		if (headlessPhysicsValidation) {
			report.diagnostic = "Rendered visual capture was requested from a headless runtime.";
			return report;
		}
		RenderSystem* renderSystem = GetSystem<RenderSystem>();
		if (renderSystem == nullptr) {
			report.diagnostic = "The rendered animation parity lane has no RenderSystem.";
			return report;
		}
		TerraEngine::Renderer::RenderCaptureRequest request;
		request.requestId = "export-animation-parity-game-view";
		request.sourceKind = TerraEngine::Renderer::RenderCaptureSourceKind::GameView;
		request.viewType = RenderViewType::Game;
		request.sourceName = "Export animation parity Game View";
		request.targetSize = glm::uvec2(1280U, 720U);
		request.format = "image/png";
		// Ask RenderSystem for one dedicated frame from the latest authored
		// runtime camera. Packaged builds render their visible window through the
		// swapchain, while validation needs a transfer-readable offscreen target
		// that contains the same immutable camera and animation generation.
		request.freshness = "nextFrame";
		request.includeOverlays = false;
		request.allowBackendFallback = false;
		request.timeoutMs = 10000U;
		const TerraEngine::Renderer::RenderCaptureResult capture =
			renderSystem->CaptureImage(request);
		report.visualCapture = {
			{"required", true},
			{"available", capture.ok},
			{"backendReadback", capture.backendReadback},
			{"deterministicFallback", capture.deterministicFallback},
			{"mimeType", capture.mimeType},
			{"width", capture.size.x},
			{"height", capture.size.y},
			{"frameId", capture.frameId},
			{"message", capture.message},
			{"errorCategory", capture.errorCategory},
			{"pngBase64", capture.base64Data}
		};
		if (!capture.ok || !capture.backendReadback || capture.deterministicFallback
			|| capture.mimeType != "image/png" || capture.size.x == 0U
			|| capture.size.y == 0U || capture.base64Data.empty()) {
			report.diagnostic =
				"The rendered animation parity lane did not produce a real Vulkan PNG readback.";
			return report;
		}
	}
	report.succeeded = true;
	return report;
}

TerraEngine::Runtime::PhysicsInvariantReport Engine::RunPhysicsInvariantFixture()
{
	using namespace TerraEngine;
	using namespace TerraEngine::Physics;
	Runtime::PhysicsInvariantReport report;
	PhysicsSystem& physics = PhysicsSystem::Instance();
	ScriptingSystem& scripting = ScriptingSystem::Instance();
	SceneManager& scene = SceneManager::Instance();

	// The fixture must be a real activated scene. This keeps both editor and
	// exported validation on the same Serializer/SceneManager component path.
	if (scene.GetActiveScene() == nullptr) {
		report.diagnostic = "Physics parity requires an activated serialized startup scene.";
		return report;
	}

		const auto executeFrame = [this]() {
		return systemScheduler.ExecuteFrame(0.0F, [](const TerraEngine::Scheduling::MutationFlushPoint& flushPoint) {
			TerraEngine::Scheduling::MutationFlushRuntimeState state;
			TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			TerraEngine::Scene::SceneMutationBatchResult result;
			const bool sameFramePhysicsConsumer = TerraEngine::Scheduling::SameFramePhysicsPublicationConsumerActive();
			const bool deferredAfterPhysics = flushPoint.name == "After Physics" && !sameFramePhysicsConsumer;
			const bool spatialOnlyBoundary = flushPoint.name == "Before Physics";
			if (!deferredAfterPhysics && !spatialOnlyBoundary && !queue.Empty()) {
				result = SceneManager::Instance().ApplySceneMutations("physics-parity-scheduler-flush");
				state.message = result.applied ? "Scene mutation queue flushed." : "Scene mutation queue rejected one or more requests.";
			}
			if (spatialOnlyBoundary || flushPoint.name == "Frame End") {
				// Headless parity exercises the exact same protected spatial publication
				// contract as the interactive scheduler path.
				TerraEngine::Scene::WorldSpatialSystem::Instance().PublishPreparedAtProtectedBoundary(
					ObjectManager::instance(), flushPoint.name);
			}
			const bool physicsPublicationBoundary = (flushPoint.name == "After Physics" && sameFramePhysicsConsumer)
				|| (flushPoint.name == "Frame End" && !sameFramePhysicsConsumer);
			if (physicsPublicationBoundary) {
				PhysicsSystem::Instance().OnAfterPhysicsFlush(
					result.transactionResults, result.publicationGroupResults);
				ScriptingSystem::Instance().DeliverPhysicsEventsAfterFlush();
			}
			state.pendingAfterFlush = queue.Size();
			return state;
		});
	};

	const auto stopFixture = [&]() {
		(void)physics.StopWorld();
		scripting.SetRunning(false);
		scripting.SetPaused(false);
	};
	const auto fail = [&](std::string diagnostic) {
		stopFixture();
		report.diagnostic = std::move(diagnostic);
		return report;
	};

	try {
		if (!physics.WorldHandle().IsValid() && !physics.StartWorld()) {
			return fail("The serialized parity scene could not start PhysicsSystem.");
		}
		const PhysicsSettings activeSettings = physics.ConfiguredSettings();
		// Store authored values in integer units so evidence comparison remains exact
		// across JSON parsing and avoids provider-specific float representations.
		report.gravityYMilliMetresPerSecondSquared = static_cast<std::int32_t>(std::lround(activeSettings.gravity.y * 1000.0F));
		report.fixedTimeStepMicroseconds = static_cast<std::uint32_t>(std::lround(activeSettings.fixedTimeStep * 1000000.0F));
		report.parityLayerBroadPhaseGroup = activeSettings.layers[1U].broadPhaseGroup;
		scripting.SetRunning(true);
		scripting.SetPaused(true);
		if (!physics.SetPaused(true)) return fail("The parity world could not enter paused explicit-step mode.");
		report.managedBindingsReady = true;

		const std::uint64_t publicationsBefore = physics.ExplicitStepPublicationCount();
		const PhysicsEventSubscription subscription = physics.SubscribeEvents({});
		std::uint64_t observedEvents = 0U;
		for (std::uint32_t step = 0U; step < 90U; ++step) {
			if (!physics.RequestExplicitStep()) return fail("PhysicsSystem rejected a serialized-fixture explicit step.");
			scripting.StepExplicit(physics.FixedTimeStep(), 1);
			const auto& summary = executeFrame();
			if (!summary.valid) return fail("The scheduler rejected a serialized-fixture physics frame.");
			if (const auto events = physics.LatestEventPacket(); events && events->events) {
				observedEvents += events->events->size();
			}
		}

		const auto bodies = physics.LatestBodySnapshot();
		const auto characters = physics.LatestCharacterStatePacket();
		const auto diagnostics = physics.LatestDiagnostics();
		if (!bodies || !bodies->bodies || !characters || !characters->states || !diagnostics) {
			std::string diagnostic = "PhysicsSystem did not publish the serialized fixture snapshots.";
			if (diagnostics) {
				diagnostic += " state=" + std::to_string(static_cast<unsigned>(diagnostics->state));
				diagnostic += " bodies=" + std::to_string(diagnostics->counters.bodyCount);
				diagnostic += " characters=" + std::to_string(diagnostics->counters.characterCount);
				diagnostic += " rejected=" + std::to_string(diagnostics->counters.rejectedCount) + ".";
			}
			if (const auto commands = physics.LatestCommandResults(); commands && commands->results
				&& !commands->results->empty()) {
				diagnostic += " Last physics result: " + commands->results->back().diagnostic;
			}
			return fail(std::move(diagnostic));
		}
		report.bodiesCreated = static_cast<std::uint32_t>(bodies->bodies->size());
		report.charactersCreated = static_cast<std::uint32_t>(characters->states->size());
		report.constraintsCreated = static_cast<std::uint32_t>(diagnostics->counters.constraintCount);
		report.fixedStepsCompleted = static_cast<std::uint32_t>(physics.ExplicitStepPublicationCount() - publicationsBefore);
		report.publishedEvents = observedEvents > 0U ? 1U : 0U;
		// The cooked ground is accepted only after PhysicsSystem has published the
		// engine-owned cache observation, not merely because four bodies exist.
		report.cookedColliderUsed = diagnostics->cache.assetCount >= 1U
			&& diagnostics->cache.publicationCount >= 1U && diagnostics->cache.payloadBytes > 0U;

		PhysicsQueryRequest query;
		query.world = physics.WorldHandle();
		query.kind = PhysicsQueryKind::RayCast;
		query.hitMode = PhysicsQueryHitMode::Closest;
		query.pose.position = {0.0F, 8.0F, 0.0F};
		query.direction = {0.0F, -1.0F, 0.0F};
		query.distance = 16.0F;
		query.maximumHits = 1U;
		query.filter.bodyTypes = PhysicsQueryBodyTypeMask::Static;
		const PhysicsQueryResult queryResult = physics.QueryImmediate(query);
		if (queryResult.status != PhysicsResultStatus::Success || !queryResult.hits || queryResult.hits->empty()) {
			return fail("The serialized fixture's static layer-filtered ray query returned no hit.");
		}
		report.filteredQueryHits = static_cast<std::uint32_t>(queryResult.hits->size());
		(void)physics.UnsubscribeEvents(subscription.id, subscription.generation);

		// Restarting the same loaded scene validates world teardown/recreation while
		// preserving the authored serialized component data in ObjectManager.
		stopFixture();
		++report.completedTransitions;
		if (!physics.StartWorld() || !physics.SetPaused(true) || !physics.RequestExplicitStep()) {
			return fail("The serialized fixture could not recreate its PhysicsSystem world.");
		}
		scripting.SetRunning(true);
		scripting.SetPaused(true);
		scripting.StepExplicit(physics.FixedTimeStep(), 1);
		if (!executeFrame().valid) return fail("The scheduler rejected the recreated parity world frame.");
		stopFixture();
		++report.completedTransitions;

		const PhysicsSystemJobDrainDiagnostics jobs = physics.JobDrainDiagnostics();
		report.activePhysicsJobsAfterDrain = jobs.activePumps;
		report.queuedPhysicsJobsAfterDrain = jobs.queued;
		report.orderlyShutdown = !physics.WorldHandle().IsValid() && jobs.activePumps == 0U && jobs.queued == 0U;
		report.succeeded = report.managedBindingsReady && report.cookedColliderUsed
			&& report.bodiesCreated == 4U && report.fixedStepsCompleted == 90U
			&& report.filteredQueryHits == 1U && report.publishedEvents == 1U
			&& report.constraintsCreated == 1U && report.charactersCreated == 1U
			&& report.completedTransitions == 2U && report.orderlyShutdown
			&& report.gravityYMilliMetresPerSecondSquared == -15500
			&& report.fixedTimeStepMicroseconds == 20000U
			&& report.parityLayerBroadPhaseGroup == 7U;
		if (!report.succeeded) report.diagnostic = "The serialized PhysicsSystem fixture did not satisfy every invariant.";
		return report;
	}
	catch (const std::exception& exception) {
		return fail(std::string("Serialized PhysicsSystem fixture failed: ") + exception.what());
	}
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
