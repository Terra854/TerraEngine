// Shared, test-only coordination. These helpers never initialize or shut down
// engine singletons; each harness retains ownership of its runtime fixture.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace TerraEngine::Tests::Correctness {

	inline constexpr auto kDefaultTimeout = std::chrono::seconds(5);

	enum class GateResult { Released, Canceled, TimedOut };

	// A one-shot gate controls an exact interleaving without timing guesses.
	// Worker callbacks capture MakeWaiter() by value, never a reference to the
	// owner. Destroying the owner cancels waiters, whose shared state stays alive.
	class ControlledGate {
		struct State {
			std::mutex mutex;
			std::condition_variable changed;
			std::size_t arrivals = 0;
			bool terminal = false;
			GateResult result = GateResult::Canceled;
		};

	public:
		ControlledGate() : state_(std::make_shared<State>()) {}
		~ControlledGate() { Cancel(); }
		ControlledGate(const ControlledGate&) = delete;
		ControlledGate& operator=(const ControlledGate&) = delete;

		// The bounded wait also handles a callback reached only after owner
		// teardown. Release/cancel arbitration is first-terminal-wins.
		std::function<GateResult()> MakeWaiter(std::chrono::milliseconds timeout = kDefaultTimeout) const {
			return [state = state_, timeout]() {
				std::unique_lock lock(state->mutex);
				++state->arrivals;
				state->changed.notify_all();
				if (!state->changed.wait_for(lock, timeout, [&] { return state->terminal; })) {
					return GateResult::TimedOut;
				}
				return state->result;
			};
		}

		// Await arrival before releasing a gate so the test proves that the
		// production callback reached the intended boundary, not just that a
		// future eventually completed. Cancellation makes an unmet wait fail.
		bool WaitForArrivals(std::size_t count, std::chrono::milliseconds timeout = kDefaultTimeout) const {
			std::unique_lock lock(state_->mutex);
			state_->changed.wait_for(lock, timeout, [&] {
				return state_->arrivals >= count || state_->terminal;
			});
			return state_->arrivals >= count;
		}

		void Release() { Finish(GateResult::Released); }
		void Cancel() { Finish(GateResult::Canceled); }

	private:
		void Finish(GateResult result) {
			std::lock_guard lock(state_->mutex);
			if (!state_->terminal) {
				state_->terminal = true;
				state_->result = result;
				state_->changed.notify_all();
			}
		}
		std::shared_ptr<State> state_;
	};

	// Copy the counter into callbacks to keep its lifetime independent from the
	// stack fixture. Count every terminal callback, including duplicates; after
	// joining producer futures compare Value() with the exact expected count.
	class CompletionCounter {
		struct State {
			std::mutex mutex;
			std::condition_variable changed;
			std::size_t count = 0;
		};
	public:
		CompletionCounter() : state_(std::make_shared<State>()) {}
		void Increment() const {
			std::lock_guard lock(state_->mutex);
			++state_->count;
			state_->changed.notify_all();
		}
		std::size_t Value() const {
			std::lock_guard lock(state_->mutex);
			return state_->count;
		}
		bool WaitForAtLeast(std::size_t count, std::chrono::milliseconds timeout = kDefaultTimeout) const {
			std::unique_lock lock(state_->mutex);
			return state_->changed.wait_for(lock, timeout, [&] { return state_->count >= count; });
		}
	private:
		std::shared_ptr<State> state_;
	};

	// An exclusively created temporary directory owns only disposable data.
	// Join every producer before destroying it; cleanup never changes cwd or
	// resource roots. Explicit Cleanup() exposes failures to test assertions.
	class DisposableFixturePath {
	public:
		DisposableFixturePath() {
			std::random_device random;
			const auto parent = std::filesystem::temp_directory_path();
			for (unsigned attempt = 0; attempt < 64; ++attempt) {
				const auto candidate = parent / ("TerraEngine-correctness-"
					+ std::to_string(random()) + "-" + std::to_string(random()));
				std::error_code error;
				if (std::filesystem::create_directory(candidate, error)) {
					root_ = candidate;
					return;
				}
				if (error && error != std::errc::file_exists) {
					throw std::filesystem::filesystem_error("Create correctness fixture", candidate, error);
				}
			}
			throw std::runtime_error("Could not allocate a unique correctness fixture directory.");
		}
		~DisposableFixturePath() { (void)Cleanup(); }
		DisposableFixturePath(const DisposableFixturePath&) = delete;
		DisposableFixturePath& operator=(const DisposableFixturePath&) = delete;
		const std::filesystem::path& Root() const { return root_; }

		// Reject traversal/absolute inputs before forming a child fixture path.
		// Callers own creation of child directories/files and must not introduce
		// symlinks that escape this test-owned tree.
		std::filesystem::path Child(const std::filesystem::path& relative) const {
			if (root_.empty()) throw std::logic_error("Fixture directory was already cleaned up.");
			if (relative.has_root_path()) throw std::invalid_argument("Fixture child must be relative.");
			for (const auto& part : relative) {
				if (part == "..") throw std::invalid_argument("Fixture child cannot traverse parents.");
			}
			return root_ / relative;
		}
		std::error_code Cleanup() noexcept {
			std::error_code error;
			if (!root_.empty()) std::filesystem::remove_all(root_, error);
			// Relinquish ownership after successful removal so a destructor cannot
			// later remove a different directory recreated at the same pathname.
			if (!error) root_.clear();
			return error;
		}
	private:
		std::filesystem::path root_;
	};

	// Existing ThreadPool example (runtime initialization belongs to the test):
	// ControlledGate gate; CompletionCounter completions;
	// auto task = pool.createTask([wait = gate.MakeWaiter(), completions] {
	//     const auto result = wait(); completions.Increment(); return result;
	// });
	// auto future = task->getFuture(); pool.enqueue(task);
	// context.Expect(gate.WaitForArrivals(1), "Worker reached the gate.");
	// gate.Release(); // Or Cancel(), including on assertion/exception teardown.
	// Assert future.wait_for(kDefaultTimeout) == ready BEFORE calling get().
	// Assert future.get() == GateResult::Released and completions.Value() == 1.
	// The external watchdog bounds unrelated production deadlocks; no helper
	// claims that canceling its gate can stop arbitrary blocked engine work.
}
