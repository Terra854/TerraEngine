// ThreadPool.cpp — Contains the Thread Pool queue state, worker coordination, and synchronization helpers implementation for task queue storage, worker thread lifecycle, and synchronization primitives.

#include "ThreadPool.h"
#include <cstdio>
#include "ScopedTaskSettlement.h"
#include "Core/Logger/Logger.h"
#include "Profiling/Profiler.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
	using TelemetryClock = std::chrono::steady_clock;
	// During drain, a running dependency may still release its parent into the
	// same queue. External producers are closed; worker completion remains owned.
	thread_local ThreadPool* executingPool = nullptr;
	std::atomic<bool> nestedHookEnabled{false};
	std::atomic<std::shared_ptr<const TerraEngine::Tasks::NestedTaskHook>> nestedHook;

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

void TerraEngine::Tasks::SetNestedTaskHookForTesting(NestedTaskHook hook)
{
	// Configure only at a native test's protected quiescent boundary. The copied
	// callable remains alive until every already-admitted child has settled.
	auto snapshot = hook ? std::make_shared<const NestedTaskHook>(std::move(hook)) : nullptr;
	nestedHook.store(std::move(snapshot));
	nestedHookEnabled.store(static_cast<bool>(nestedHook.load()));
}

std::shared_ptr<const TerraEngine::Tasks::NestedTaskHook> TerraEngine::Tasks::CaptureNestedTaskHookForTesting()
{
	// The ordinary frame path pays no shared-pointer lock when tests are absent.
	return nestedHookEnabled.load(std::memory_order_relaxed) ? nestedHook.load() : nullptr;
}

ThreadPool& ThreadPool::instance() {
	static ThreadPool instance;
	return instance;
}

std::unique_ptr<ThreadPool> ThreadPool::CreateOwned(std::size_t workerCount,
	std::function<void(std::size_t)> beforeWorker)
{
	if(workerCount == 0) throw std::invalid_argument("Owned executor capacity must be positive");
	auto pool = std::unique_ptr<ThreadPool>(new ThreadPool);
	pool->ownedExecutor = true;
	pool->InitializeWorkers(workerCount, beforeWorker);
	return pool;
}

ThreadPool::~ThreadPool()
{
	// The creating owner releases this pool only after request/process settlement.
	// Joining also makes construction rollback and repeated shutdown harmless.
	shutdown();
}


void ThreadPool::initialize(std::size_t requestedWorkerCount) {
	InitializeWorkers(requestedWorkerCount, {});
}

void ThreadPool::InitializeWorkers(std::size_t requestedWorkerCount,
	const std::function<void(std::size_t)>& beforeWorker) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		// Treat initialization as idempotent so callers can safely guard startup
		// paths without accidentally spawning duplicate worker groups.
		if (!workers.empty()) {
			return;
		}
	}
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		// Never reuse a generation identity after wraparound. Lifecycle ownership
		// serializes initialize/shutdown, so this reservation needs no second owner.
		if(cacheGeneration == std::numeric_limits<std::uint64_t>::max()) {
			throw std::overflow_error("ThreadPool cache generation exhausted");
		}
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
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
	try {
		ResetTelemetryForWorkers(numThreads);
		for (size_t i = 0; i < numThreads; ++i) {
			if(beforeWorker) beforeWorker(i);
			workers.emplace_back([this, i] { this->workerThread((int)i); });
		}
		// A partially started pool must never recycle. Open a fresh generation
		// only after all requested workers were successfully constructed.
		std::lock_guard<std::mutex> lock(poolMutex);
		++cacheGeneration;
		recyclingState = RecyclingState::Recycling;
	} catch(...) {
		// A partially started executor still owns joinable workers. Stop and join
		// those workers before propagating the original construction failure.
		shutdown();
		throw;
	}
}


void ThreadPool::shutdown()
{
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		// Close before joining: releases by draining workers must destroy outside
		// locks instead of repopulating the cache being retired.
		recyclingState = RecyclingState::Closing;
	}
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		// Workers observe this flag under the same mutex used by the condition
		// variable so shutdown has a clear happens-before relationship.
		stop = true;
	}

	condition.notify_all();

	for (std::thread& worker : workers) {
		if (worker.joinable()) {
			worker.join();
		}
	}
	workers.clear();

	// Normal workers drain this queue. Unstarted/startup-failed ownership still
	// needs terminal cancellation. Retire one owner at a time without allocating
	// a replacement deque or destroying arbitrary captures under queue_mutex.
	for(;;) {
		QueuedTask retired;
		{
			std::lock_guard<std::mutex> lock(queue_mutex);
			if(tasks.empty()) break;
			retired = std::move(tasks.front());
			tasks.pop();
		}
		retired.failureOwner->Fail(shutdownFailure);
		completeTask(retired.failureOwner);
	}

	std::vector<Task<void>*> retiredCache;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		retiredCache.swap(voidTaskPool);
		recyclingState = RecyclingState::Closed;
	}
	// A callable destructor can release other tasks or invoke application cleanup.
	// The detached pointer owner remains stable through every such reentry.
	for(auto* task : retiredCache) delete task;
}

