// ThreadPool.h — Contains declarations for the Thread Pool queue state, worker coordination, and synchronization helpers used by task queue storage, worker thread lifecycle, and synchronization primitives.

#pragma once
#include "Core/Export.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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

// ThreadPoolWorkerActivity describes the worker's state at the instant an
// immutable telemetry snapshot is captured. Timing counters below retain the
// completed history, while this value explains what the worker is doing now.
enum class ThreadPoolWorkerActivity : std::uint8_t {
	Starting,
	Idle,
	Running,
	Stopped
};

// ThreadPoolWorkerTelemetrySample is one completed dispatch. Queue depth at
// enqueue travels with the queued task, so the worker that eventually claims
// the task can publish both queue observations without guessing ownership.
struct ThreadPoolWorkerTelemetrySample {
	std::uint64_t sequence = 0;
	std::size_t workerIndex = 0;
	std::size_t queueDepthAtEnqueue = 0;
	std::size_t queueDepthAtStart = 0;
	std::uint64_t usefulItemCount = 0;
	std::uint64_t waitNanoseconds = 0;
	std::uint64_t runNanoseconds = 0;
	std::uint64_t idleNanoseconds = 0;
};

// ThreadPoolWorkerTelemetry contains cumulative counters and a bounded copy of
// recent dispatches for one worker. Callers receive this data only through a
// const snapshot, never through the mutable counters updated by worker threads.
struct ThreadPoolWorkerTelemetry {
	std::size_t workerIndex = 0;
	ThreadPoolWorkerActivity activity = ThreadPoolWorkerActivity::Starting;
	std::size_t latestQueueDepthAtEnqueue = 0;
	std::size_t latestQueueDepthAtStart = 0;
	std::uint64_t dispatchCount = 0;
	std::uint64_t usefulItemCount = 0;
	std::uint64_t waitNanoseconds = 0;
	std::uint64_t runNanoseconds = 0;
	std::uint64_t idleNanoseconds = 0;
	std::uint64_t droppedHistorySampleCount = 0;
	std::vector<ThreadPoolWorkerTelemetrySample> history;
};

// ThreadPoolTelemetrySnapshot is an immutable, internally consistent view of
// all worker counters. The sampling interval and history limit make omitted and
// overwritten detail explicit instead of silently presenting partial evidence.
struct ThreadPoolTelemetrySnapshot {
	std::uint64_t generation = 0;
	std::uint32_t samplingIntervalDispatches = 1;
	std::size_t historyCapacityPerWorker = 0;
	std::uint64_t droppedHistorySampleCount = 0;
	std::vector<ThreadPoolWorkerTelemetry> workers;
};

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
	static constexpr std::size_t kMaximumTelemetryHistoryCapacityPerWorker = 256;

	static TERRAENGINE_API ThreadPool& instance();
	// Independent capacity reuses the same queue implementation without changing
	// frame workers. The owner must close/drain before releasing its dependencies.
	// The optional startup checkpoint is native-only deterministic test input.
	static TERRAENGINE_API std::unique_ptr<ThreadPool> CreateOwned(
		std::size_t workerCount, std::function<void(std::size_t)> beforeWorker = {});
	TERRAENGINE_API ~ThreadPool();

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

	// requestedWorkerCount==0 preserves the historical hardware-concurrency
	// default. Explicit counts are used by deterministic performance runs.
	TERRAENGINE_API void initialize(std::size_t requestedWorkerCount = 0);
	TERRAENGINE_API void shutdown();

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
			// Owned pools must never export a raw-this reuse-cache deleter. Even an
			// accidentally retained void handle can then safely outlive that pool.
			if(ownedExecutor) return std::make_shared<Task<void>>(std::function<void()>(std::move(boundFunc)));
			return AllocateVoidTask(std::move(boundFunc));
		} else {
			return std::make_shared<Task<ReturnType>>(std::function<ReturnType()>(boundFunc));
		}
	}

	// usefulItemCount records how much caller-visible work the dispatch owns.
	// A zero value remains visible so diagnostics can identify useless dispatches.
	void enqueue(std::shared_ptr<ITask> weakTask, std::uint64_t usefulItemCount = 1);
	void completeTask(std::shared_ptr<ITask> completedTask);

	std::shared_ptr<Task<void>> AllocateVoidTask(std::function<void()> func);
	void ReleaseVoidTask(Task<void>* task);

	// getThreadCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	size_t getThreadCount() const { return workers.size(); }

	// ConfigureTelemetrySampling changes only diagnostic retention. Capacity is
	// always clamped to the public hard maximum so long sessions stay bounded.
	void ConfigureTelemetrySampling(std::uint32_t dispatchInterval, std::size_t historyCapacityPerWorker);
	void ResetTelemetry();
	[[nodiscard]] std::shared_ptr<const ThreadPoolTelemetrySnapshot> GetTelemetrySnapshot() const;
	// The profiler needs only cumulative worker counters at a frame boundary.
	// Visiting the existing records under the telemetry lock avoids allocating an
	// immutable snapshot and copying every bounded dispatch-history deque each
	// frame. The visitor must copy values only and must never retain references.
	template<typename Visitor>
	void VisitTelemetryCounters(Visitor&& visitor) const
	{
		std::lock_guard<std::mutex> lock(telemetryMutex);
		for(const auto& source : workerTelemetry)
		{
			std::invoke(visitor, source.counters);
		}
	}

