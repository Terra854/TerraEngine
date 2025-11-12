#include "ThreadPool.h"
#include <iostream>

ThreadPool& ThreadPool::instance() {
	static ThreadPool instance;
	return instance;
}

void ThreadPool::initialize() {
	for (int i = 0; i < 4; ++i)
		workers.emplace_back(
			[this, i] {
				this->workerThread(i);
			}
		);
}

void ThreadPool::shutdown()
{
	stop = true;

	condition.notify_all();
	for (std::thread& worker : workers)
		worker.join();
}

// Enqueue a task; handle its dependencies automatically
void ThreadPool::enqueue(std::shared_ptr<ITask> task) {
	if (task->unresolvedDependencies == -1)
		task->unresolvedDependencies = static_cast<int>(task->dependenciesList.size());

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
	std::cout << "Worker thread no. " << threadNum << " initialized and running." << std::endl;
	std::function<void()> task;
	while (true) {
                {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock,
                                [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) {
                                std::cout << "Worker thread no. " << threadNum << " shutting down." << std::endl;
                                return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                }
                task();
        }
}