// Enqueue a task; handle its dependencies automatically

void ThreadPool::enqueue(std::shared_ptr<ITask> task, std::uint64_t usefulItemCount) {
	if(!task) throw std::invalid_argument("ThreadPool requires an owned task");
	{
		std::lock_guard lock(task->settlementMutex);
		if(task->executionState != ITask::ExecutionState::Prepared) {
			throw std::logic_error("ThreadPool task already submitted or completed");
		}
		if(task->dependenciesList.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
			throw std::length_error("ThreadPool dependency count exceeded");
		}
		task->remainingDependencies = task->dependenciesList.size();
		task->unresolvedDependencies = static_cast<int>(task->remainingDependencies);
		task->executionState = task->remainingDependencies == 0
			? ITask::ExecutionState::Ready : ITask::ExecutionState::WaitingDependencies;
	}
	if(task->dependenciesList.empty()) {
		try { AdmitReady(task, usefulItemCount); }
		catch(...) {
			// Dependency-free rejection retains its strong guarantee: the queue
			// owns nothing and callers may remove the unadmitted future or retry.
			std::lock_guard lock(task->settlementMutex);
			task->executionState = ITask::ExecutionState::Prepared;
			task->remainingDependencies = 0;
			task->unresolvedDependencies = -1;
			throw;
		}
		return;
	}
	for(std::size_t index = 0; index < task->dependenciesList.size(); ++index) {
		try { enqueue(task->dependenciesList[index]); }
		catch(...) {
			const auto failure = std::current_exception();
			{
				std::lock_guard lock(task->settlementMutex);
				if(!task->infrastructureFailure) task->infrastructureFailure = failure;
			}
			// Earlier children may already be admitted. Keep their real countdown
			// and retire only untouched branches; this is not graph-wide rollback.
			for(; index < task->dependenciesList.size(); ++index) {
				RejectUnstartedSubtree(task->dependenciesList[index], failure);
			}
			std::rethrow_exception(failure);
		}
	}
}

void ThreadPool::AdmitReady(const std::shared_ptr<ITask>& task, std::uint64_t usefulItemCount)
{
	{
		// Task->queue is the sole nested lock order. Workers drop queue_mutex
		// before touching task state, and shutdown retires task ownership unlocked.
		std::lock_guard stateLock(task->settlementMutex);
		if(task->executionState != ITask::ExecutionState::Ready || task->infrastructureFailure) {
			throw std::logic_error("ThreadPool task is not ready for admission");
		}
		std::lock_guard queueLock(queue_mutex);
		if(stop && executingPool != this) throw std::runtime_error("ThreadPool admission is closed");
		const auto depth = tasks.size() + 1;
		tasks.emplace(QueuedTask{task, depth, usefulItemCount, MonotonicNanoseconds()});
		task->executionState = ITask::ExecutionState::Queued;
	}
	ObserveTaskSafely([] { TerraEngine::Profiling::Profiler::Instance().RecordTaskQueued(); });
	condition.notify_one();
}

void ThreadPool::RejectUnstartedSubtree(const std::shared_ptr<ITask>& task,
	const std::exception_ptr& failure) noexcept
{
	{
		std::lock_guard lock(task->settlementMutex);
		// A recursively rejected branch may still own earlier admitted children.
		// It already established their settlement; never cancel/notify it twice.
		if(task->executionState != ITask::ExecutionState::Prepared) return;
		task->infrastructureFailure = failure;
		task->remainingDependencies = task->dependenciesList.size();
		task->unresolvedDependencies = static_cast<int>(task->remainingDependencies);
		task->executionState = task->remainingDependencies == 0
			? ITask::ExecutionState::Ready : ITask::ExecutionState::WaitingDependencies;
	}
	if(task->dependenciesList.empty()) {
		task->Fail(failure);
		completeTask(task);
		return;
	}
	for(const auto& child : task->dependenciesList) RejectUnstartedSubtree(child, failure);
}


