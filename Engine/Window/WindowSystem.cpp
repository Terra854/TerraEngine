// WindowSystem.cpp — Contains the Window System system state, startup/shutdown hooks, and per-frame work implementation for window configuration, backend contracts, and high-level window system state.

#include "WindowSystem.h"

#include <stdexcept>

#include "Debug.h"
#include "Core/Logger/Logger.h"
#include "Profiling/ProfilerScope.h"

namespace
{
	constexpr const char* kWindowPollingClassification = "protected";
	constexpr const char* kWindowPollingProtectedReason = "Platform event polling, close/resize callbacks, cursor state, and swap-buffer calls remain owned by the coordinator thread.";
	constexpr const char* kWindowPollingWorkerSafeSubwork = "Workers may consume immutable input/window snapshots after polling.";
}

WindowSystem& WindowSystem::Instance()
{
	// Provide a single shared instance so the engine stack can reference the
	// window system without heap allocations or duplicate ownership.
	static WindowSystem instance;
	return instance;
}

// WindowSystem::Configure is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WindowSystem::Configure(std::unique_ptr<IWindowBackend> backendInstance, WindowConfig config, std::function<void()> shutdownCallback)
{
	// Store the dependencies up front so Initialize can safely boot the backend
	// and the engine can reuse the singleton across runs without stale state.
	backend = std::move(backendInstance);
	windowConfig = std::move(config);
	onShutdownRequested = std::move(shutdownCallback);
	configured = true;
	initialized = false;
	pollingDiagnostics = {};
	pollingDiagnostics.classification = kWindowPollingClassification;
	pollingDiagnostics.protectedReason = kWindowPollingProtectedReason;
	pollingDiagnostics.workerSafeSubwork = kWindowPollingWorkerSafeSubwork;
	latestWorkerSnapshot = {};
	latestWorkerSnapshot.width = windowConfig.width;
	latestWorkerSnapshot.height = windowConfig.height;
	latestWorkerSnapshot.resizable = windowConfig.resizable;
	latestWorkerSnapshot.title = windowConfig.title;
	latestWorkerSnapshot.boundarySummary = pollingDiagnostics.workerSafeSubwork;
	// Debug log makes backend selection visible before initialization runs.
	TERRAENGINE_LOG(LogLevel::Debug, "WindowSystem configured with backend " << (backend ? backend->BackendName() : "null"));
}

// WindowSystem::GetNativeHandle is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void* WindowSystem::GetNativeHandle() const
{
	if (!backend) {
		return nullptr;
	}
	
	return backend->GetNativeHandle();
}

// WindowSystem::SetResizeCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WindowSystem::SetResizeCallback(std::function<void(int, int)> callback)
{
	if (!backend) return;
	backend->SetResizeCallback(std::move(callback));
}

// WindowSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WindowSystem::Initialize()
{
	if (!configured) {
		throw std::runtime_error("WindowSystem must be configured before initialization.");
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!backend) {
		throw std::runtime_error("WindowSystem requires a valid backend.");
	}
	
	// Route the backend close request back to the engine so the main loop can
	// exit cleanly.
	backend->SetCloseCallback([this]() {
		if (onShutdownRequested) {
			onShutdownRequested();
		}
	});
	
	backend->Initialize(windowConfig);
	initialized = true;

	// Let consumers know that window creation succeeded and which backend drove it.
	TERRAENGINE_LOG(LogLevel::Info, "WindowSystem initialized using backend: " << backend->BackendName());
}

// WindowSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WindowSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}

	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Poll Events", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		backend->PollEvents();
	}
	bool closeRequested = false;
	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Publish Worker Snapshot", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		// Platform polling and buffer swaps remain protected because they execute
		// backend callbacks and OS-facing side effects. The descriptive strings
		// are initialized during Configure so steady frames do not allocate.
		pollingDiagnostics.pollRevision += 1;
		pollingDiagnostics.lastPollRan = true;
		closeRequested = backend->ShouldClose();
		latestWorkerSnapshot.frameRevision = pollingDiagnostics.pollRevision;
		latestWorkerSnapshot.width = windowConfig.width;
		latestWorkerSnapshot.height = windowConfig.height;
		latestWorkerSnapshot.resizable = windowConfig.resizable;
		latestWorkerSnapshot.shouldClose = closeRequested;
		latestWorkerSnapshot.workerReadable = true;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (closeRequested) {
		// Respect the request from the OS/user to close the application.
		TERRAENGINE_LOG(LogLevel::Debug, "Window backend " << backend->BackendName() << " requested application shutdown");
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (onShutdownRequested) {
			onShutdownRequested();
		}
	}
	
	{
		TERRAENGINE_PROFILE_SCOPE("WindowSystem: Swap Buffers", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		backend->SwapBuffers();
	}
	pollingDiagnostics.lastSwapRan = true;
}

// WindowSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WindowSystem::Shutdown()
{
	if (!backend) {
		return;
	}
	
	backend->Shutdown();
	initialized = false;
	latestWorkerSnapshot.workerReadable = false;

	TERRAENGINE_LOG(LogLevel::Info, "WindowSystem shutdown complete (backend " << backend->BackendName() << ").");
}

// WindowSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string WindowSystem::SystemName()
{
	return "WindowSystem";
}

WindowSystem::WindowPollingBoundaryDiagnostics WindowSystem::GetPollingBoundaryDiagnostics() const
{
	return pollingDiagnostics;
}

WindowSystem::WindowWorkerSnapshot WindowSystem::GetLatestWorkerSnapshot() const
{
	return latestWorkerSnapshot;
}
