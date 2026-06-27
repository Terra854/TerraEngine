// WindowSystem.h — Contains declarations for the Window System system state, startup/shutdown hooks, and per-frame work used by window configuration, backend contracts, and high-level window system state.

#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>

#include "Core/SystemBase.h"
#include "WindowBackend.h"
#include "WindowConfig.h"

// A simple engine system responsible for owning the platform window. The class
// collaborates with an IWindowBackend instance so GLFW usage remains isolated
// and swappable.
class WindowSystem : public SystemsBase {
public:
	static WindowSystem& Instance();

	WindowSystem(const WindowSystem&) = delete;
	WindowSystem& operator=(const WindowSystem&) = delete;
	WindowSystem(WindowSystem&&) = delete;
	WindowSystem& operator=(WindowSystem&&) = delete;
	~WindowSystem() override = default;

	// Supply the backend, configuration, and shutdown callback before
	// initialization so the singleton can be reused without heap allocations.
	void Configure(std::unique_ptr<IWindowBackend> backend, WindowConfig config, std::function<void()> shutdownCallback);

	// Provide access to the native window handle so other systems (e.g., input)
	// can register callbacks without depending on a specific backend type.
	void* GetNativeHandle() const;

	void SetResizeCallback(std::function<void(int, int)> callback);

	// GetBackend is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	IWindowBackend* GetBackend() const { return backend.get(); }

	struct WindowPollingBoundaryDiagnostics
	{
		std::uint64_t pollRevision = 0;
		std::string classification;
		std::string protectedReason;
		std::string workerSafeSubwork;
		bool lastPollRan = false;
		bool lastSwapRan = false;
	};

	struct WindowWorkerSnapshot
	{
		// A snapshot is copied after protected event polling so worker systems
		// can read window state without calling the platform backend directly.
		std::uint64_t frameRevision = 0;
		int width = 0;
		int height = 0;
		bool resizable = true;
		bool shouldClose = false;
		bool workerReadable = false;
		std::string title;
		std::string boundarySummary;
	};

	[[nodiscard]] WindowPollingBoundaryDiagnostics GetPollingBoundaryDiagnostics() const;
	[[nodiscard]] WindowWorkerSnapshot GetLatestWorkerSnapshot() const;

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

private:
	WindowSystem() = default;

	WindowConfig windowConfig{};
	std::unique_ptr<IWindowBackend> backend;
	std::function<void()> onShutdownRequested;
	WindowPollingBoundaryDiagnostics pollingDiagnostics{};
	WindowWorkerSnapshot latestWorkerSnapshot{};
	bool initialized = false;
	bool configured = false;
};
