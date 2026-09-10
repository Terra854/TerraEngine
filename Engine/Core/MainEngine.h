// MainEngine.h — Core engine facade and application lifecycle interface.
//
// This file defines the Engine class, which serves as the primary interface for
// the launcher to initialize and run the engine. It manages the collection of
// active systems, handles frame timing, and provides access to core engine
// features like the ObjectManager and ThreadPool.

#pragma once
#include <cstdint>
#include <iostream>
#include <vector>

#ifdef _WIN32
// Ensure Windows.h doesn't inject the legacy min/max macros that break
// std::numeric_limits usage when building the engine on MSVC.
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include "Export.h"
#include "SystemBase.h"
#include "Scheduling/JobGraph.h"
#include "Scheduling/FrameWorkload.h"
#include "Scheduling/SystemScheduler.h"
#include "Object/ObjectManager.h"
#include "Scene/RuntimeLayout.h"
#include "ThreadPool/ThreadPool.h"
#include "Timer.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Runtime/Validation/AnimationParityValidator.h"

// Primary engine facade exposed to the launcher and other executables. The
// TERRAENGINE_API macro ensures the class is exported from the shared library
// on Windows builds so the import library is generated correctly.
class TERRAENGINE_API Engine {
	public:
		// The engine owns singleton system references, so copying would duplicate
		// ownership semantics incorrectly. Moves stay enabled in case the engine
		// needs to be passed between scopes.
		Engine() = default;
		Engine(const Engine&) = delete;
		Engine& operator=(const Engine&) = delete;
		Engine(Engine&&) = default;
		Engine& operator=(Engine&&) = default;
		~Engine() = default;

		void Run();
		void SignalShutdown();
		void SetEditorMode(SystemsBase* system);

		void Initialize();
		bool Update(float deltaTime);
		void Shutdown();
		void ResetFrameTimer();
		// Configures a process-scoped validation session that initializes only the
		// scene, scripting, physics, resource, and scheduler path. The launcher
		// uses this before Initialize() so parity validation never creates a window
		// or render device merely to run a deterministic physics fixture.
		void ConfigureHeadlessPhysicsValidation();
		// Starts the same no-window/no-renderer lifecycle for animation export and
		// recovery validation. The existing physics name remains for compatibility;
		// CPU animation fallback is selected because RenderSystem is intentionally
		// absent from the resulting scheduler.
		void ConfigureHeadlessCpuFallback();
		// Executes the already-loaded startup scene through the same PhysicsSystem
		// and scheduler/flush boundary used by ordinary frames. Callers own normal
		// engine shutdown after this bounded validation operation returns.
		TerraEngine::Runtime::PhysicsInvariantReport RunPhysicsInvariantFixture();
		// Runs a bounded fixed-tick animation parity capture on the already initialized scene.
		TerraEngine::Runtime::AnimationParityReport RunAnimationParityFixture(
			const TerraEngine::Runtime::AnimationParityValidationOptions& options = {});

		template <typename T>
		// GetSystem is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		T* GetSystem() {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (auto* system : systems) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (T* typedSystem = dynamic_cast<T*>(system)) {
					return typedSystem;
				}
			}
			return nullptr;
		}

		// GetEditorSystem is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		SystemsBase* GetEditorSystem() { return editorSystem; }

		const TerraEngine::Scheduling::ScheduleSummary& GetSchedulerSummary() const { return systemScheduler.LatestSummary(); }
		const TerraEngine::Scheduling::JobGraphSummary& GetJobGraphSummary() const { return latestJobGraphSummary; }
		const TerraEngine::Scheduling::FrameExecutionPlan& GetParallelFrameExecutionPlan() const { return latestParallelFramePlan; }
		const TerraEngine::Scheduling::ArchitectureMigrationSummary& GetArchitectureMigrationSummary() const { return latestArchitectureSummary; }
		const TerraEngine::Scheduling::WorkerCoverageReport& GetWorkerCoverageReport() const { return latestWorkerCoverageReport; }
		const TerraEngine::Scene::LayoutDiagnosticsSummary& GetRuntimeLayoutDiagnostics() const { return latestRuntimeLayoutDiagnostics; }

	private:
		bool running = true;
		// Non-owning references to systems stored as singletons elsewhere in the
		// engine so the main loop can iterate them without heap allocations.
		std::vector<SystemsBase*> systems;
		TerraEngine::Scheduling::SystemScheduler systemScheduler;
		TerraEngine::Scheduling::JobGraphSummary latestJobGraphSummary;
		TerraEngine::Scheduling::FrameExecutionPlan latestParallelFramePlan;
		TerraEngine::Scheduling::ArchitectureMigrationSummary latestArchitectureSummary;
		TerraEngine::Scheduling::WorkerCoverageReport latestWorkerCoverageReport;
		TerraEngine::Scene::LayoutDiagnosticsSummary latestRuntimeLayoutDiagnostics;
		FrameTimer frameTimer;

		SystemsBase* editorSystem = nullptr;
		bool editorMode = false;
		bool headlessPhysicsValidation = false;
		// Track attempts separately because zero is a valid initial revision. A
		// deterministic rejection is cached until one of the protected inputs below
		// changes, preventing an unchanged editor scene from being rescanned and
		// logged every frame.
		bool navigationBootstrapAttempted = false;
		// This is the component revision examined by the latest bootstrap attempt,
		// whether that attempt queued work or found no eligible navigation geometry.
		std::uint64_t lastNavigationBootstrapComponentRevision = 0U;
		// Profile configuration selects distinct Recast/Detour shard build keys.
		// Track its copied revision beside scene components so editor/project profile
		// changes receive coverage even if no object was touched in that frame.
		std::uint64_t lastNavigationBootstrapProfileRevision = 0U;
		// Terrain collision can publish a newer physically authoritative heightfield
		// without rewriting the TerrainComponent itself. Track that generation so
		// navigation recaptures holes/heights at the next normal frame-end boundary.
		std::uint64_t lastNavigationBootstrapTerrainPublicationGeneration = 0U;

		ObjectManager& objManager = ObjectManager::instance();
		ThreadPool& pool = ThreadPool::instance();
};
