// ThreadPool.cpp — Contains the Thread Pool queue state, worker coordination, and synchronization helpers implementation for task queue storage, worker thread lifecycle, and synchronization primitives.

#include "ThreadPool.h"
#include "Core/Logger/Logger.h"
#include "Profiling/Profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>

namespace {
	using TelemetryClock = std::chrono::steady_clock;

	// Steady-clock nanoseconds are meaningful only as differences, which is
	// exactly what worker telemetry needs. They are never serialized as wall time.
	std::uint64_t MonotonicNanoseconds()
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			TelemetryClock::now().time_since_epoch()).count());
	}

	std::uint64_t ElapsedNanoseconds(std::uint64_t start, std::uint64_t finish)
	{
		return finish >= start ? finish - start : 0;
	}

	// Diagnostic allocation failure must never change admission or prevent an
	// already-owned task from settling its future. Keep observations optional.
	template<typename Observation>
	void ObserveTaskSafely(Observation&& observation) noexcept
	{
		try { observation(); } catch (...) {}
	}
}

ThreadPool& ThreadPool::instance() {
	static ThreadPool instance;
	return instance;
}

// ThreadPool::initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ThreadPool::initialize(std::size_t requestedWorkerCount) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		// Treat initialization as idempotent so callers can safely guard startup
		// paths without accidentally spawning duplicate worker groups.
		if (!workers.empty()) {
			return;
		}
		stop = false;
	}

	const size_t hardwareThreads = std::thread::hardware_concurrency();
	const size_t detectedHardwareThreads = hardwareThreads > 0 ? hardwareThreads : 4;
	if (requestedWorkerCount == 0) {
		// The environment override is intentionally opt-in so existing launches
		// retain the hardware-concurrency behavior unless a baseline requests a
		// deterministic worker count.
		if (const char* overrideText = std::getenv("TERRAENGINE_THREADPOOL_WORKERS"); overrideText != nullptr && *overrideText != '\0') {
			char* end = nullptr;
			const unsigned long parsed = std::strtoul(overrideText, &end, 10);
			const bool valid = overrideText[0] != '-' && end != overrideText && *end == '\0' && parsed <= static_cast<unsigned long>(std::numeric_limits<std::size_t>::max());
			if (valid && parsed > 0) {
				requestedWorkerCount = static_cast<std::size_t>(parsed);
			}
			else if (valid && parsed == 0) {
				requestedWorkerCount = detectedHardwareThreads;
			}
			else {
				TERRAENGINE_LOG(LogLevel::Warning, "Ignoring invalid TERRAENGINE_THREADPOOL_WORKERS='" << overrideText << "'; using hardware concurrency.");
			}
		}
	}

	// Zero keeps the true hardware default. Explicit API and environment values
	// are intentionally not capped at hardware concurrency: deterministic stress
	// matrices must be able to request the exact worker count they are measuring.
	const size_t numThreads = std::max<std::size_t>(1, requestedWorkerCount == 0 ? detectedHardwareThreads : requestedWorkerCount);
	ResetTelemetryForWorkers(numThreads);

	for (size_t i = 0; i < numThreads; ++i) {
		workers.emplace_back([this, i] { this->workerThread((int)i); });
	}
}

// ThreadPool::shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ThreadPool::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		// Workers observe this flag under the same mutex used by the condition
		// variable so shutdown has a clear happens-before relationship.
		stop = true;
	}

	condition.notify_all();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (std::thread& worker : workers) {
		if (worker.joinable()) {
			worker.join();
		}
	}
	workers.clear();

	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		// A normal shutdown drains queued work before workers exit. This cleanup
		// keeps repeated shutdown calls safe if a caller queued work before
		// starting workers or if startup failed partway through.
		std::queue<QueuedTask> emptyTasks;
		tasks.swap(emptyTasks);
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
// ThreadPool::enqueue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ThreadPool::enqueue(std::shared_ptr<ITask> task, std::uint64_t usefulItemCount) {
	if (task->unresolvedDependencies == -1) {
		task->unresolvedDependencies = static_cast<int>(task->dependenciesList.size());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (task->unresolvedDependencies == 0) {
		{
			// Lock the queue mutex to ensure exclusive access to the task queue
			{
				std::unique_lock<std::mutex> lock(queue_mutex);
				const std::size_t queueDepthAtEnqueue = tasks.size() + 1;
				tasks.emplace(QueuedTask{[task]() {
					// The profiler returns whether a task sample was actually
					// opened. Carry that through to completion so idle profiler
					// sessions do not pay an unmatched EndSample call.
					bool profilerSampleActive = false;
					ObserveTaskSafely([&] { profilerSampleActive = TerraEngine::Profiling::Profiler::Instance().RecordTaskStarted("ThreadPool Task"); });
					task->run();
					ThreadPool::instance().completeTask(task);
					ObserveTaskSafely([&] { TerraEngine::Profiling::Profiler::Instance().RecordTaskFinished(profilerSampleActive); });
				}, queueDepthAtEnqueue, usefulItemCount, MonotonicNanoseconds()});
			} // Release the lock asap when done to minimize the chances of deadlocks

			ObserveTaskSafely([] { TerraEngine::Profiling::Profiler::Instance().RecordTaskQueued(); });
		}
		condition.notify_one();  // Notify a worker thread that a task is available
	}
	else {
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& dependentTask : task->dependenciesList) {
			enqueue(dependentTask);
		}
	}
}

