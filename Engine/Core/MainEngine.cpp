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
		// Log system initialization so startup ordering is traceable.
		TERRAENGINE_LOG(LogLevel::Info, "Test System initialized.");
	}

	void Update() override {
		// Note when the system tick runs for visibility during the main loop.
		TERRAENGINE_LOG(LogLevel::Debug, "Test System update.");
	}

	void Shutdown() override {
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

	for (auto& system : systems) {
		system->Initialize();
	}

	// Confirm engine initialization finished so later logs have a baseline.
	TERRAENGINE_LOG(LogLevel::Info, "Setup complete!");
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
	// Mark execution of a background task to prove asynchronous work runs.
	TERRAENGINE_LOG(LogLevel::Debug, "Another task executed");
}

void Engine::MainEngineLoop()
{
	/*
	while (running)
	{
		TERRAENGINE_LOG(LogLevel::Debug, "Engine loop!");
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
		// Track task execution and parameters to validate the thread pool dispatch order.
		TERRAENGINE_LOG(LogLevel::Debug, "Task A executed with parameters: " << a << ", " << b);
		return a + b;
		}, 5, 10);

	// Get the future by downcasting to the appropriate Task type
	auto futureA = std::static_pointer_cast<Task<int>>(taskA)->getFuture();

	// Create taskB but defer the use of the result from taskA
	auto taskB = pool.createTask([&futureA]() mutable {
		int result = futureA.get(); // This will block only within the task, not before enqueuing
		// Show that dependent task results propagate correctly.
		TERRAENGINE_LOG(LogLevel::Debug, "Task B executed with result: " << result);
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
		// Surface the intentional error so the test scenario is easy to observe.
		TERRAENGINE_LOG(LogLevel::Error, "Caught exception: " << e.what());
	}

	// Wait for all tasks to complete
	for (auto& task : tasks) {
		std::static_pointer_cast<Task<void>>(task)->getFuture().get();
	}

	// Validate that all scheduled tasks executed by logging the final counter value.
	TERRAENGINE_LOG(LogLevel::Info, "Counter value after all tasks: " << counter); // Expecting 10

	// End Multiple Tasks

	/* END TEST THREADPOOL */

	for (auto& system : systems) {
		system->Update();
	}
}

void Engine::Shutdown()
{
	for (auto& system : systems) {
		system->Shutdown();
	}

	pool.Shutdown();

	// Notify that the engine shutdown path completed and logging can be stopped soon.
	TERRAENGINE_LOG(LogLevel::Info, "Shutdown complete!");

	// Stop the logger thread so queued messages flush before the application exits.
	Logger::Instance().Shutdown();
}
