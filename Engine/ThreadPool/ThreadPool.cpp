#include "ThreadPool.h"
#include "Core/Logger/Logger.h"

ThreadPool& ThreadPool::instance() {
	static ThreadPool instance;
	return instance;
}

void ThreadPool::initialize() {
    const size_t hardwareThreads = std::thread::hardware_concurrency();

    // specific hardware threads or default to 4 if detection fails.
    // Ensure at least one worker is created to process tasks.
    const size_t numThreads = hardwareThreads > 0 ? hardwareThreads : 4;

	for (size_t i = 0; i < numThreads; ++i) {
		workers.emplace_back([this, i] { this->workerThread((int)i); });
	}
}

void ThreadPool::shutdown()
{
	stop = true;

	condition.notify_all();
	for (std::thread& worker : workers) {
		worker.join();
	}

    // Clean up task pool
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        for (auto* task : voidTaskPool) {
            delete task;
        }
        voidTaskPool.clear();
    }
}

// Enqueue a task; handle its dependencies automatically
void ThreadPool::enqueue(std::shared_ptr<ITask> task) {
	if (task->unresolvedDependencies == -1) {
		task->unresolvedDependencies = static_cast<int>(task->dependenciesList.size());
	}

	if (task->unresolvedDependencies == 0) {
		{
			// Lock the queue mutex to ensure exclusive access to the task queue
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				tasks.emplace([task]() {
					task->run();
				});
			} // Release the lock asap when done to minimize the chances of deadlocks

			this->completeTask(task);  // Call the completion logic after task execution
		}
		condition.notify_one();  // Notify a worker thread that a task is available
	}
	else {
		for (auto& dependentTask : task->dependenciesList) {
			enqueue(dependentTask);
		}
	}
}

std::shared_ptr<Task<void>> ThreadPool::AllocateVoidTask(std::function<void()> func)
{
    Task<void>* ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        if (!voidTaskPool.empty()) {
            ptr = voidTaskPool.back();
            voidTaskPool.pop_back();
        }
    }

    if (!ptr) {
        ptr = new Task<void>(std::move(func));
    } else {
        ptr->reset(std::move(func));
    }

    // Return shared_ptr with custom deleter that returns to pool
    return std::shared_ptr<Task<void>>(ptr, [this](Task<void>* t) {
        ReleaseVoidTask(t);
    });
}

void ThreadPool::ReleaseVoidTask(Task<void>* task)
{
    std::lock_guard<std::mutex> lock(poolMutex);
    voidTaskPool.push_back(task);
}

// Notify that a task has completed; decrement unresolvedDependencies of the parent task
void ThreadPool::completeTask(std::shared_ptr<ITask> completedTask) {
	if (auto parent = completedTask->getParentTask().lock()) {
		// Atomically decrements the unresolvedDependencies and returns the previous value.
		// If it returns 1, it means that after the decrement, the value is 0, and the task
		// is ready to be enqueued.

		// std::memory_order_acq_rel ensures that operations on the atomic variable are performed
		// in a specific order, which is important for ensuring consistency across multiple threads.
		if (parent->unresolvedDependencies.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			// If the parent task now has no unresolved dependencies, enqueue it
			enqueue(parent);
			// Reset unresolved dependecies counter
			parent->unresolvedDependencies = -1;
		}
	}
	// Optionally handle any other post-task logic here
}

void ThreadPool::workerThread(int threadNum)
{
	// Log that the worker thread is live so we can trace thread lifecycle events.
	TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " initialized and running.");
	std::function<void()> task;
	while (true) {
		{
			std::unique_lock<std::mutex> lock(this->queue_mutex);
			this->condition.wait(lock,
				[this] { return this->stop || !this->tasks.empty(); });
			if (this->stop && this->tasks.empty()) {
				// Record that the worker is exiting after processing its last job.
				TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " shutting down.");
				return;
			}
			task = std::move(this->tasks.front());
			this->tasks.pop();
		}
		task();
	}
}
