// AtomicFileWrite.cpp — never opens the destination until the complete sibling is closed.
#include "Utilities/AtomicFileWrite.h"
#include "Utilities/UUID.h"
#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace TerraEngine::Utilities {
	namespace {
		std::mutex lockDirectoryMutex;
		std::unordered_map<std::wstring, std::weak_ptr<std::mutex>> lockDirectory;
		std::shared_ptr<std::mutex> LockFor(const std::filesystem::path& target) {
			auto key = target.wstring();
#if defined(_WIN32)
			std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return std::towlower(c); });
#endif
			std::lock_guard guard(lockDirectoryMutex);
			// Remove expired entries so saving many different assets does not retain
			// a process-lifetime list of every path ever encountered.
			for(auto it = lockDirectory.begin(); it != lockDirectory.end();) {
				if(it->second.expired()) it = lockDirectory.erase(it); else ++it;
			}
			auto& entry = lockDirectory[key];
			if(auto existing = entry.lock()) return existing;
			auto created = std::make_shared<std::mutex>(); entry = created; return created;
		}
		std::string NativeError() {
#if defined(_WIN32)
			return std::system_category().message(static_cast<int>(GetLastError()));
#else
			return std::generic_category().message(errno);
#endif
		}
		struct StagingOwner {
			std::filesystem::path path;
			bool ownsPath = false;
#if defined(_WIN32)
			HANDLE handle = INVALID_HANDLE_VALUE;
#else
			int handle = -1;
#endif
			// Cleanup affects only the exclusively acquired staging name, never the
			// existing destination. Destruction is a fallback for allocation failures.
			std::error_code Cleanup() noexcept {
				std::error_code error;
#if defined(_WIN32)
				if(handle != INVALID_HANDLE_VALUE) {
					if(!CloseHandle(handle)) error = {static_cast<int>(GetLastError()), std::system_category()};
					handle = INVALID_HANDLE_VALUE;
				}
#else
				if(handle >= 0) {
					if(::close(handle) != 0) error = {errno, std::generic_category()};
					handle = -1;
				}
#endif
				if(ownsPath) {
					try {
						std::error_code removalError; std::filesystem::remove(path, removalError);
						if(removalError) error = removalError; else ownsPath = false;
					} catch(...) { error = std::make_error_code(std::errc::not_enough_memory); }
				}
				return error;
			}
			~StagingOwner() { Cleanup(); }
		};
	}
	AtomicFileWriteTransaction::AtomicFileWriteTransaction(const std::filesystem::path& requested)
		: target(std::filesystem::absolute(requested).lexically_normal()), targetMutex(LockFor(target)), targetLock(*targetMutex) {}
	AtomicFileWriteTransaction::~AtomicFileWriteTransaction() = default;
	AtomicFileWriteResult AtomicFileWriteTransaction::Commit(std::string_view bytes, const AtomicFileWriteOptions& options) {
		AtomicFileWriteResult result;
		StagingOwner staging;
		try {
			// The suffix cannot be discovered as a .tescene file. CREATE_NEW/O_EXCL
			// is the authority for exclusive ownership, rather than a prior exists().
			staging.path = target.parent_path() / (target.filename().wstring() + L".terra-save-" +
				std::filesystem::path(ToHexString(GenerateUUID())).wstring() + L".tmp");
			auto ready = [&](AtomicFileWriteStage stage) {
				result.stage = stage;
				if(options.checkpoint) result.reason = options.checkpoint(stage, target);
				return result.reason.empty();
			};
			auto failure = [&]() {
				const auto cleanupError = staging.Cleanup();
				if(cleanupError) result.cleanupError = cleanupError.message();
				return result;
			};
			if(!ready(AtomicFileWriteStage::Open)) return failure();
#if defined(_WIN32)
			staging.handle = CreateFileW(staging.path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(staging.handle == INVALID_HANDLE_VALUE) { result.reason = NativeError(); return failure(); }
#else
			staging.handle = ::open(staging.path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
			if(staging.handle < 0) { result.reason = NativeError(); return failure(); }
#endif
			staging.ownsPath = true;
			if(!ready(AtomicFileWriteStage::Write)) return failure();
			std::size_t offset = 0;
			while(offset < bytes.size()) {
#if defined(_WIN32)
				const DWORD count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
				DWORD written = 0;
				if(!WriteFile(staging.handle, bytes.data() + offset, count, &written, nullptr)) {
					result.reason = NativeError(); return failure();
				}
				if(written == 0) { result.reason = "Staging write made no progress."; return failure(); }
#else
				const auto written = ::write(staging.handle, bytes.data() + offset, std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<ssize_t>::max()));
				if(written < 0 && errno == EINTR) continue;
				if(written <= 0) { result.reason = NativeError(); return failure(); }
#endif
				offset += static_cast<std::size_t>(written);
			}
			if(!ready(AtomicFileWriteStage::Flush)) return failure();
#if defined(_WIN32)
			if(!FlushFileBuffers(staging.handle)) { result.reason = NativeError(); return failure(); }
#else
			if(::fsync(staging.handle) != 0) { result.reason = NativeError(); return failure(); }
#endif
			if(!ready(AtomicFileWriteStage::Finalize)) return failure();
#if defined(_WIN32)
			LARGE_INTEGER size{};
			if(!GetFileSizeEx(staging.handle, &size)) { result.reason = NativeError(); return failure(); }
			const auto finalSize = static_cast<std::uint64_t>(size.QuadPart);
#else
			struct stat info{};
			if(::fstat(staging.handle, &info) != 0) { result.reason = NativeError(); return failure(); }
			const auto finalSize = static_cast<std::uint64_t>(info.st_size);
#endif
			if(finalSize != bytes.size()) { result.reason = "Staging length differs from the complete authored bytes."; return failure(); }
			if(!ready(AtomicFileWriteStage::Close)) return failure();
#if defined(_WIN32)
			if(!CloseHandle(staging.handle)) { result.reason = NativeError(); return failure(); }
			staging.handle = INVALID_HANDLE_VALUE;
#else
			const int closeResult = ::close(staging.handle); staging.handle = -1;
			if(closeResult != 0) { result.reason = NativeError(); return failure(); }
#endif
			if(!ready(AtomicFileWriteStage::Replace)) return failure();
			// The sibling is on the destination volume. There is deliberately no
			// delete-old step or copy fallback; an unsupported replacement fails.
#if defined(_WIN32)
			if(!MoveFileExW(staging.path.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				result.reason = NativeError(); return failure();
			}
#else
			if(::rename(staging.path.c_str(), target.c_str()) != 0) { result.reason = NativeError(); return failure(); }
#endif
			staging.ownsPath = false; result.committed = true; result.stage = AtomicFileWriteStage::Complete;
			return result;
		} catch(const std::exception& error) { result.reason = error.what(); }
		catch(...) { result.reason = "Unknown filesystem staging failure."; }
		const auto cleanupError = staging.Cleanup();
		if(cleanupError) result.cleanupError = cleanupError.message();
		return result;
	}
}
