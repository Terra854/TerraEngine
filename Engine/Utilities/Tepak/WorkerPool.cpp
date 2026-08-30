// WorkerPool.cpp - Shared task-based worker coordination for TEPAK operations.

#include "Utilities/Tepak/WorkerPool.h"

#include <stdexcept>

namespace tepak::util {

	WorkerPool::WorkerPool(std::uint32_t workerCount)
		: m_workerCount(workerCount == 0U ? 1U : workerCount)
	{
		// All threads are created at the protected construction boundary. Jobs can
		// then run concurrently without changing the pool topology.
		m_threads.reserve(m_workerCount);
		for(std::uint32_t index = 0U; index < m_workerCount; ++index) {
			m_threads.emplace_back([this]() {
				WorkerLoop();
			});
		}
	}

	WorkerPool::~WorkerPool()
	{
		{
			std::scoped_lock lock(m_mutex);
			// Stop new submissions but allow workers to drain every accepted task
			// before their threads are joined.
			m_stopping = true;
		}
		m_condition.notify_all();
		for(std::thread& thread : m_threads) {
			if(thread.joinable()) {
				thread.join();
			}
		}
	}

	std::uint32_t WorkerPool::GetWorkerCount() const
	{
		return m_workerCount;
	}

	void WorkerPool::Enqueue(std::function<void()> task)
	{
		if(!task) {
			throw std::runtime_error("Worker pool tasks must be callable.");
		}

		{
			std::scoped_lock lock(m_mutex);
			if(m_stopping) {
				throw std::runtime_error("Worker pool is shutting down.");
			}
			m_tasks.push(std::move(task));
		}
		m_condition.notify_one();
	}

	void WorkerPool::WorkerLoop()
	{
		while(true) {
			std::function<void()> task;
			{
				std::unique_lock lock(m_mutex);
				m_condition.wait(lock, [this]() {
					return m_stopping || !m_tasks.empty();
				});

				if(m_stopping && m_tasks.empty()) {
					return;
				}

				// Queue ownership ends under the mutex. The task itself runs after the
				// lock is released so other workers and producers keep progressing.
				task = std::move(m_tasks.front());
				m_tasks.pop();
			}

			task();
		}
	}

} // namespace tepak::util
