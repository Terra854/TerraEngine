// FileWatcher.cpp — Filesystem watcher implementation
//
// Implements the background watching logic using ReadDirectoryChangesW on Windows and inotify on Linux. It handles recursive directory monitoring and translates native filesystem events into generic status updates for the asset database.

#include "FileWatcher.h"
#include <iostream>
#include <map>
#include <chrono>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#endif

FileWatcher::FileWatcher() {}

FileWatcher::~FileWatcher() {
	Stop();
}

// FileWatcher::Start is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::Start(const std::filesystem::path& path) {
	if (running) return;
	this->pathData = path;
	running = true;
	watchThread = std::thread(&FileWatcher::WatchLoop, this);
}

// FileWatcher::Stop is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::Stop() {
	running = false;
	if (watchThread.joinable()) {
		watchThread.join();
	}
}

// FileWatcher::PopEvents is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::vector<FileEvent> FileWatcher::PopEvents() {
	std::lock_guard<std::mutex> lock(queueMutex);
	std::vector<FileEvent> events = std::move(eventQueue);
	eventQueue.clear();
	return events;
}

bool FileWatcher::ConsumeReconcileRequired()
{
	std::lock_guard<std::mutex> lock(queueMutex);
	const bool required = reconcileRequired;
	reconcileRequired = false;
	return required;
}

FileWatcherDiagnostics FileWatcher::GetDiagnostics() const
{
	std::lock_guard<std::mutex> lock(queueMutex);
	FileWatcherDiagnostics diagnostics;
	diagnostics.watchedRoot = pathData;
	diagnostics.queuedEvents = eventQueue.size();
	diagnostics.lastSequence = nextSequence;
	diagnostics.coalescedEvents = coalescedEvents;
	diagnostics.droppedEvents = droppedEvents;
	diagnostics.running = running.load();
	diagnostics.reconcileRequired = reconcileRequired;
	return diagnostics;
}

void FileWatcher::FlagReconcileRequired()
{
	std::lock_guard<std::mutex> lock(queueMutex);
	reconcileRequired = true;
}

void FileWatcher::QueueEvent(std::filesystem::path path, FileStatus status)
{
	constexpr std::size_t kMaximumQueuedEvents = 4096u;
	std::error_code pathError;
	path = std::filesystem::absolute(path, pathError).lexically_normal();
	if (pathError || path.empty())
	{
		FlagReconcileRequired();
		return;
	}
	std::lock_guard<std::mutex> lock(queueMutex);
	// The newest state for one path is the only useful one after AssetDatabase's
	// debounce window. Replacing it avoids reimporting an already superseded
	// intermediate save while retaining create/erase ordering across paths.
	if (const auto existing = std::find_if(eventQueue.rbegin(), eventQueue.rend(),
		[&path](const FileEvent& event) { return event.path == path; }); existing != eventQueue.rend())
	{
		// Preserve the net filesystem effect of common atomic-save sequences.
		// In particular, Erased->Created is a replacement/modify, while an item
		// created and erased inside one debounce window should remain erased.
		if (existing->status == FileStatus::Erased && status == FileStatus::Created)
		{
			existing->status = FileStatus::Modified;
		}
		else if (existing->status != FileStatus::Created || status == FileStatus::Erased)
		{
			existing->status = status;
		}
		existing->sequence = ++nextSequence;
		++coalescedEvents;
		return;
	}
	if (eventQueue.size() >= kMaximumQueuedEvents)
	{
		// The editor consumes reconcileRequired and walks authoritative disk state.
		// This makes bounded queue pressure observable and loss-recoverable instead
		// of silently assuming another event will arrive for the dropped path.
		eventQueue.erase(eventQueue.begin());
		++droppedEvents;
		reconcileRequired = true;
	}
	eventQueue.push_back({ std::move(path), status, ++nextSequence });
}

// FileWatcher::WatchLoop is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::WatchLoop() {
#if defined(_WIN32)
	WatchLoopWin32();
#elif defined(__linux__)
	WatchLoopLinux();
#else
	WatchLoopGeneric();
#endif
}

