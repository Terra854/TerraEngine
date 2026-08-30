// WorkerPool.h - Shared TEPAK task queue used by runtime reads and export writes.
//
// The runtime content mount and editor exporter perform the same bounded units
// of archive work. Keeping their worker coordination here gives both modules one
// implementation while preserving the existing tepak::util public API.

#pragma once

#include "Core/Export.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace tepak::util {

	// WorkerPool owns a fixed group of threads and a protected FIFO task queue.
	// Archive modules submit independent jobs and receive futures without creating
	// ad-hoc threads or exposing mutable queue storage to worker callbacks.
	class TERRAENGINE_API WorkerPool final {
	public:
		explicit WorkerPool(std::uint32_t workerCount);
		~WorkerPool();

		WorkerPool(WorkerPool&&) = delete;
		WorkerPool& operator=(WorkerPool&&) = delete;
		WorkerPool(const WorkerPool&) = delete;
		WorkerPool& operator=(const WorkerPool&) = delete;

		template<typename Fn>
		auto Submit(Fn&& function) -> std::future<std::invoke_result_t<Fn>>
		{
			using Result = std::invoke_result_t<Fn>;

			// packaged_task translates a worker exception into the returned future,
			// while the internal queue stores one uniform callable type.
			auto task = std::make_shared<std::packaged_task<Result()>>(
				std::forward<Fn>(function));
			auto future = task->get_future();
			Enqueue([task]() {
				(*task)();
			});
			return future;
		}

		// Returns the actual thread count after a zero request is clamped to one.
		[[nodiscard]] std::uint32_t GetWorkerCount() const;

	private:
		void Enqueue(std::function<void()> task);
		void WorkerLoop();

		std::vector<std::thread> m_threads;
		std::queue<std::function<void()>> m_tasks;
		std::mutex m_mutex;
		std::condition_variable m_condition;
		bool m_stopping = false;
		std::uint32_t m_workerCount = 0U;
	};

} // namespace tepak::util
