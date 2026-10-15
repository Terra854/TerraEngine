// Renderer fixture borrower gate. Owns synchronization, never engine state/ECS.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace RuntimeMemorySafetyDraft {

	// The hook adapter calls Begin/End around the real light hash loop. The
	// fixture observer releases the held child before joining its phase future.
	class RendererBorrowerGate final {
	public:
		explicit RendererBorrowerGate(bool failInline) : failInline_(failInline) {}
		~RendererBorrowerGate() { Release(); }
		void Begin(std::size_t begin, std::size_t end) {
			if(begin >= end) throw std::runtime_error("Empty native light range");
			std::unique_lock lock(mutex_);
			if(begin != 0U && !childSelected_) {
				childSelected_ = true; heldBegin_ = begin; heldEnd_ = end;
				childArrived_ = true; changed_.notify_all();
				if(!changed_.wait_for(lock, std::chrono::seconds(10), [&] { return release_; }))
					throw std::runtime_error("Held native light borrower release timed out");
			} else if(begin == 0U && failInline_) {
				if(!changed_.wait_for(lock, std::chrono::seconds(5), [&] { return childArrived_; }))
					throw std::runtime_error("No child arrived before inline failure");
				inlineFailureReached_ = true; changed_.notify_all();
				throw std::runtime_error("RMS renderer inline failure after held child arrived");
			}
		}
		void End(std::size_t begin, std::size_t end, std::uint64_t actualDigest) {
			std::lock_guard lock(mutex_);
			++completedRanges_;
			if(childSelected_ && begin == heldBegin_ && end == heldEnd_) {
				childReadCompleted_ = true; childDigest_ = actualDigest;
			}
			changed_.notify_all();
		}
		bool WaitForHeldChild() {
			std::unique_lock lock(mutex_);
			return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return childArrived_; });
		}
		bool WaitForInlineFailure() {
			std::unique_lock lock(mutex_);
			return changed_.wait_for(lock, std::chrono::seconds(5), [&] { return inlineFailureReached_; });
		}
		void Release() noexcept {
			std::lock_guard lock(mutex_); release_ = true; changed_.notify_all();
		}
		struct Facts {
			bool childArrived, inlineFailureReached, childReadCompleted;
			std::size_t heldBegin, heldEnd, completedRanges;
			std::uint64_t childDigest;
		};
		Facts CopyFacts() {
			std::lock_guard lock(mutex_);
			return {childArrived_, inlineFailureReached_, childReadCompleted_, heldBegin_, heldEnd_, completedRanges_, childDigest_};
		}
	private:
		std::mutex mutex_;
		std::condition_variable changed_;
		bool failInline_ = false, childSelected_ = false, childArrived_ = false;
		bool inlineFailureReached_ = false, childReadCompleted_ = false, release_ = false;
		std::size_t heldBegin_ = 0U, heldEnd_ = 0U, completedRanges_ = 0U;
		std::uint64_t childDigest_ = 0U;
	};
}