std::shared_ptr<Task<void>> ThreadPool::AllocateVoidTask(std::function<void()> func)
{
	Task<void>* ptr = nullptr;
	std::uint64_t allocationGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		// Reserve the observed concurrent peak before acquiring any task. Reserve
		// failure leaves both the cache and ownership count unchanged; geometric
		// growth keeps normal reusable graph bursts efficient across frames.
		if (outstandingVoidTasks >= voidTaskPool.capacity()) {
			const auto maximum = voidTaskPool.max_size();
			if (outstandingVoidTasks >= maximum) throw std::length_error("Void task cache capacity exceeded.");
			const auto capacity = voidTaskPool.capacity();
			const auto next = capacity > maximum / 2U ? maximum : std::max<std::size_t>(64U, capacity * 2U);
			voidTaskPool.reserve(std::min(maximum, next));
		}
		++outstandingVoidTasks;
		allocationGeneration = cacheGeneration;
		if (recyclingState == RecyclingState::Recycling && !voidTaskPool.empty()) {
			ptr = voidTaskPool.back(); voidTaskPool.pop_back();
		}
	}
	try {
		if (!ptr) ptr = new Task<void>(std::move(func));
		else ptr->reset(std::move(func));
	} catch (...) {
		// A null rollback releases only the early ownership reservation. A
		// failed reset still owns its task and returns it through the safe path.
		ReleaseVoidTask(ptr, allocationGeneration); throw;
	}
	// If shared_ptr control-block allocation fails, its constructor invokes this
	// deleter exactly once. Do not also roll back that transferred pointer.
	return std::shared_ptr<Task<void>>(ptr, [this, allocationGeneration](Task<void>* task) noexcept {
		ReleaseVoidTask(task, allocationGeneration);
	});
}

// The shared-task deleter may run during fatal settlement or stack unwinding.
// Pointer insertion cannot allocate when size is below reserved capacity;
// excess tasks and any unexpected cache/lock failure are deleted unlocked.
void ThreadPool::ReleaseVoidTask(Task<void>* task) noexcept
{
	// Preserve the historical low-level entry point. Factory-exported owners
	// always use the overload carrying their actual allocation generation.
	std::uint64_t generation = 0;
	try {
		std::lock_guard<std::mutex> lock(poolMutex);
		generation = cacheGeneration;
	} catch(...) { delete task; return; }
	ReleaseVoidTask(task, generation);
}

void ThreadPool::ReleaseVoidTask(Task<void>* task, std::uint64_t allocationGeneration) noexcept
{
	try {
		std::lock_guard<std::mutex> lock(poolMutex);
		if (outstandingVoidTasks != 0U) --outstandingVoidTasks;
		if (task && recyclingState == RecyclingState::Recycling && allocationGeneration == cacheGeneration
			&& voidTaskPool.size() < voidTaskPool.capacity()) {
			voidTaskPool.push_back(task); task = nullptr;
		}
	} catch (...) {}
	delete task;
}

void ThreadPool::SetDependencyAdmissionFailureForTesting(const ITask* target, std::exception_ptr failure)
{
	if((target != nullptr) != static_cast<bool>(failure)) {
		throw std::invalid_argument("Dependency admission target and failure must both be set or cleared");
	}
	// Native callers own quiescence. Publishing the target last makes the immutable
	// exception visible before any matching dispatch reads it; clear only after drain.
	admissionFailureTarget.store(nullptr, std::memory_order_release);
	admissionFailure = std::move(failure);
	admissionFailureCount.store(0, std::memory_order_release);
	admissionFailureTarget.store(target, std::memory_order_release);
}

// Notify that a task has completed; decrement unresolvedDependencies of the parent task

