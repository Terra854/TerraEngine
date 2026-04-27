// FileWatcher.h — Cross-platform filesystem watcher
//
// Provides a threaded file watcher for monitoring project directory changes. it supports OS-specific backends for efficient notification delivery and maintains a thread-safe event queue for the editor to process.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <queue>

enum class FileStatus {
	Created,
	Modified,
	Erased
};

// struct FileEvent groups related state and operations so callers have one documented contract.
struct FileEvent {
	std::filesystem::path path;
	FileStatus status;
};

// class FileWatcher groups related state and operations so callers have one documented contract.
class FileWatcher {
public:
	using Callback = std::function<void(std::filesystem::path, FileStatus)>;

	FileWatcher();
	~FileWatcher();

	// Start the background watching thread for the given directory
	void Start(const std::filesystem::path& path);

	// Stop the watching thread
	void Stop();

	// Retrieve all queued events (thread-safe)
	std::vector<FileEvent> PopEvents();

private:
	std::filesystem::path pathData;
	std::atomic<bool> running{ false };
	std::thread watchThread;
	
	// Event Queue
	std::vector<FileEvent> eventQueue;
	std::mutex queueMutex;
	
	// Internal helper to check if we should use the OS specific or generic loop
	void WatchLoop();

#if defined(_WIN32)
	void WatchLoopWin32();
#elif defined(__linux__)
	void WatchLoopLinux();
	void AddWatchesLinux(int fd, const std::filesystem::path& root, std::map<int, std::filesystem::path>& wdToPath);
#else
	void WatchLoopGeneric();
#endif
};
