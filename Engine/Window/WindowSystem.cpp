#include "WindowSystem.h"

#include <stdexcept>

#include "Debug.h"
#include "Core/Logger/Logger.h"

WindowSystem& WindowSystem::Instance()
{
	// Provide a single shared instance so the engine stack can reference the
	// window system without heap allocations or duplicate ownership.
	static WindowSystem instance;
	return instance;
}

void WindowSystem::Configure(std::unique_ptr<IWindowBackend> backendInstance, WindowConfig config, std::function<void()> shutdownCallback)
{
	// Store the dependencies up front so Initialize can safely boot the backend
	// and the engine can reuse the singleton across runs without stale state.
	backend = std::move(backendInstance);
	windowConfig = std::move(config);
	onShutdownRequested = std::move(shutdownCallback);
	configured = true;
	initialized = false;
	// Debug log makes backend selection visible before initialization runs.
	TERRAENGINE_LOG(LogLevel::Debug, "WindowSystem configured with backend " << (backend ? backend->BackendName() : "null"));
}

void* WindowSystem::GetNativeHandle() const
{
	if (!backend) {
		return nullptr;
	}
	
	return backend->GetNativeHandle();
}

void WindowSystem::SetResizeCallback(std::function<void(int, int)> callback)
{
	if (!backend) return;
	backend->SetResizeCallback(std::move(callback));
}

void WindowSystem::Initialize()
{
	if (!configured) {
		throw std::runtime_error("WindowSystem must be configured before initialization.");
	}
	
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

void WindowSystem::Update(float deltaTime)
{
	if (!initialized) {
		return;
	}

	backend->PollEvents();
	
	if (backend->ShouldClose()) {
		// Respect the request from the OS/user to close the application.
		TERRAENGINE_LOG(LogLevel::Debug, "Window backend " << backend->BackendName() << " requested application shutdown");
		if (onShutdownRequested) {
			onShutdownRequested();
		}
	}
	
	backend->SwapBuffers();
}

void WindowSystem::Shutdown()
{
	if (!backend) {
		return;
	}
	
	backend->Shutdown();
	initialized = false;

	TERRAENGINE_LOG(LogLevel::Info, "WindowSystem shutdown complete (backend " << backend->BackendName() << ").");
}

std::string WindowSystem::SystemName()
{
	return "WindowSystem";
}