#if defined(_WIN32)
// FileWatcher::WatchLoopWin32 is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::WatchLoopWin32() {
	// Win32 ReadDirectoryChangesW Implementation
	HANDLE hDir = CreateFileW(
		pathData.c_str(),
		FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		NULL
	);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (hDir == INVALID_HANDLE_VALUE) {
		// Fallback or error logging
		return;
	}

	char buffer[4096];
	DWORD bytesReturned;
	OVERLAPPED overlapped = { 0 };
	overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
	
	BOOL success = ReadDirectoryChangesW(
		hDir,
		buffer,
		sizeof(buffer),
		TRUE, // Recursive
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
		NULL,
		&overlapped,
		NULL
	);

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (running) {
		DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 50); // Check every 50ms
		if (waitResult == WAIT_OBJECT_0) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
				FILE_NOTIFY_INFORMATION* pNotify;
				int offset = 0;
				do {
					pNotify = (FILE_NOTIFY_INFORMATION*)((char*)buffer + offset);
					
					std::wstring filename(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
					std::filesystem::path fullPath = pathData / filename;
					
					FileStatus status = FileStatus::Modified;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME) status = FileStatus::Created;
					else if (pNotify->Action == FILE_ACTION_REMOVED || pNotify->Action == FILE_ACTION_RENAMED_OLD_NAME) status = FileStatus::Erased;
					else if (pNotify->Action == FILE_ACTION_MODIFIED) status = FileStatus::Modified;

					QueueEvent(std::move(fullPath), status);

					offset += pNotify->NextEntryOffset;
				} while (pNotify->NextEntryOffset != 0);
			}
			ResetEvent(overlapped.hEvent);
			success = ReadDirectoryChangesW(
				hDir, buffer, sizeof(buffer), TRUE,
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
				NULL, &overlapped, NULL
			);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!success) break;
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (success) {
		CancelIo(hDir);
		GetOverlappedResult(hDir, &overlapped, &bytesReturned, TRUE);
	}
	
	CloseHandle(overlapped.hEvent);
	CloseHandle(hDir);
}
#elif defined(__linux__)

// FileWatcher::AddWatchesLinux is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::AddWatchesLinux(int fd, const std::filesystem::path& root, std::map<int, std::filesystem::path>& wdToPath) {
	// Add watch for the root
	int wd = inotify_add_watch(fd, root.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (wd != -1) {
		wdToPath[wd] = root;
	}
	
	// Recursively add watches for subdirectories
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
		if (p.is_directory()) {
			wd = inotify_add_watch(fd, p.path().c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (wd != -1) {
				wdToPath[wd] = p.path();
			}
		}
	}
}

// FileWatcher::WatchLoopLinux is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::WatchLoopLinux() {
	int fd = inotify_init1(IN_NONBLOCK);
	if (fd == -1) return;

	std::map<int, std::filesystem::path> wdToPath;
	AddWatchesLinux(fd, pathData, wdToPath);

	const int BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
	char buf[BUF_LEN];

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (running) {
		int length = read(fd, buf, BUF_LEN);
		if (length < 0) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (errno == EAGAIN) {
				// No events, sleep a bit
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			} else {
				// Error
				break;
			}
		}

		int i = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (i < length) {
			// struct inotify_event groups related state and operations so callers have one documented contract.
			struct inotify_event* event = (struct inotify_event*)&buf[i];
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (event->len) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (wdToPath.find(event->wd) != wdToPath.end()) {
					std::filesystem::path dirPath = wdToPath[event->wd];
					std::filesystem::path fullPath = dirPath / event->name;

					FileStatus status = FileStatus::Modified;
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
						status = FileStatus::Created;
						// If it's a directory, add a watch for it
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (std::filesystem::is_directory(fullPath)) {
							int wd = inotify_add_watch(fd, fullPath.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
							if (wd != -1) wdToPath[wd] = fullPath;
						}
					}
					// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
					else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
						status = FileStatus::Erased;
						// If it's a directory (tracked), remove watch logic is implicit as the wd becomes invalid or we can just ignore
					}
					else if (event->mask & IN_MODIFY) status = FileStatus::Modified;

					QueueEvent(std::move(fullPath), status);
				}
			}
			i += sizeof(struct inotify_event) + event->len;
		}
	}
	close(fd);
}
#else
// FileWatcher::WatchLoopGeneric is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void FileWatcher::WatchLoopGeneric() {
	std::map<std::filesystem::path, std::filesystem::file_time_type> paths;
	
	// Initial Scan
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(pathData)) {
		for (auto& file : std::filesystem::recursive_directory_iterator(pathData)) {
			try {
				paths[file.path()] = std::filesystem::last_write_time(file);
			} catch (...) {}
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	while (running) {
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		// Check for erasures
		auto it = paths.begin();
		while (it != paths.end()) {
			if (!std::filesystem::exists(it->first)) {
				QueueEvent(it->first, FileStatus::Erased);
				it = paths.erase(it);
			} else {
				it++;
			}
		}

		// Check for created/modified
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::filesystem::exists(pathData)) {
			for (auto& file : std::filesystem::recursive_directory_iterator(pathData)) {
				try {
					auto current_time = std::filesystem::last_write_time(file);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (paths.find(file.path()) == paths.end()) {
						paths[file.path()] = current_time;
						QueueEvent(file.path(), FileStatus::Created);
					} else {
						// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
						if (paths[file.path()] != current_time) {
							paths[file.path()] = current_time;
							QueueEvent(file.path(), FileStatus::Modified);
						}
					}
				} catch (...) {
					continue;
				}
			}
		}
	}
}
#endif
