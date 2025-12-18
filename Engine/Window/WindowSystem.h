#pragma once

#include <functional>
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

	IWindowBackend* GetBackend() const { return backend.get(); }

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

private:
	WindowSystem() = default;

	WindowConfig windowConfig{};
	std::unique_ptr<IWindowBackend> backend;
	std::function<void()> onShutdownRequested;
	bool initialized = false;
	bool configured = false;
};