// ThreadPool::AllocateVoidTask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::shared_ptr<Task<void>> ThreadPool::AllocateVoidTask(std::function<void()> func)
{
	Task<void>* ptr = nullptr;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!voidTaskPool.empty()) {
			ptr = voidTaskPool.back();
			voidTaskPool.pop_back();
		}
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ThreadPool::ReleaseVoidTask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ThreadPool::ReleaseVoidTask(Task<void>* task)
{
	std::lock_guard<std::mutex> lock(poolMutex);
	voidTaskPool.push_back(task);
}

// Notify that a task has completed; decrement unresolvedDependencies of the parent task
// ThreadPool::completeTask is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

void ThreadPool::ConfigureTelemetrySampling(std::uint32_t dispatchInterval, std::size_t historyCapacityPerWorker)
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	telemetrySamplingIntervalDispatches = std::max<std::uint32_t>(1, dispatchInterval);
	telemetryHistoryCapacityPerWorker = std::min(
		std::max<std::size_t>(1, historyCapacityPerWorker),
		kMaximumTelemetryHistoryCapacityPerWorker);

	// If a caller lowers the bound, discard oldest detail first and account for
	// every removed sample. Cumulative counters remain complete and unchanged.
	for (MutableWorkerTelemetry& worker : workerTelemetry) {
		while (worker.history.size() > telemetryHistoryCapacityPerWorker) {
			worker.history.pop_front();
			++worker.counters.droppedHistorySampleCount;
			++droppedTelemetryHistorySamples;
		}
	}
	++telemetryGeneration;
}

void ThreadPool::ResetTelemetry()
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	telemetrySequence = 0;
	droppedTelemetryHistorySamples = 0;
	for (std::size_t workerIndex = 0; workerIndex < workerTelemetry.size(); ++workerIndex) {
		const ThreadPoolWorkerActivity activity = workerTelemetry[workerIndex].counters.activity;
		workerTelemetry[workerIndex] = MutableWorkerTelemetry{};
		workerTelemetry[workerIndex].counters.workerIndex = workerIndex;
		workerTelemetry[workerIndex].counters.activity = activity;
	}
	++telemetryGeneration;
}

std::shared_ptr<const ThreadPoolTelemetrySnapshot> ThreadPool::GetTelemetrySnapshot() const
{
	auto snapshot = std::make_shared<ThreadPoolTelemetrySnapshot>();
	std::lock_guard<std::mutex> lock(telemetryMutex);
	snapshot->generation = telemetryGeneration;
	snapshot->samplingIntervalDispatches = telemetrySamplingIntervalDispatches;
	snapshot->historyCapacityPerWorker = telemetryHistoryCapacityPerWorker;
	snapshot->droppedHistorySampleCount = droppedTelemetryHistorySamples;
	snapshot->workers.reserve(workerTelemetry.size());
	for (const MutableWorkerTelemetry& source : workerTelemetry) {
		ThreadPoolWorkerTelemetry worker = source.counters;
		worker.history.assign(source.history.begin(), source.history.end());
		snapshot->workers.push_back(std::move(worker));
	}
	return snapshot;
}

void ThreadPool::ResetTelemetryForWorkers(std::size_t workerCount)
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	workerTelemetry.clear();
	workerTelemetry.resize(workerCount);
	for (std::size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
		workerTelemetry[workerIndex].counters.workerIndex = workerIndex;
	}
	telemetrySequence = 0;
	droppedTelemetryHistorySamples = 0;
	++telemetryGeneration;
}

void ThreadPool::RecordWorkerActivity(std::size_t workerIndex, ThreadPoolWorkerActivity activity, std::uint64_t idleNanoseconds)
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	if (workerIndex >= workerTelemetry.size()) {
		return;
	}
	MutableWorkerTelemetry& worker = workerTelemetry[workerIndex];
	worker.counters.activity = activity;
	worker.counters.idleNanoseconds += idleNanoseconds;
	++telemetryGeneration;
}

