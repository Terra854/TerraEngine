// FileWatcher.cpp — Filesystem watcher implementation
//
// Implements the background watching logic using ReadDirectoryChangesW on Windows and inotify on Linux. It handles recursive directory monitoring and translates native filesystem events into generic status updates for the asset database.

#include "FileWatcher.h"
#include <iostream>
#include <map>
#include <chrono>
#include <algorithm>
#include <cstring>

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

void FileWatcher::ProcessWindowsCompletion(std::span<const std::byte> bytes, bool succeeded)
{
	// Stage copied events before queueing. Bounds checks keep even a truncated
	// native completion from reading bytes belonging to an earlier request.
	if (!succeeded || bytes.empty()) { FlagReconcileRequired(); return; }
	const auto malformed = [this] { FlagReconcileRequired(); };
	try {
	std::vector<FileEvent> decoded;
	std::size_t offset = 0U;
	for (;;) {
		constexpr std::size_t headerBytes = 12U;
		if (offset > bytes.size() || bytes.size() - offset < headerBytes) { malformed(); return; }
		std::uint32_t next{}, action{}, nameBytes{};
		std::memcpy(&next, bytes.data() + offset, 4U);
		std::memcpy(&action, bytes.data() + offset + 4U, 4U);
		std::memcpy(&nameBytes, bytes.data() + offset + 8U, 4U);
		if (nameBytes == 0U || nameBytes % 2U != 0U || nameBytes > bytes.size() - offset - headerBytes
			|| action < 1U || action > 5U) { malformed(); return; }
		std::u16string name(nameBytes / 2U, u'\0');
		std::memcpy(name.data(), bytes.data() + offset + headerBytes, nameBytes);
		// UTF-16 surrogate pairs must be complete before filesystem conversion;
		// embedded NULs and rooted/traversing names cannot name a watched child.
		for (std::size_t index = 0U; index < name.size(); ++index) {
			const auto value = name[index];
			if (value == 0U || (value >= 0xDC00U && value <= 0xDFFFU)) { malformed(); return; }
			if (value >= 0xD800U && value <= 0xDBFFU) {
				if (++index == name.size() || name[index] < 0xDC00U || name[index] > 0xDFFFU) { malformed(); return; }
			}
		}
		std::filesystem::path child;
		try { child = std::filesystem::path(name); } catch (...) { malformed(); return; }
		if (child.is_absolute() || child.has_root_name()
			|| std::find(child.begin(), child.end(), std::filesystem::path("..")) != child.end()) { malformed(); return; }
		const auto status = action == 1U || action == 5U ? FileStatus::Created
			: (action == 2U || action == 4U ? FileStatus::Erased : FileStatus::Modified);
		decoded.push_back({pathData / child, status});
		if (next == 0U) break;
		if (next % 4U != 0U || next < headerBytes + nameBytes || next > bytes.size() - offset
			|| bytes.size() - offset - next < headerBytes) { malformed(); return; }
		offset += next;
	}
	for (auto& event : decoded) QueueEvent(std::move(event.path), event.status);
	} catch (...) {
		// A conversion/allocation failure loses notification information too;
		// leave recovery visible rather than terminating the platform pump.
		malformed();
	}
}

void FileWatcher::ProcessLinuxLoss(bool succeeded, std::uint32_t mask)
{
	// The platform-neutral entry retains the native read disposition and mask
	// so recovery classification is exercised without creating an inotify fd.
	constexpr std::uint32_t queueOverflow = 0x00004000U;
	if (!succeeded || (mask & queueOverflow) != 0U) FlagReconcileRequired();
}


void FileWatcher::Start(const std::filesystem::path& path) {
	if (running) return;
	this->pathData = path;
	running = true;
	watchThread = std::thread(&FileWatcher::WatchLoop, this);
}


