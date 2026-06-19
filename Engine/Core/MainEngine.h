// MainEngine.h — Core engine facade and application lifecycle interface.
//
// This file defines the Engine class, which serves as the primary interface for
// the launcher to initialize and run the engine. It manages the collection of
// active systems, handles frame timing, and provides access to core engine
// features like the ObjectManager and ThreadPool.

#pragma once
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
#include "Scheduling/SystemScheduler.h"
#include "Object/ObjectManager.h"
#include "ThreadPool/ThreadPool.h"
#include "Timer.h"

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

	private:
		bool running = true;
		// Non-owning references to systems stored as singletons elsewhere in the
		// engine so the main loop can iterate them without heap allocations.
		std::vector<SystemsBase*> systems;
		TerraEngine::Scheduling::SystemScheduler systemScheduler;
		TerraEngine::Scheduling::JobGraphSummary latestJobGraphSummary;
		FrameTimer frameTimer;

		SystemsBase* editorSystem = nullptr;
		bool editorMode = false;

		ObjectManager& objManager = ObjectManager::instance();
		ThreadPool& pool = ThreadPool::instance();
};