void ThreadPool::RecordWorkerTaskStarted(
	std::size_t workerIndex,
	std::size_t queueDepthAtEnqueue,
	std::size_t queueDepthAtStart,
	std::uint64_t usefulItemCount,
	std::uint64_t waitNanoseconds,
	std::uint64_t idleNanoseconds)
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	if (workerIndex >= workerTelemetry.size()) {
		return;
	}
	ThreadPoolWorkerTelemetry& worker = workerTelemetry[workerIndex].counters;
	worker.activity = ThreadPoolWorkerActivity::Running;
	worker.latestQueueDepthAtEnqueue = queueDepthAtEnqueue;
	worker.latestQueueDepthAtStart = queueDepthAtStart;
	++worker.dispatchCount;
	worker.usefulItemCount += usefulItemCount;
	worker.waitNanoseconds += waitNanoseconds;
	worker.idleNanoseconds += idleNanoseconds;
	++telemetryGeneration;
}

void ThreadPool::RecordWorkerTaskFinished(
	std::size_t workerIndex,
	std::size_t queueDepthAtEnqueue,
	std::size_t queueDepthAtStart,
	std::uint64_t usefulItemCount,
	std::uint64_t waitNanoseconds,
	std::uint64_t idleNanoseconds,
	std::uint64_t runNanoseconds)
{
	std::lock_guard<std::mutex> lock(telemetryMutex);
	if (workerIndex >= workerTelemetry.size()) {
		return;
	}
	MutableWorkerTelemetry& worker = workerTelemetry[workerIndex];
	worker.counters.activity = ThreadPoolWorkerActivity::Idle;
	worker.counters.runNanoseconds += runNanoseconds;

	// Sampling is deterministic by the worker's completed dispatch number. A
	// skipped interval is intentional sampling, while overwritten ring entries
	// are counted separately as dropped history.
	if (worker.counters.dispatchCount % telemetrySamplingIntervalDispatches == 0) {
		if (worker.history.size() == telemetryHistoryCapacityPerWorker) {
			worker.history.pop_front();
			++worker.counters.droppedHistorySampleCount;
			++droppedTelemetryHistorySamples;
		}
		worker.history.push_back(ThreadPoolWorkerTelemetrySample{
			++telemetrySequence,
			workerIndex,
			queueDepthAtEnqueue,
			queueDepthAtStart,
			usefulItemCount,
			waitNanoseconds,
			runNanoseconds,
			idleNanoseconds});
	}
	++telemetryGeneration;
}

void ThreadPool::workerThread(int threadNum)
{
	// Log that the worker thread is live so we can trace thread lifecycle events.
	TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " initialized and running.");
	const std::size_t workerIndex = static_cast<std::size_t>(threadNum);
	QueuedTask task;
	while (true) {
		const std::uint64_t idleStartedNanoseconds = MonotonicNanoseconds();
		std::size_t queueDepthAtStart = 0;
		std::uint64_t waitNanoseconds = 0;
		std::uint64_t idleNanoseconds = 0;
		RecordWorkerActivity(workerIndex, ThreadPoolWorkerActivity::Idle);
		{
			std::unique_lock<std::mutex> lock(this->queue_mutex);
			this->condition.wait(lock,
				[this] { return this->stop || !this->tasks.empty(); });
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (this->stop && this->tasks.empty()) {
				const std::uint64_t stoppedNanoseconds = MonotonicNanoseconds();
				RecordWorkerActivity(workerIndex, ThreadPoolWorkerActivity::Stopped,
					ElapsedNanoseconds(idleStartedNanoseconds, stoppedNanoseconds));
				// Record that the worker is exiting after processing its last job.
				TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " shutting down.");
				return;
			}
			queueDepthAtStart = this->tasks.size();
			task = std::move(this->tasks.front());
			this->tasks.pop();
			const std::uint64_t startedNanoseconds = MonotonicNanoseconds();
			idleNanoseconds = ElapsedNanoseconds(idleStartedNanoseconds, startedNanoseconds);
			waitNanoseconds = ElapsedNanoseconds(task.enqueueNanoseconds, startedNanoseconds);
			RecordWorkerTaskStarted(workerIndex, task.queueDepthAtEnqueue, queueDepthAtStart,
				task.usefulItemCount, waitNanoseconds, idleNanoseconds);
		}
		const std::uint64_t runStartedNanoseconds = MonotonicNanoseconds();
		task.execute();
		const std::uint64_t finishedNanoseconds = MonotonicNanoseconds();
		ObserveTaskSafely([&] { RecordWorkerTaskFinished(workerIndex, task.queueDepthAtEnqueue, queueDepthAtStart,
			task.usefulItemCount, waitNanoseconds, idleNanoseconds,
			ElapsedNanoseconds(runStartedNanoseconds, finishedNanoseconds)); });
		// Drop captures held by the completed callable before waiting for the
		// next job so pooled Task<void> objects can return to the reuse cache
		// immediately after their futures become ready.
		task = {};
	}
}
