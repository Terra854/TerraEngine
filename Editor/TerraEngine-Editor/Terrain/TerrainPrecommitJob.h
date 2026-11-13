// TerrainPrecommitJob.h -- Bounded protected joins for detached terrain preparation.
#pragma once

#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace TerraEngine::Editor::Terrain
{
	enum class TerrainPreparationPoll { Pending, Ready, Failed };
	// The protected frame never joins detached work. Only a ready future is
	// consumed; worker failures become terminal diagnostics without publication.
	template<typename Result>
	[[nodiscard]] TerrainPreparationPoll PollTerrainPrecommitJob(std::future<Result>& future,
		std::optional<Result>& output, std::string& outError)
	{
		outError.clear();
		if(!future.valid()) { outError = "Terrain preparation has no admitted future."; return TerrainPreparationPoll::Failed; }
		if(future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return TerrainPreparationPoll::Pending;
		try { output = future.get(); return TerrainPreparationPoll::Ready; }
		catch(const std::exception& error) { outError = error.what(); }
		catch(...) { outError = "Automatic terrain collision worker failed."; }
		output.reset();
		return TerrainPreparationPoll::Failed;
	}

	// Detached collision preparation must settle before the protected editor caller
	// stages a candidate for the existing Before Physics adoption boundary. The
	// authoring root may already be committed; failure does not roll that root back.
	// The expensive work stays on a worker and the owner joins for at most ten
	// seconds, matching other
	// bounded native preparation waits. Callables must own their immutable inputs.
	// Pool promises (unlike std::async futures) do not join in their destructor:
	// on timeout, late work keeps its own captures and its result is discarded.
	template<typename Work>
	[[nodiscard]] auto RunTerrainPrecommitJob(ThreadPool& pool, Work work,
		std::uint64_t usefulItems, std::string& outError,
		std::chrono::milliseconds waitBudget = std::chrono::seconds(10))
		-> std::optional<std::invoke_result_t<Work&>>
	{
		using Result = std::invoke_result_t<Work&>;
		static_assert(!std::is_void_v<Result>);
		outError.clear();
		if(waitBudget <= std::chrono::milliseconds::zero() || pool.getThreadCount() == 0u)
		{
			outError = "Terrain collision preparation requires an active worker and a positive deadline.";
			return std::nullopt;
		}
		try
		{
			auto task = pool.createTask(std::move(work));
			auto future = task->getFuture();
			pool.enqueue(task, usefulItems);
			if(future.wait_for(waitBudget) != std::future_status::ready)
			{
				outError = "Terrain collision preparation exceeded its precommit deadline; no collision candidate was published.";
				return std::nullopt;
			}
			return future.get();
		}
		catch(const std::exception& exception)
		{
			outError = std::string("Terrain collision preparation failed before publication: ") + exception.what();
		}
		catch(...)
		{
			outError = "Terrain collision preparation failed before publication with an unknown worker exception.";
		}
		return std::nullopt;
	}
}