private:
	ThreadPool() : stop(false) {}
	void InitializeWorkers(std::size_t requestedWorkerCount,
		const std::function<void(std::size_t)>& beforeWorker);

	void workerThread(int threadNum);
	void ResetTelemetryForWorkers(std::size_t workerCount);
	void RecordWorkerActivity(std::size_t workerIndex, ThreadPoolWorkerActivity activity, std::uint64_t idleNanoseconds = 0);
	void RecordWorkerTaskStarted(
		std::size_t workerIndex,
		std::size_t queueDepthAtEnqueue,
		std::size_t queueDepthAtStart,
		std::uint64_t usefulItemCount,
		std::uint64_t waitNanoseconds,
		std::uint64_t idleNanoseconds);
	void RecordWorkerTaskFinished(
		std::size_t workerIndex,
		std::size_t queueDepthAtEnqueue,
		std::size_t queueDepthAtStart,
		std::uint64_t usefulItemCount,
		std::uint64_t waitNanoseconds,
		std::uint64_t idleNanoseconds,
		std::uint64_t runNanoseconds);

	// QueuedTask preserves diagnostic facts captured under the queue lock. The
	// callable remains independent of telemetry and keeps existing task behavior.
	struct QueuedTask {
		std::function<void()> execute;
		std::size_t queueDepthAtEnqueue = 0;
		std::uint64_t usefulItemCount = 0;
		std::uint64_t enqueueNanoseconds = 0;
	};

	struct MutableWorkerTelemetry {
		ThreadPoolWorkerTelemetry counters;
		std::deque<ThreadPoolWorkerTelemetrySample> history;
	};

	std::vector<std::thread> workers;
	std::queue<QueuedTask> tasks;

	std::mutex queue_mutex;
	std::condition_variable condition;
	bool stop;
	bool ownedExecutor = false;
	
	std::vector<Task<void>*> voidTaskPool;
	std::mutex poolMutex;

	mutable std::mutex telemetryMutex;
	std::vector<MutableWorkerTelemetry> workerTelemetry;
	std::uint64_t telemetryGeneration = 0;
	std::uint64_t telemetrySequence = 0;
	std::uint64_t droppedTelemetryHistorySamples = 0;
	std::uint32_t telemetrySamplingIntervalDispatches = 1;
	std::size_t telemetryHistoryCapacityPerWorker = kMaximumTelemetryHistoryCapacityPerWorker;
};
