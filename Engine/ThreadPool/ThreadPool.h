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
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class ThreadPool;
struct ThreadPoolTestAccess;

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

struct ITask {
	std::atomic<int> unresolvedDependencies = -1;
	std::vector<std::shared_ptr<ITask>> dependenciesList;

	virtual ~ITask() = default;

	// Method to clone the task
	virtual std::shared_ptr<ITask> clone() const = 0;
	
	virtual void run() = 0;
	// Cancel owned, unstarted work through its already-created typed promise.
	// Queue retirement calls this outside executor locks; it never runs user code.
	virtual void Fail(std::exception_ptr failure) noexcept = 0;
	// Use weak_ptr instead of shared_ptr to avoid increasing reference count
	// Otherwise it will not destroy itself on exit, resulting in memory leaks
	virtual void setParentTask(std::weak_ptr<ITask> parent) = 0;
	virtual std::weak_ptr<ITask> getParentTask() = 0;

protected:
	friend class ThreadPool;
	enum class ExecutionState { Prepared, WaitingDependencies, Ready, Queued, Running, Terminal };
	// One mutex decides readiness, terminal publication ownership, and notification.
	// Callables, promise publication, and captured-owner destruction happen unlocked.
	std::mutex settlementMutex;
	ExecutionState executionState = ExecutionState::Prepared;
	std::size_t remainingDependencies = 0;
	std::exception_ptr infrastructureFailure;
	bool terminalProducerClaimed = false;
	bool parentNotificationDelivered = false;
};

template <typename ReturnType>

struct Task : public ITask, public std::enable_shared_from_this<Task<ReturnType>> {
	std::function<ReturnType()> taskFunc;
	std::promise<ReturnType> taskPromise;
	std::weak_ptr<ITask> parentTask; // Weak pointer to the parent task to avoid increasing reference count

	Task(std::function<ReturnType()> func)

		: ITask(), taskFunc(std::move(func)), parentTask() {}

	std::shared_ptr<ITask> clone() const override {
		//return std::make_shared<Task<ReturnType>>(taskFunc);
		auto clonedTask = std::make_shared<Task<ReturnType>>(taskFunc);

		// Clone each dependency in the dependenciesList

		for (const auto& dependency : this->dependenciesList) {
			auto clonedDependency = dependency->clone();
			clonedTask->dependenciesList.push_back(clonedDependency);
			clonedDependency->setParentTask(clonedTask);
		}

		return clonedTask;
	}


	void run() override {
		{
			std::lock_guard lock(this->settlementMutex);
			if(this->terminalProducerClaimed) std::terminate();
			this->terminalProducerClaimed = true;
			this->executionState = ITask::ExecutionState::Running;
		}
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
			// A callable exception belongs only to this task's existing result
			// channel. It does not cancel dependent ancestors as infrastructure does.
			try { taskPromise.set_exception(std::current_exception()); }
			catch(...) { std::terminate(); }
		}
		std::lock_guard lock(this->settlementMutex);
		this->executionState = ITask::ExecutionState::Terminal;
	}

	void Fail(std::exception_ptr failure) noexcept override {
		try {
			{
				std::lock_guard lock(this->settlementMutex);
				if(this->terminalProducerClaimed) std::terminate();
				this->terminalProducerClaimed = true;
				if(!this->infrastructureFailure) this->infrastructureFailure = failure;
				failure = this->infrastructureFailure;
				this->executionState = ITask::ExecutionState::Terminal;
			}
			// Reuse the existing promise state and exception pointer. No replacement
			// promise, callback, diagnostic, or container is allocated during failure.
			taskPromise.set_exception(std::move(failure));
		} catch(...) { std::terminate(); }
	}


	void setParentTask(std::weak_ptr<ITask> parent) override {
		parentTask = parent;
	}


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

	void clearDependencies() {
		dependenciesList.clear();
	}
	

	void reset(std::function<ReturnType()> func) {
		// Promise allocation may fail. Preserve the old callable, dependencies,
		// and state until replacement storage exists; callers own this exclusively.
		std::promise<ReturnType> replacement;
		taskFunc = std::move(func);
		taskPromise = std::move(replacement);
		dependenciesList.clear();
		unresolvedDependencies = -1;
		parentTask.reset();
		// reset is exclusive: retire old exception ownership outside state/cache
		// locks, then establish a fresh execution before exporting another owner.
		infrastructureFailure = {};
		remainingDependencies = 0;
		executionState = ITask::ExecutionState::Prepared;
		terminalProducerClaimed = false;
		parentNotificationDelivered = false;
	}


	std::future<ReturnType> getFuture() {
		return taskPromise.get_future();
	}
};


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
	void completeTask(std::shared_ptr<ITask> completedTask) noexcept;

	// Native validation configures this only at a quiescent boundary. The exact
	// parent remains caller-owned until drain; the pre-created exception lets a
	// final-dependency admission failure be tested without global allocation faults.
	TERRAENGINE_API void SetDependencyAdmissionFailureForTesting(
		const ITask* target, std::exception_ptr failure);
	[[nodiscard]] std::size_t DependencyAdmissionFailureCountForTesting() const noexcept {
		return admissionFailureCount.load(std::memory_order_acquire);
	}

	std::shared_ptr<Task<void>> AllocateVoidTask(std::function<void()> func);
	void ReleaseVoidTask(Task<void>* task) noexcept;


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
	// The native test friend stages only a quiescent unstarted queue. It adds no
	// runtime control surface or conditional production admission behavior.
	friend struct ThreadPoolTestAccess;
	ThreadPool() : stop(false), shutdownFailure(std::make_exception_ptr(
		std::runtime_error("ThreadPool shut down before queued task execution"))) {}
	void ReleaseVoidTask(Task<void>* task, std::uint64_t allocationGeneration) noexcept;
	void AdmitReady(const std::shared_ptr<ITask>& task, std::uint64_t usefulItemCount = 1);
	void RejectUnstartedSubtree(const std::shared_ptr<ITask>& task, const std::exception_ptr& failure) noexcept;
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
		// Retain failure ownership directly; queue insertion no longer allocates a
		// wrapper callable that hides the task from worker infrastructure recovery.
		std::shared_ptr<ITask> failureOwner;
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
	// Allocate the cancellation exception before any queue ownership is accepted.
	// Shutdown itself can then publish cancellation without constructing an error.
	std::exception_ptr shutdownFailure;
	std::atomic<const ITask*> admissionFailureTarget{nullptr};
	std::exception_ptr admissionFailure;
	std::atomic<std::size_t> admissionFailureCount{0};
	
	std::vector<Task<void>*> voidTaskPool;
	// Peak reservation happens before ownership; return-to-cache never grows.
	std::size_t outstandingVoidTasks = 0U;
	enum class RecyclingState { Closed, Recycling, Closing };
	RecyclingState recyclingState = RecyclingState::Closed;
	std::uint64_t cacheGeneration = 0;
	std::mutex poolMutex;

	mutable std::mutex telemetryMutex;
	std::vector<MutableWorkerTelemetry> workerTelemetry;
	std::uint64_t telemetryGeneration = 0;
	std::uint64_t telemetrySequence = 0;
	std::uint64_t droppedTelemetryHistorySamples = 0;
	std::uint32_t telemetrySamplingIntervalDispatches = 1;
	std::size_t telemetryHistoryCapacityPerWorker = kMaximumTelemetryHistoryCapacityPerWorker;
};