void FileWatcher::Stop() {
	running = false;
	if (watchThread.joinable()) {
		watchThread.join();
	}
}


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


	if (hDir == INVALID_HANDLE_VALUE) {
		FlagReconcileRequired();
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
	if (!success) FlagReconcileRequired();


	while (running) {
		DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 50); // Check every 50ms
		if (waitResult == WAIT_OBJECT_0) {

			const bool completed = GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE) != FALSE
				&& bytesReturned <= sizeof(buffer);
			ProcessWindowsCompletion(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer),
				completed ? std::min<std::size_t>(bytesReturned, sizeof(buffer)) : 0U), completed);
			ResetEvent(overlapped.hEvent);
			success = ReadDirectoryChangesW(
				hDir, buffer, sizeof(buffer), TRUE,
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
				NULL, &overlapped, NULL
			);

			if (!success) { FlagReconcileRequired(); break; }
		}
	}
	

	if (success) {
		CancelIo(hDir);
		GetOverlappedResult(hDir, &overlapped, &bytesReturned, TRUE);
	}
	
	CloseHandle(overlapped.hEvent);
	CloseHandle(hDir);
}
#elif defined(__linux__)


void FileWatcher::AddWatchesLinux(int fd, const std::filesystem::path& root, std::map<int, std::filesystem::path>& wdToPath) {
	// Add watch for the root
	int wd = inotify_add_watch(fd, root.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);

	if (wd != -1) {
		wdToPath[wd] = root;
	} else FlagReconcileRequired();
	
	// Recursively add watches for subdirectories

	for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
		if (p.is_directory()) {
			wd = inotify_add_watch(fd, p.path().c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);

			if (wd != -1) {
				wdToPath[wd] = p.path();
			} else FlagReconcileRequired();
		}
	}
}


void FileWatcher::WatchLoopLinux() {
	int fd = inotify_init1(IN_NONBLOCK);
	if (fd == -1) { FlagReconcileRequired(); return; }

	std::map<int, std::filesystem::path> wdToPath;
	AddWatchesLinux(fd, pathData, wdToPath);

	const int BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
	char buf[BUF_LEN];


	while (running) {
		int length = read(fd, buf, BUF_LEN);
		ProcessLinuxLoss(length >= 0 || errno == EAGAIN, 0U);
		if (length < 0) {

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

		while (i < length) {
			if (length - i < static_cast<int>(sizeof(inotify_event))) { FlagReconcileRequired(); break; }

			inotify_event header{};
			std::memcpy(&header, buf + i, sizeof(header));
			const auto* event = &header;
			if (event->len > static_cast<unsigned>(length - i - sizeof(header))) { FlagReconcileRequired(); break; }
			const char* name = buf + i + sizeof(header);
			if (event->len != 0U && std::memchr(name, '\0', event->len) == nullptr) { FlagReconcileRequired(); break; }
			ProcessLinuxLoss(true, event->mask);

			if (event->len) {

				if (wdToPath.find(event->wd) != wdToPath.end()) {
					std::filesystem::path dirPath = wdToPath[event->wd];
					std::filesystem::path fullPath = dirPath / name;

					FileStatus status = FileStatus::Modified;

					if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
						status = FileStatus::Created;
						// If it's a directory, add a watch for it

						if (std::filesystem::is_directory(fullPath)) {
							int wd = inotify_add_watch(fd, fullPath.c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
							if (wd != -1) wdToPath[wd] = fullPath;
						}
					}

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

void FileWatcher::WatchLoopGeneric() {
	std::map<std::filesystem::path, std::filesystem::file_time_type> paths;
	
	// Initial Scan

	if (std::filesystem::exists(pathData)) {
		for (auto& file : std::filesystem::recursive_directory_iterator(pathData)) {
			try {
				paths[file.path()] = std::filesystem::last_write_time(file);
			} catch (...) {}
		}
	}


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

		if (std::filesystem::exists(pathData)) {
			for (auto& file : std::filesystem::recursive_directory_iterator(pathData)) {
				try {
					auto current_time = std::filesystem::last_write_time(file);

					if (paths.find(file.path()) == paths.end()) {
						paths[file.path()] = current_time;
						QueueEvent(file.path(), FileStatus::Created);
					} else {

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
