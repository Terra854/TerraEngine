#include "MainEngine.h"
#include "SystemBase.h"
#include "Object/Transform.h"
#include "Logger/Logger.h"

#include <chrono>
#include <stdexcept>

// Example system for testing
class TestSystem : public SystemsBase {
public:
        void Initialize() override {
                // Report that the example system has finished setting up.
                TERRAENGINE_LOG(LogLevel::INFO, "Test System initialized.");
        }

        void Update() override {
                // Provide a heartbeat so developers can see the system running.
                TERRAENGINE_LOG(LogLevel::DEBUG, "Test System update.");
        }

        void shutdown() override {
                // Confirm the system teardown executed without issues.
                TERRAENGINE_LOG(LogLevel::INFO, "Test System shutdown.");
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

        for (auto& system : systems) {
                system->Initialize();
        }

        // Log the end of initialization so startup regressions are easy to spot.
        TERRAENGINE_LOG(LogLevel::INFO, "Setup complete!");
}

void simpleTask(int& counter) {
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	counter++;
}

void exceptionTask() {
	throw std::runtime_error("Intentional exception for testing");
}

void anotherTask(){
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        // Indicate the auxiliary task ran to completion.
        TERRAENGINE_LOG(LogLevel::INFO, "Another task executed");
}

void Engine::MainEngineLoop()
{
	/*
        while (running)
        {
                TERRAENGINE_LOG(LogLevel::DEBUG, "Engine loop!");
        }
        */

	Object obj1 = objManager.createObject("Object1");
	obj1.addComponent<Transform>();

	Transform* transform = obj1.getComponent<Transform>();

    // Exercise the CRTP-backed Transform component by tweaking each field with
    // glm math types so RTTR can observe the updates at runtime.
    transform->position = glm::vec3(90.0f, 180.0f, 270.0f);
    transform->rotation = glm::quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
    transform->scale = glm::vec3(2.0f, 2.0f, 2.0f);

	Transform* transform2 = obj1.getComponent<Transform>();

	obj1.removeComponent<Transform>();

	objManager.removeObject(obj1);

	Transform* transform3 = obj1.getComponent<Transform>();

	
        /* START TEST THREADPOOL */

	// Create a task that takes parameters and returns a value
        auto taskA = pool.createTask([](int a, int b) -> int {
                // Capture the parameters fed into the task for traceability during debugging.
                TERRAENGINE_LOG(LogLevel::DEBUG, "Task A executed with parameters: " << a << ", " << b);
                return a + b;
                }, 5, 10);

	// Get the future by downcasting to the appropriate Task type
	auto futureA = std::static_pointer_cast<Task<int>>(taskA)->getFuture();

	// Create taskB but defer the use of the result from taskA
        auto taskB = pool.createTask([&futureA]() mutable {
                int result = futureA.get(); // This will block only within the task, not before enqueuing
                // Record the dependency result to ensure ordering works as expected.
                TERRAENGINE_LOG(LogLevel::DEBUG, "Task B executed with result: " << result);
                });

	auto taskC = pool.createTask(anotherTask);

	//taskB->addDependency(taskA);  // Task B depends on Task A
	taskB->addDependency(taskC);  // Task B depends on Task C
	taskB->addDependency(taskC);  // Task B depends on Task C

	pool.enqueue(taskB);  // Task B will execute after Task A and Task C complete
	
	// Do not enqueue taskB again, it will throw an exception as the future promise is already
	// fufilled

	// Workaround is to clone and run the cloned task
	auto cloned_task = taskB->clone();
	pool.enqueue(cloned_task);

	// Multiple tasks to increment a counter to 10
	int counter = 0;
	std::mutex counter_mutex;

	// Schedule a simple task multiple times
	std::vector<std::shared_ptr<ITask>> tasks;
	for (int i = 0; i < 10; ++i) {
		auto task = pool.createTask([&counter, &counter_mutex] {
			std::lock_guard<std::mutex> lock(counter_mutex);
			simpleTask(counter);
			});
		tasks.push_back(task);
		pool.enqueue(task);  // Enqueue the task to be executed by the thread pool
	}

	// Test error handling
        try {
                auto errorTask = pool.createTask(exceptionTask);
                pool.enqueue(errorTask);  // Enqueue the task
                std::static_pointer_cast<Task<void>>(errorTask)->getFuture().get(); // This should throw an exception
        }
        catch (const std::exception& e) {
                // Surface the intentional exception so future errors are easy to diagnose.
                TERRAENGINE_LOG(LogLevel::ERROR, "Caught exception: " << e.what());
        }

	// Wait for all tasks to complete
        for (auto& task : tasks) {
                std::static_pointer_cast<Task<void>>(task)->getFuture().get();
        }

        // Provide the final counter so concurrency regressions are obvious in logs.
        TERRAENGINE_LOG(LogLevel::INFO, "Counter value after all tasks: " << counter); // Expecting 10

	// End Multiple Tasks

	/* END TEST THREADPOOL */

        for (auto& system : systems) {
                system->Update();
        }
}

void Engine::Shutdown()
{
        for (auto& system : systems) {
                system->shutdown();
        }

        pool.shutdown();

        // Announce shutdown completion before stopping the logger thread.
        TERRAENGINE_LOG(LogLevel::INFO, "Shutdown complete!");
        Logger::Instance().Shutdown();
}
