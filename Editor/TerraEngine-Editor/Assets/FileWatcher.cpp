#include "FileWatcher.h"
#include <iostream>
#include <map>
#include <chrono>

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

    while (running) {
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 50); // Check every 50ms
        if (waitResult == WAIT_OBJECT_0) {
            if (GetOverlappedResult(hDir, &overlapped, &bytesReturned, FALSE)) {
                 FILE_NOTIFY_INFORMATION* pNotify;
                 int offset = 0;
                 do {
                     pNotify = (FILE_NOTIFY_INFORMATION*)((char*)buffer + offset);
                     
                     std::wstring filename(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
                     std::filesystem::path fullPath = pathData / filename;
                     
                     FileStatus status = FileStatus::Modified;
                     if (pNotify->Action == FILE_ACTION_ADDED || pNotify->Action == FILE_ACTION_RENAMED_NEW_NAME) status = FileStatus::Created;
                     else if (pNotify->Action == FILE_ACTION_REMOVED || pNotify->Action == FILE_ACTION_RENAMED_OLD_NAME) status = FileStatus::Erased;
                     else if (pNotify->Action == FILE_ACTION_MODIFIED) status = FileStatus::Modified;

                     {
                         std::lock_guard<std::mutex> lock(queueMutex);
                         eventQueue.push_back({fullPath, status});
                     }

                     offset += pNotify->NextEntryOffset;
                 } while (pNotify->NextEntryOffset != 0);
            }
            ResetEvent(overlapped.hEvent);
            success = ReadDirectoryChangesW(
                hDir, buffer, sizeof(buffer), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION,
                NULL, &overlapped, NULL
            );
            if (!success) break;
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
    }
    
    // Recursively add watches for subdirectories
    for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
        if (p.is_directory()) {
            wd = inotify_add_watch(fd, p.path().c_str(), IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
            if (wd != -1) {
                wdToPath[wd] = p.path();
            }
        }
    }
}

void FileWatcher::WatchLoopLinux() {
    int fd = inotify_init1(IN_NONBLOCK);
    if (fd == -1) return;

    std::map<int, std::filesystem::path> wdToPath;
    AddWatchesLinux(fd, pathData, wdToPath);

    const int BUF_LEN = (10 * (sizeof(struct inotify_event) + NAME_MAX + 1));
    char buf[BUF_LEN];

    while (running) {
        int length = read(fd, buf, BUF_LEN);
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
            struct inotify_event* event = (struct inotify_event*)&buf[i];
            if (event->len) {
                if (wdToPath.find(event->wd) != wdToPath.end()) {
                    std::filesystem::path dirPath = wdToPath[event->wd];
                    std::filesystem::path fullPath = dirPath / event->name;

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

                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        eventQueue.push_back({fullPath, status});
                    }
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
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    eventQueue.push_back({it->first, FileStatus::Erased});
                }
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
                        {
                            std::lock_guard<std::mutex> lock(queueMutex);
                            eventQueue.push_back({file.path(), FileStatus::Created});
                        }
                    } else {
                        if (paths[file.path()] != current_time) {
                            paths[file.path()] = current_time;
                            {
                                std::lock_guard<std::mutex> lock(queueMutex);
                                eventQueue.push_back({file.path(), FileStatus::Modified});
                            }
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
