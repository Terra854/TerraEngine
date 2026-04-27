// WorkerPool.cpp — Contains the Worker Pool queue state, worker coordination, and synchronization helpers implementation for binary IO helpers and worker coordination used by runtime content reads.

#include "util/WorkerPool.h"

#include <stdexcept>

namespace tepak::util {

WorkerPool::WorkerPool(std::uint32_t workerCount) : m_workerCount(workerCount == 0U ? 1U : workerCount) {
	// The standalone packer keeps thread ownership inside a small worker pool so
	// compression and extraction stay task-based instead of spawning ad hoc threads.
	m_threads.reserve(m_workerCount);
	for (std::uint32_t index = 0U; index < m_workerCount; ++index) {
		m_threads.emplace_back([this]() {
			WorkerLoop();
		});
	}
}

WorkerPool::~WorkerPool() {
	{
		std::scoped_lock lock(m_mutex);
		// Stop accepting new work and let sleeping workers drain any queued tasks
		// before the pool tears its threads down.
		m_stopping = true;
	}
	m_condition.notify_all();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& thread : m_threads) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (thread.joinable()) {
			thread.join();
		}
	}
}

// WorkerPool::GetWorkerCount is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::uint32_t WorkerPool::GetWorkerCount() const {
	return m_workerCount;
}

// WorkerPool::Enqueue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WorkerPool::Enqueue(std::function<void()> task) {
	if (!task) {
		throw std::runtime_error("Worker pool tasks must be callable.");
	}

	{
		std::scoped_lock lock(m_mutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_stopping) {
			throw std::runtime_error("Worker pool is shutting down.");
		}
		m_tasks.push(std::move(task));
	}
	m_condition.notify_one();
}

// WorkerPool::WorkerLoop is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void WorkerPool::WorkerLoop() {
	while (true) {
		std::function<void()> task;
		{
			std::unique_lock lock(m_mutex);
			m_condition.wait(lock, [this]() {
				return m_stopping || !m_tasks.empty();
			});

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (m_stopping && m_tasks.empty()) {
				return;
			}

			// Pop under the lock, then run outside it so long compression jobs do
			// not block unrelated producers or workers from making progress.
			task = std::move(m_tasks.front());
			m_tasks.pop();
		}

		task();
	}
}

} // namespace tepak::util
