#include "MainEngine.h"
#include "SystemBase.h"
#include "Object/Transform.h"

#include <chrono>
#include <stdexcept>

// Example system for testing
class TestSystem : public SystemsBase {
public:
	void Initialize() override {
		std::cout << "Test System initialized.\n";
	}

	void Update() override {
		std::cout << "Test System update.\n";
	}

	void shutdown() override {
		std::cout << "Test System shutdown.\n";
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

	std::cout << "Setup complete!\n";
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
	std::cout << "Another task executed\n";
}

void Engine::MainEngineLoop()
{
	/*
	while (running)
	{
		std::cout << "Engine loop!\n";
	}
	*/

	Object obj1 = objManager.createObject("Object1");
	obj1.addComponent<Transform>();

	Transform* transform = obj1.getComponent<Transform>();

	transform->x = 90.0f;
	transform->y = 180.0f;
	transform->z = 270.0f;

	Transform* transform2 = obj1.getComponent<Transform>();

	obj1.removeComponent<Transform>();

	objManager.removeObject(obj1);

	Transform* transform3 = obj1.getComponent<Transform>();

	
	/* START TEST THREADPOOL */

	// Create a task that takes parameters and returns a value
	auto taskA = pool.createTask([](int a, int b) -> int {
		std::cout << "Task A executed with parameters: " << a << ", " << b << std::endl;
		return a + b;
		}, 5, 10);

	// Get the future by downcasting to the appropriate Task type
	auto futureA = std::static_pointer_cast<Task<int>>(taskA)->getFuture();

	// Create taskB but defer the use of the result from taskA
	auto taskB = pool.createTask([&futureA]() mutable {
		int result = futureA.get(); // This will block only within the task, not before enqueuing
		std::cout << "Task B executed with result: " << result << std::endl;
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
		std::cout << "Caught exception: " << e.what() << std::endl;
	}

	// Wait for all tasks to complete
	for (auto& task : tasks) {
		std::static_pointer_cast<Task<void>>(task)->getFuture().get();
	}

	std::cout << "Counter value after all tasks: " << counter << std::endl; // Expecting 10

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

        std::cout << "Shutdown complete!\n";
}
