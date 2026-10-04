// ScopedTaskSettlement.h — Own child completion before captured inputs expire.
#pragma once

#include "ThreadPool.h"

#include <exception>
#include <stdexcept>

namespace TerraEngine::Tasks {
	// Native tests observe the actual five production groups. The hook is copied
	// once at group entry, so changing a test hook cannot alter admitted children.
	enum class NestedTaskGroup {
		Unspecified, AnimationCompactCapture, AnimationHomogeneousRows,
		AnimationMixedRows, AnimationCompactRuntime, VulkanSecondaryRecording
	};
	enum class NestedTaskStage { Creation, Submission, Inline, Child, Result };
	struct NestedTaskCheckpoint {
		NestedTaskGroup group;
		NestedTaskStage stage;
		std::size_t childIndex;
	};
	using NestedTaskHook = std::function<void(NestedTaskCheckpoint)>;
	TERRAENGINE_API void SetNestedTaskHookForTesting(NestedTaskHook hook);
	TERRAENGINE_API std::shared_ptr<const NestedTaskHook> CaptureNestedTaskHookForTesting();

	// Declare this guard AFTER the inputs referenced by the children. Destruction
	// then joins admitted work before those inputs are destroyed during unwinding.
	// The caller reserves the future collection before submitting its first child.
	template<typename Result>
	class ScopedTaskSettlement {
	public:
		explicit ScopedTaskSettlement(std::vector<std::future<Result>>& futures,
			NestedTaskGroup group = NestedTaskGroup::Unspecified)
			: futures_(futures), group_(group), hook_(CaptureNestedTaskHookForTesting()) {}
		ScopedTaskSettlement(const ScopedTaskSettlement&) = delete;
		ScopedTaskSettlement& operator=(const ScopedTaskSettlement&) = delete;
		~ScopedTaskSettlement() noexcept { Wait(); }

		// The submit function must have the dependency-free enqueue guarantee:
		// throwing means the queue did not acquire ownership. Pretracking prevents
		// a later allocation from abandoning already admitted work. Removing an
		// unadmitted future is essential: a cached void task can retain its promise
		// indefinitely, so releasing its handle would not make that future ready.
		template<typename Submit>
		void TrackAndSubmit(const std::shared_ptr<Task<Result>>& task, Submit&& submit)
		{
			if(!task || !task->dependenciesList.empty()) {
				throw std::invalid_argument("Scoped settlement requires a dependency-free task");
			}
			if(futures_.size() == futures_.capacity()) {
				throw std::length_error("Reserve child completion tracking before admission");
			}
			futures_.push_back(task->getFuture());
			try { std::invoke(std::forward<Submit>(submit), task); }
			catch(...) {
				futures_.pop_back();
				throw;
			}
		}

		void Enqueue(ThreadPool& pool, const std::shared_ptr<Task<Result>>& task)
		{
			TrackAndSubmit(task, [&pool](const auto& ownedTask) { pool.enqueue(ownedTask); });
		}

		// Production callers use this entry point so tests can fail allocation,
		// queue insertion and child execution at their real ownership boundaries.
		template<typename Work>
		void CreateAndEnqueue(ThreadPool& pool, Work&& work)
		{
			const auto index = futures_.size();
			Checkpoint(NestedTaskStage::Creation, index);
			auto task = pool.createTask([hook = hook_, group = group_, index,
				work = std::forward<Work>(work)]() mutable -> Result {
				if(hook) (*hook)({group, NestedTaskStage::Child, index});
				return std::invoke(work);
			});
			TrackAndSubmit(task, [&](const auto& ownedTask) {
				Checkpoint(NestedTaskStage::Submission, index);
				pool.enqueue(ownedTask);
			});
		}

		template<typename Work>
		decltype(auto) RunInline(Work&& work)
		{
			Checkpoint(NestedTaskStage::Inline, futures_.size());
			return std::invoke(std::forward<Work>(work));
		}

		// Waiting does not inspect child exceptions and cannot replace an exception
		// already escaping the caller. Invalid/consumed futures are harmless. No
		// logging or allocation is permitted in this last-resort cleanup path.
		void Wait() noexcept
		{
			for(auto& future : futures_) {
				if(future.valid()) {
					try { future.wait(); } catch(...) {}
				}
			}
		}

		// Observe every admitted child even when an earlier one failed. The first
		// exception is preserved and no caller result acceptance happens here.
		void GetAll() requires std::is_void_v<Result>
		{
			std::exception_ptr first;
			std::size_t index = 0;
			for(auto& future : futures_) {
				if(!future.valid()) { ++index; continue; }
				try { future.get(); Checkpoint(NestedTaskStage::Result, index); }
				catch(...) { if(!first) first = std::current_exception(); }
				++index;
			}
			if(first) std::rethrow_exception(first);
		}

		// Results remain caller-owned prepared data until this returns successfully.
		// Reserve their storage before child admission as well. All children settle
		// even if getting or moving a result fails; callers discard failed batches.
		template<typename R = Result>
		void Collect(std::vector<R>& results) requires (!std::is_void_v<R> && std::is_same_v<R, Result>)
		{
			if(results.capacity() - results.size() < futures_.size()) {
				throw std::length_error("Reserve child results before admission");
			}
			std::exception_ptr first;
			std::size_t index = 0;
			for(auto& future : futures_) {
				if(!future.valid()) { ++index; continue; }
				try { results.push_back(future.get()); Checkpoint(NestedTaskStage::Result, index); }
				catch(...) { if(!first) first = std::current_exception(); }
				++index;
			}
			if(first) std::rethrow_exception(first);
		}

	private:
		void Checkpoint(NestedTaskStage stage, std::size_t index) const
		{
			if(hook_) (*hook_)({group_, stage, index});
		}
		std::vector<std::future<Result>>& futures_;
		NestedTaskGroup group_;
		std::shared_ptr<const NestedTaskHook> hook_;
	};
}
