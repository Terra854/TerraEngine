// Range jobs borrow the caller callback only until every admitted child settles.
#pragma once
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Profiling/ProfilerScope.h"
#include <algorithm>
#include <future>
#include <vector>
namespace TerraEngine::Renderer::Frame {
	inline constexpr std::size_t kRenderParallelThreshold = 64u;
	template<typename Fn>
	void RunRenderChunkedWork(std::size_t itemCount, std::size_t workerLimit, std::size_t parallelThreshold, const char* taskLabel, Fn&& work)
	{
		if (itemCount == 0u)
		{
			return;
		}
		const std::size_t workerCount = std::min(itemCount, std::max<std::size_t>(1u, workerLimit));
		if (workerCount <= 1u || itemCount < parallelThreshold)
		{
			work(0u, itemCount);
			return;
		}

		const std::size_t chunkSize = (itemCount + workerCount - 1u) / workerCount;
		std::vector<std::future<void>> futures;
		futures.reserve(workerCount - 1U);
		// Children borrow the callback and its immutable input spans. Settle every
		// admitted child before an inline, queue, or result exception can unwind them.
		TerraEngine::Tasks::ScopedTaskSettlement<void> settlement(futures);
		// This helper is called from an existing scheduler worker. Queue every
		// remaining disjoint range, then execute range zero on the current worker so
		// large renderer batches do not pay for an avoidable future and handoff.
		for (std::size_t taskIndex = 1u; taskIndex < workerCount; ++taskIndex)
		{
			const std::size_t begin = taskIndex * chunkSize;
			if (begin >= itemCount)
			{
				break;
			}
			const std::size_t end = std::min(itemCount, begin + chunkSize);
			settlement.CreateAndEnqueue(ThreadPool::instance(), [begin, end, taskLabel, &work]() {
				TERRAENGINE_PROFILE_DYNAMIC_SCOPE(taskLabel, TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
				work(begin, end);
			});
		}
		{
			TERRAENGINE_PROFILE_DYNAMIC_SCOPE(taskLabel,
				TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
			settlement.RunInline([&]() { work(0U, std::min(itemCount, chunkSize)); });
		}
		settlement.GetAll();
	}

	template<typename Cache>
	void TrimSignatureCache(Cache& cache, std::size_t limit)
	{
		while (cache.recordsBySignature.size() > limit)
		{
			cache.recordsBySignature.erase(cache.recordsBySignature.begin());
		}
	}
}
