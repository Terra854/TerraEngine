#include "MainEngine.h"
#include "SystemBase.h"
#include "Logger/Logger.h"
#include "Window/WindowSystem.h"

// Example system for testing
class TestSystem : public SystemsBase {
public:
	void Initialize() override {
		// Log system initialization so startup ordering is traceable.
		TERRAENGINE_LOG(LogLevel::Info, "Test System initialized.");
	}

	void Update() override {
		// Note when the system tick runs for visibility during the main loop.
		TERRAENGINE_LOG(LogLevel::Debug, "Test System update.");
	}

	void shutdown() override {
		// Signal that the test system cleaned up.
		TERRAENGINE_LOG(LogLevel::Info, "Test System shutdown.");
	}

	std::string SystemName() override {
		return "TestSystem";
	}
};

void Engine::Run()
{
	Initialize();

	MainEngineLoop();
	
	Shutdown();
}

void Engine::SignalShutdown()
{
	running = false;
}

void Engine::Initialize()
{
        pool.initialize();
        systems.push_back(std::make_unique<TestSystem>());

        // The window backend is created through the factory so the engine stays
        // decoupled from GLFW headers. Swapping the backend only requires
        // changing the factory implementation.
        windowSystem = CreateDefaultWindowSystem();
        WindowConfig windowConfig{};
        windowSystem->Initialize(windowConfig, [this]() {
                // Honor user-initiated close requests by routing them through the
                // standard shutdown path.
                SignalShutdown();
        });

        for (auto& system : systems) {
                system->Initialize();
        }

	// Confirm engine initialization finished so later logs have a baseline.
	TERRAENGINE_LOG(LogLevel::Info, "Setup complete!");
}

void Engine::MainEngineLoop()
{
        // Reset the timer before entering the main loop so delta calculations stay
        // consistent even after restarting the engine.
        frameTimer.Start();
        running = true;

        // Legacy ad-hoc object/component experiments and thread-pool demos are
        // intentionally disabled to keep the runtime loop focused on system
        // updates and platform event processing.
        while (running) {
                const double deltaSeconds = frameTimer.Mark();
                (void)deltaSeconds; // Placeholder until systems consume timing information.

                if (windowSystem) {
                        windowSystem->PollEvents();

                        // If the window backend reports a closure request, trigger the
                        // standard shutdown routine rather than directly exiting.
                        if (windowSystem->ShouldClose()) {
                                SignalShutdown();
                        }
                }

                if (!running) {
                        // Exit immediately when shutdown is signaled so no further systems
                        // run after the request.
                        break;
                }

                for (auto& system : systems) {
                        system->Update();
                }

                if (windowSystem) {
                        windowSystem->SwapBuffers();
                }
        }
}

void Engine::Shutdown()
{
        for (auto& system : systems) {
                system->shutdown();
        }

        if (windowSystem) {
                windowSystem->Shutdown();
                windowSystem.reset();
        }

        pool.shutdown();

	// Notify that the engine shutdown path completed and logging can be stopped soon.
	TERRAENGINE_LOG(LogLevel::Info, "Shutdown complete!");

	// Stop the logger thread so queued messages flush before the application exits.
	Logger::Instance().Shutdown();
}