void ThreadPool::completeTask(std::shared_ptr<ITask> completedTask) noexcept {
	// Iterative failure propagation retains only shared owners/exception pointers.
	// It allocates no callback, promise, container, diagnostic, or recursive stack.
	while(completedTask) {
		std::exception_ptr childFailure;
		{
			std::lock_guard lock(completedTask->settlementMutex);
			if(completedTask->executionState != ITask::ExecutionState::Terminal
				|| completedTask->parentNotificationDelivered) std::terminate();
			completedTask->parentNotificationDelivered = true;
			childFailure = completedTask->infrastructureFailure;
		}
		auto parent = completedTask->getParentTask().lock();
		if(!parent) return;
		std::exception_ptr failure;
		{
			std::lock_guard lock(parent->settlementMutex);
			if(parent->executionState != ITask::ExecutionState::WaitingDependencies
				|| parent->remainingDependencies == 0) std::terminate();
			if(childFailure && !parent->infrastructureFailure) parent->infrastructureFailure = childFailure;
			--parent->remainingDependencies;
			parent->unresolvedDependencies = static_cast<int>(parent->remainingDependencies);
			if(parent->remainingDependencies != 0) return;
			parent->executionState = ITask::ExecutionState::Ready;
			failure = parent->infrastructureFailure;
		}
		if(!failure) {
			bool targetedFailure = false;
			try {
				if(admissionFailureTarget.load(std::memory_order_acquire) == parent.get()) {
					admissionFailureCount.fetch_add(1, std::memory_order_release);
					std::fputs("RMS_ADMISSION_FAILURE_CHECKPOINT reached=1\n", stderr);
					std::fflush(stderr);
					targetedFailure = true;
					std::rethrow_exception(admissionFailure);
				}
				AdmitReady(parent);
				return;
			} catch(...) {
				// MSVC may copy an exception during rethrow/current_exception.
				// The native checkpoint already owns the original pointer; preserve
				// it after the real throw instead of replacing it with that copy.
				failure = targetedFailure ? admissionFailure : std::current_exception();
			}
		}
		// Promise publication occurs after the ancestor's own admitted siblings
		// physically complete, outside state locks, without running its callable.
		parent->Fail(failure);
		completedTask = std::move(parent);
	}
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
	// Restore thread-local ownership even if a future implementation reuses a
	// worker entry point. Queue callbacks finish before this owner joins the thread.
	struct ExecutionOwner {
		ThreadPool* previous = executingPool;
		explicit ExecutionOwner(ThreadPool* pool) { executingPool = pool; }
		~ExecutionOwner() { executingPool = previous; }
	} executionOwner(this);
	// Log that the worker thread is live so we can trace thread lifecycle events.
	ObserveTaskSafely([&] { TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " initialized and running."); });
	const std::size_t workerIndex = static_cast<std::size_t>(threadNum);
	QueuedTask task;
	while (true) {
		const std::uint64_t idleStartedNanoseconds = MonotonicNanoseconds();
		std::size_t queueDepthAtStart = 0;
		std::uint64_t waitNanoseconds = 0;
		std::uint64_t idleNanoseconds = 0;
		ObserveTaskSafely([&] { RecordWorkerActivity(workerIndex, ThreadPoolWorkerActivity::Idle); });
		{
			std::unique_lock<std::mutex> lock(this->queue_mutex);
			this->condition.wait(lock,
				[this] { return this->stop || !this->tasks.empty(); });

			if (this->stop && this->tasks.empty()) {
				const std::uint64_t stoppedNanoseconds = MonotonicNanoseconds();
				ObserveTaskSafely([&] { RecordWorkerActivity(workerIndex, ThreadPoolWorkerActivity::Stopped,
					ElapsedNanoseconds(idleStartedNanoseconds, stoppedNanoseconds)); });
				// Record that the worker is exiting after processing its last job.
				ObserveTaskSafely([&] { TERRAENGINE_LOG(LogLevel::Info, "Worker thread no. " << threadNum << " shutting down."); });
				return;
			}
			queueDepthAtStart = this->tasks.size();
			task = std::move(this->tasks.front());
			this->tasks.pop();
			const std::uint64_t startedNanoseconds = MonotonicNanoseconds();
			idleNanoseconds = ElapsedNanoseconds(idleStartedNanoseconds, startedNanoseconds);
			waitNanoseconds = ElapsedNanoseconds(task.enqueueNanoseconds, startedNanoseconds);
			ObserveTaskSafely([&] { RecordWorkerTaskStarted(workerIndex, task.queueDepthAtEnqueue, queueDepthAtStart,
				task.usefulItemCount, waitNanoseconds, idleNanoseconds); });
		}
		const std::uint64_t runStartedNanoseconds = MonotonicNanoseconds();
		bool profilerSampleActive = false;
		ObserveTaskSafely([&] { profilerSampleActive = TerraEngine::Profiling::Profiler::Instance().RecordTaskStarted("ThreadPool Task"); });
		try { task.failureOwner->run(); }
		catch(...) {
			// Direct queue ownership exposes the actual typed promise when owned
			// dispatch infrastructure fails before ordinary callable settlement.
			task.failureOwner->Fail(std::current_exception());
		}
		completeTask(task.failureOwner);
		ObserveTaskSafely([&] { TerraEngine::Profiling::Profiler::Instance().RecordTaskFinished(profilerSampleActive); });
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
