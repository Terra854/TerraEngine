// FileWatcher.h — Cross-platform filesystem watcher
//
// Provides a threaded file watcher for monitoring project directory changes. it supports OS-specific backends for efficient notification delivery and maintains a thread-safe event queue for the editor to process.

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <queue>
#include <cstdint>

enum class FileStatus {
	Created,
	Modified,
	Erased
};

// struct FileEvent groups related state and operations so callers have one documented contract.
struct FileEvent {
	std::filesystem::path path;
	FileStatus status;
	std::uint64_t sequence = 0u;
};

struct FileWatcherDiagnostics {
	std::filesystem::path watchedRoot;
	std::size_t queuedEvents = 0u;
	std::uint64_t lastSequence = 0u;
	std::uint64_t coalescedEvents = 0u;
	std::uint64_t droppedEvents = 0u;
	bool running = false;
	bool reconcileRequired = false;
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
	// A native overflow or bounded-queue drop requests a full AssetDatabase
	// reconcile. Consuming this flag is atomic with respect to watcher events so
	// the main thread cannot lose a recovery request between frames.
	bool ConsumeReconcileRequired();
	FileWatcherDiagnostics GetDiagnostics() const;

private:
	std::filesystem::path pathData;
	std::atomic<bool> running{ false };
	std::thread watchThread;
	
	// Event Queue
	std::vector<FileEvent> eventQueue;
	mutable std::mutex queueMutex;
	std::uint64_t nextSequence = 0u;
	std::uint64_t coalescedEvents = 0u;
	std::uint64_t droppedEvents = 0u;
	bool reconcileRequired = false;
	// Native watchers can report many writes while a compiler atomically swaps
	// outputs. Coalesce by path and bound the queue so a stalled editor cannot
	// turn source-watch noise into unbounded memory or delayed stale reimports.
	void QueueEvent(std::filesystem::path path, FileStatus status);
	void FlagReconcileRequired();
	
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
