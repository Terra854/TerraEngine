// WorkerPool.h — TEPAK task-based worker pool interface
//
// Defines a reusable worker pool for managing background tasks during archive creation and extraction. It utilizes a thread-safe task queue and condition variables to coordinate parallel processing across multiple CPU cores.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

namespace tepak::util {

// WorkerPool is a small reusable task queue used by pack creation and extract
// operations so TEPAK stays task-based instead of managing raw threads in each
// subsystem.
class WorkerPool final {
public:
	explicit WorkerPool(std::uint32_t workerCount);
	~WorkerPool();

	WorkerPool(WorkerPool&&) = delete;
	WorkerPool& operator=(WorkerPool&&) = delete;
	WorkerPool(const WorkerPool&) = delete;
	WorkerPool& operator=(const WorkerPool&) = delete;

	template <typename Fn>
	auto Submit(Fn&& function) -> std::future<std::invoke_result_t<Fn>> {
		using Result = std::invoke_result_t<Fn>;

		// packaged_task gives callers a future while the queue itself only has to
		// store one type-erased callable.
		auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Fn>(function));
		auto future = task->get_future();
		Enqueue([task]() {
			(*task)();
		});
		return future;
	}

	// Returns the number of worker threads created for this pool.
	std::uint32_t GetWorkerCount() const;

private:
	void Enqueue(std::function<void()> task);
	void WorkerLoop();

	std::vector<std::thread> m_threads;
	std::queue<std::function<void()>> m_tasks;
	std::mutex m_mutex;
	std::condition_variable m_condition;
	bool m_stopping = false;
	std::uint32_t m_workerCount = 0;
};

} // namespace tepak::util
