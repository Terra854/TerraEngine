// ProfilerTestSupport.h -- Deterministic helpers shared by profiler journal tests.

#pragma once

#include "Profiling/ProfilerJournalV1.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>

namespace TerraEngine::Tests::ProfilerTestSupport
{
class TemporaryWorkspace
{
public:
	explicit TemporaryWorkspace(std::string_view name)
		: root(std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / name)
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
		std::filesystem::create_directories(root, ignored);
	}

	~TemporaryWorkspace()
	{
		std::error_code ignored;
		std::filesystem::remove_all(root, ignored);
	}

	[[nodiscard]] std::filesystem::path Path(std::string_view name) const
	{
		return root / std::string(name);
	}

	void WriteBytes(std::string_view name, std::string_view bytes) const
	{
		std::ofstream output(Path(name), std::ios::binary | std::ios::trunc);
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	}

	// TruncateFile models an interrupted final write without depending on a
	// process crash or filesystem race. Tests name the exact retained byte count,
	// so recovery assertions remain deterministic on every platform.
	bool TruncateFile(std::string_view name, std::uintmax_t retainedBytes) const
	{
		std::error_code error;
		std::filesystem::resize_file(Path(name), retainedBytes, error);
		return !error;
	}

	// CorruptByte changes one known byte in place. Committed-data rejection tests
	// can therefore inject corruption independently from truncation semantics.
	bool CorruptByte(std::string_view name, std::uintmax_t offset, std::uint8_t mask) const
	{
		std::fstream file(Path(name), std::ios::binary | std::ios::in | std::ios::out);
		if (!file)
		{
			return false;
		}
		file.seekg(static_cast<std::streamoff>(offset));
		char value = 0;
		file.read(&value, 1);
		if (!file)
		{
			return false;
		}
		value = static_cast<char>(static_cast<std::uint8_t>(value) ^ mask);
		file.seekp(static_cast<std::streamoff>(offset));
		file.write(&value, 1);
		file.flush();
		return static_cast<bool>(file);
	}

private:
	std::filesystem::path root;
};

// DeterministicClock is an injectable monotonic clock for lifecycle tests.
// Advancing time is explicit, which lets timeout/finalization state machines be
// tested without sleeps and without coupling their result to machine load.
class DeterministicClock
{
public:
	using Duration = std::chrono::nanoseconds;

	[[nodiscard]] Duration Now() const noexcept
	{
		return Duration(nowNanoseconds.load(std::memory_order_acquire));
	}

	void Advance(Duration duration) noexcept
	{
		nowNanoseconds.fetch_add(duration.count(), std::memory_order_acq_rel);
	}

	[[nodiscard]] std::function<Duration()> Reader()
	{
		return [this]() { return Now(); };
	}

private:
	std::atomic<std::int64_t> nowNanoseconds{0};
};

// ControlledJournalSink provides deterministic storage pressure. A test can
// stop one write until the producer has returned, or reject a chosen write,
// without relying on filesystem timing or a sleeping worker.
class ControlledJournalSink final : public Profiling::ProfilerJournalV1Sink
{
public:
	bool Open(const std::filesystem::path&, std::string& outError) override
	{
		outError.clear();
		open.store(true, std::memory_order_release);
		return true;
	}

	bool WriteAndFlush(std::string_view bytes, std::string& outError) override
	{
		std::unique_lock<std::mutex> lock(mutex);
		const std::uint64_t currentWrite = ++writeCount;
		changed.notify_all();
		if (currentWrite == blockedWrite)
		{
			blocked = true;
			changed.notify_all();
			changed.wait(lock, [this]() { return released; });
			blocked = false;
		}
		if (currentWrite == failedWrite)
		{
			outError = "Injected unavailable profiler journal sink.";
			changed.notify_all();
			return false;
		}
		// The controlled sink verifies ordering/gating, not payload contents.
		// Retaining every compressed segment would make a long-capture test sink
		// itself duration-scaled and can starve the real journal worker under a
		// full suite. Keep only bounded scalar accounting instead.
		totalWrittenBytes += static_cast<std::uint64_t>(bytes.size());
		changed.notify_all();
		return true;
	}

	bool IsOpen() const noexcept override
	{
		return open.load(std::memory_order_acquire);
	}

	void BlockWrite(std::uint64_t ordinal)
	{
		std::lock_guard<std::mutex> lock(mutex);
		blockedWrite = ordinal;
		released = false;
	}

	void FailWrite(std::uint64_t ordinal)
	{
		std::lock_guard<std::mutex> lock(mutex);
		failedWrite = ordinal;
	}

	bool WaitUntilBlocked(std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this]() { return blocked; });
	}

	bool WaitForWriteCount(std::uint64_t expected, std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(mutex);
		return changed.wait_for(lock, timeout, [this, expected]() {
			return writeCount >= expected;
		});
	}

	[[nodiscard]] std::uint64_t GetWriteCount() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return writeCount;
	}

	[[nodiscard]] std::uint64_t GetTotalWrittenBytes() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return totalWrittenBytes;
	}

	void Release()
	{
		std::lock_guard<std::mutex> lock(mutex);
		released = true;
		changed.notify_all();
	}

private:
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::uint64_t writeCount = 0u;
	std::uint64_t totalWrittenBytes = 0u;
	std::uint64_t blockedWrite = 0u;
	std::uint64_t failedWrite = 0u;
	std::atomic_bool open{false};
	bool blocked = false;
	bool released = false;
};
}
