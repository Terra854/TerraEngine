// ThreadPool.h — Contains declarations for the Thread Pool queue state, worker coordination, and synchronization helpers used by task queue storage, worker thread lifecycle, and synchronization primitives.

#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool;

// Interface for a task
// struct ITask groups related state and operations so callers have one documented contract.
struct ITask {
	std::atomic<int> unresolvedDependencies = -1;
	std::vector<std::shared_ptr<ITask>> dependenciesList;

	virtual ~ITask() = default;

	// Method to clone the task
	virtual std::shared_ptr<ITask> clone() const = 0;
	
	virtual void run() = 0;
	// Use weak_ptr instead of shared_ptr to avoid increasing reference count
	// Otherwise it will not destroy itself on exit, resulting in memory leaks
	virtual void setParentTask(std::weak_ptr<ITask> parent) = 0;
	virtual std::weak_ptr<ITask> getParentTask() = 0;
};

template <typename ReturnType>
// struct Task groups related state and operations so callers have one documented contract.
struct Task : public ITask, public std::enable_shared_from_this<Task<ReturnType>> {
	std::function<ReturnType()> taskFunc;
	std::promise<ReturnType> taskPromise;
	std::weak_ptr<ITask> parentTask; // Weak pointer to the parent task to avoid increasing reference count

	Task(std::function<ReturnType()> func)
		// ITask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		: ITask(), taskFunc(std::move(func)), parentTask() {}

	std::shared_ptr<ITask> clone() const override {
		//return std::make_shared<Task<ReturnType>>(taskFunc);
		auto clonedTask = std::make_shared<Task<ReturnType>>(taskFunc);

		// Clone each dependency in the dependenciesList
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& dependency : this->dependenciesList) {
			auto clonedDependency = dependency->clone();
			clonedTask->dependenciesList.push_back(clonedDependency);
			clonedDependency->setParentTask(clonedTask);
		}

		return clonedTask;
	}

	// run is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void run() override {
		try {
			if constexpr (std::is_void_v<ReturnType>) {
				taskFunc();  // Execute the task
				taskPromise.set_value();  // Set value for void type
			}
			else {
				taskPromise.set_value(taskFunc());  // Execute the task and set the value
			}
		}
		catch (...) {
			taskPromise.set_exception(std::current_exception()); // Handle any exceptions
		}
	}

	// setParentTask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void setParentTask(std::weak_ptr<ITask> parent) override {
		parentTask = parent;
	}

	// getParentTask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::weak_ptr<ITask> getParentTask() override {
		return parentTask;
	}

	// Add a dependency to this task
	template<typename T>
	void addDependency(const std::shared_ptr<Task<T>>& dependency) {

		std::shared_ptr<ITask> task = dependency->clone();

		dependenciesList.push_back(task);
		task->setParentTask(this->shared_from_this());
	}

	// Delete all dependencies from this task
	// clearDependencies is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void clearDependencies() {
		dependenciesList.clear();
	}
	
	// reset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void reset(std::function<ReturnType()> func) {
		taskFunc = std::move(func);
		taskPromise = std::promise<ReturnType>();
		dependenciesList.clear();
		unresolvedDependencies = -1;
		parentTask.reset();
	}

	// getFuture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::future<ReturnType> getFuture() {
		return taskPromise.get_future();
	}
};

// class ThreadPool groups related state and operations so callers have one documented contract.
class ThreadPool {
public:
	static ThreadPool& instance();

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// requestedWorkerCount==0 preserves the historical hardware-concurrency
	// default. Explicit counts are used by deterministic performance runs.
	void initialize(std::size_t requestedWorkerCount = 0);
	void shutdown();

	// This function needs to be in the header file or else you'll get linker errors
	// Create a task with parameters and return value
	// The task is not enqueued automatically
	// Use enqueue to enqueue the task
	template<class Func, class... Args>
	auto createTask(Func&& func, Args&&... args) -> std::shared_ptr<Task<std::invoke_result_t<Func, Args...>>> {
		using ReturnType = std::invoke_result_t<Func, Args...>;
		auto boundFunc = std::bind(std::forward<Func>(func), std::forward<Args>(args)...);
		
		// constexpr is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		if constexpr (std::is_void_v<ReturnType>) {
			return AllocateVoidTask(std::move(boundFunc));
		} else {
			return std::make_shared<Task<ReturnType>>(std::function<ReturnType()>(boundFunc));
		}
	}

	void enqueue(std::shared_ptr<ITask> weakTask);
	void completeTask(std::shared_ptr<ITask> completedTask);

	std::shared_ptr<Task<void>> AllocateVoidTask(std::function<void()> func);
	void ReleaseVoidTask(Task<void>* task);

	// getThreadCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	size_t getThreadCount() const { return workers.size(); }

private:
	ThreadPool() : stop(false) {}
	~ThreadPool() {}

	void workerThread(int threadNum);

	std::vector<std::thread> workers;
	std::queue<std::function<void()>> tasks;

	std::mutex queue_mutex;
	std::condition_variable condition;
	bool stop;
	
	std::vector<Task<void>*> voidTaskPool;
	std::mutex poolMutex;
};
