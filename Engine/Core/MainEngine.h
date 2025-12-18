#pragma once
#include <iostream>
#include <vector>

#ifdef _WIN32
// Ensure Windows.h doesn't inject the legacy min/max macros that break RTTR's
// std::numeric_limits usage when building the engine on MSVC.
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#endif

#include "Export.h"
#include "SystemBase.h"
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

		template <typename T>
		T* GetSystem() {
			for (auto* system : systems) {
				if (T* typedSystem = dynamic_cast<T*>(system)) {
					return typedSystem;
				}
			}
			return nullptr;
		}

	private:
		bool running = true;
		// Non-owning references to systems stored as singletons elsewhere in the
		// engine so the main loop can iterate them without heap allocations.
		std::vector<SystemsBase*> systems;
		FrameTimer frameTimer;

		SystemsBase* editorSystem = nullptr;
		bool editorMode = false;

		ObjectManager& objManager = ObjectManager::instance();
		ThreadPool& pool = ThreadPool::instance();
};

