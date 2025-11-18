#pragma once
#include <iostream>
#include <memory>
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

// Primary engine facade exposed to the launcher and other executables. The
// TERRAENGINE_API macro ensures the class is exported from the shared library
// on Windows builds so the import library is generated correctly.
class TERRAENGINE_API Engine {
public:
	void Run();
	void SignalShutdown();

private:
	void Initialize();
	void MainEngineLoop();
	void Shutdown();

	bool running = true;
	std::vector<std::unique_ptr<SystemsBase>> systems;

	ObjectManager& objManager = ObjectManager::instance();
	ThreadPool& pool = ThreadPool::instance();
};

