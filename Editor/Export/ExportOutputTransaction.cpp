// ExportOutputTransaction.cpp - Validated staging and conservative output overlay.

#include "ExportOutputTransaction.h"
#include "ExportTypes.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <algorithm>
#include <exception>

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#else
	#include <cerrno>
	#include <csignal>
	#include <unistd.h>
	#include <sys/stat.h>
#endif

namespace {
	constexpr std::string_view kLockMetadataFileName = "owner.txt";
	constexpr uint32_t kLockMetadataSchemaVersion = 1;
	std::atomic<uint64_t> g_OwnerTokenCounter = 0;

	/// Returns a native I/O spelling that bypasses the legacy MAX_PATH parser
	/// while retaining prefix-free logical paths in public APIs and lock metadata.
	/// This is intentionally defensive because this library can be hosted by
	/// third-party tools that do not carry TerraEngine's longPathAware manifest.
	std::filesystem::path NativeIoPath(const std::filesystem::path& path) {
#if defined(_WIN32)
		if(path.empty()) {
			return path;
		}
		std::filesystem::path absolutePath = path;
		if(!absolutePath.is_absolute()) {
			std::error_code error;
			absolutePath = std::filesystem::absolute(absolutePath, error);
			if(error) {
				return path;
			}
		}
		std::filesystem::path preferred = absolutePath.lexically_normal();
		preferred.make_preferred();
		const std::filesystem::path::string_type& native = preferred.native();
		constexpr std::wstring_view extendedPrefix = LR"(\\?\)";
		constexpr std::wstring_view devicePrefix = LR"(\\.\)";
		constexpr std::wstring_view uncPrefix = LR"(\\)";
		if(native.starts_with(extendedPrefix) || native.starts_with(devicePrefix)) {
			return preferred;
		}

		std::filesystem::path::string_type extended;
		if(native.starts_with(uncPrefix)) {
			extended.assign(LR"(\\?\UNC\)");
			extended.append(native.substr(uncPrefix.size()));
		} else {
			extended.assign(extendedPrefix);
			extended.append(native);
		}
		return std::filesystem::path(std::move(extended));
#else
		return path;
#endif
	}

	// Inspect entries without following redirects. A junction or symbolic link
	// inside an output overlay must never redirect writes to unrelated content.
	std::filesystem::file_type PublicationType(const std::filesystem::path& path, std::error_code& error) {
		const auto status = std::filesystem::symlink_status(NativeIoPath(path), error);
		if(status.type() == std::filesystem::file_type::not_found) { error.clear(); return status.type(); }
		if(error) return std::filesystem::file_type::none;
#if defined(_WIN32)
		const DWORD attributes = GetFileAttributesW(NativeIoPath(path).c_str());
		if(attributes == INVALID_FILE_ATTRIBUTES) { error = std::error_code(static_cast<int>(GetLastError()), std::system_category()); return std::filesystem::file_type::none; }
		if((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) { error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return std::filesystem::file_type::none; }
#endif
		if(std::filesystem::is_symlink(status)) { error = std::make_error_code(std::errc::too_many_symbolic_link_levels); return std::filesystem::file_type::none; }
		return status.type();
	}

	bool IsPublicationVolume(const std::filesystem::path& scratch, const std::filesystem::path& directory, std::error_code& error) {
#if !defined(_WIN32)
		// Mount points can be ordinary POSIX directories. Reject cross-device
		// targets before publishing, because rename would not be atomic there.
		struct stat source{}, destination{};
		if(::stat(scratch.c_str(), &source) != 0 || ::stat(directory.c_str(), &destination) != 0) {
			error = std::error_code(errno, std::generic_category()); return false;
		}
		if(source.st_dev != destination.st_dev) { error = std::make_error_code(std::errc::cross_device_link); return false; }
#else
		// All Windows mount/junction reparse entries below the canonical output
		// root are rejected by PublicationType; scratch is its sibling volume.
		(void)scratch; (void)directory; (void)error;
#endif
		return true;
	}

	// Both paths are private siblings on the output volume. Never truncate the
	// visible destination: an unsuccessful replace leaves its old bytes intact.
	void ReplacePublicationFile(const std::filesystem::path& candidate, const std::filesystem::path& destination, std::error_code& error) {
		error.clear();
#if defined(_WIN32)
		if(!MoveFileExW(NativeIoPath(candidate).c_str(), NativeIoPath(destination).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
			error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
#else
		std::filesystem::rename(NativeIoPath(candidate), NativeIoPath(destination), error);
#endif
	}

	std::string PathToUtf8(const std::filesystem::path& path);
	bool HasPendingPublicationJournal(const std::filesystem::path& scratch, std::error_code& error,
		bool* completed, std::string_view runId, std::string_view expectedOwner = {}) {
		if(completed) *completed = false;
		const auto type = PublicationType(scratch, error);
		if(error || type == std::filesystem::file_type::not_found) return false;
		if(type != std::filesystem::file_type::directory) { error = std::make_error_code(std::errc::not_a_directory); return false; }
		for(std::filesystem::directory_iterator iterator(NativeIoPath(scratch), error), end; iterator != end && !error; iterator.increment(error)) {
			if(iterator->path().filename().native().starts_with(std::filesystem::path(".publication-").native())) return true;
			// Only Commit's atomic directory rename enters this namespace. Unlike
			// a file inside the journal, the name survives partially failed recursive
			// cleanup: remaining originals can never become rollback evidence again.
			const auto prefix = std::filesystem::path(".completed-publication-").native();
			if(iterator->path().filename().native().starts_with(prefix)) {
				const auto name = PathToUtf8(iterator->path().filename());
				const auto token = std::string_view(name).substr(std::string_view(".completed-publication-").size());
				// Generic same-run cleanup accepts only the format emitted by
				// MakeOwnerToken, never a foreign/malformed completed-looking name.
				bool valid = token.starts_with(std::string(runId) + "-");
				std::string_view numbers = valid ? token.substr(runId.size() + 1) : std::string_view{};
				for(unsigned field = 0; field != 3 && valid; ++field) {
					const auto end = numbers.find('-');
					const auto number = numbers.substr(0, end);
					std::uint64_t value = 0;
					const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
					valid = !number.empty() && parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size() && value != 0
						&& (field == 2 ? end == std::string_view::npos : end != std::string_view::npos);
					if(valid && end != std::string_view::npos) numbers.remove_prefix(end + 1);
				}
				if(!valid
					|| PublicationType(iterator->path(), error) != std::filesystem::file_type::directory || error) {
					if(!error) error = std::make_error_code(std::errc::invalid_argument);
					return true;
				}
				// A completion from an older same-run owner is disposable, but cannot
				// authorize the current dead owner's backup deletion or lock release.
				if(completed && (expectedOwner.empty() || token == expectedOwner)) *completed = true;
			}
		}
		return false;
	}

	/// Converts a path to portable UTF-8 without depending on the process locale.
	std::string PathToUtf8(const std::filesystem::path& path) {
		const std::u8string encoded = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(encoded.data()), encoded.size());
	}

	/// Reconstructs a native path from the UTF-8 representation stored in a lock.
	std::filesystem::path PathFromUtf8(const std::string& text) {
		std::u8string encoded;
		encoded.reserve(text.size());
		for(const unsigned char value : text) {
			encoded.push_back(static_cast<char8_t>(value));
		}
		return std::filesystem::path(encoded);
	}

	/// Appends ASCII contract text to the platform-native path string type.
	void AppendAscii(std::filesystem::path::string_type& target, std::string_view text) {
		for(const char value : text) {
			target.push_back(static_cast<std::filesystem::path::value_type>(value));
		}
	}

	/// Creates one reserved sibling name while preserving non-ASCII output names.
	std::filesystem::path MakeTransactionSibling(
		const std::filesystem::path& finalDirectory,
		std::string_view runId,
		std::string_view suffix) {
		std::filesystem::path::string_type name;
		name.push_back(static_cast<std::filesystem::path::value_type>('.'));
		name.append(finalDirectory.filename().native());
		AppendAscii(name, ".export-");
		AppendAscii(name, runId);
		AppendAscii(name, suffix);
		return finalDirectory.parent_path() / std::filesystem::path(std::move(name));
	}

	/// The lock name is stable for an output, unlike per-run work directories.
	std::filesystem::path MakeLockPath(const std::filesystem::path& finalDirectory) {
		std::filesystem::path::string_type name;
		name.push_back(static_cast<std::filesystem::path::value_type>('.'));
		name.append(finalDirectory.filename().native());
		AppendAscii(name, ".export.lock");
		return finalDirectory.parent_path() / std::filesystem::path(std::move(name));
	}

	/// Run ids appear in filenames, so path separators and control characters are
	/// replaced. The original run id remains available in higher-level evidence.
	std::string SanitizeRunId(std::string runId) {
		if(runId.empty()) {
			return "run";
		}

		for(char& value : runId) {
			const bool alphaNumeric = (value >= 'a' && value <= 'z')
				|| (value >= 'A' && value <= 'Z')
				|| (value >= '0' && value <= '9');
			if(!alphaNumeric && value != '-' && value != '_') {
				value = '_';
			}
		}

		// Bounded names avoid exceeding Windows path component limits when a CI
		// caller accidentally supplies a long descriptive run id.
		constexpr std::size_t kMaximumRunIdLength = 64;
		if(runId.size() > kMaximumRunIdLength) {
			runId.resize(kMaximumRunIdLength);
		}
		return runId;
	}

	uint64_t CurrentProcessId() {
#if defined(_WIN32)
		return static_cast<uint64_t>(::GetCurrentProcessId());
#else
		return static_cast<uint64_t>(::getpid());
#endif
	}

	/// A failed owner query is treated conservatively. Access-denied means the
	/// process may still exist, while an invalid/nonexistent id is safely stale.
	bool IsProcessActive(uint64_t processId) {
		if(processId == 0) {
			return false;
		}

#if defined(_WIN32)
		if(processId > static_cast<uint64_t>(std::numeric_limits<DWORD>::max())) {
			return false;
		}
		HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
		if(process == nullptr) {
			return ::GetLastError() == ERROR_ACCESS_DENIED;
		}
		const DWORD waitResult = ::WaitForSingleObject(process, 0);
		::CloseHandle(process);
		return waitResult == WAIT_TIMEOUT;
#else
		if(processId > static_cast<uint64_t>(std::numeric_limits<pid_t>::max())) {
			return false;
		}
		errno = 0;
		const int result = ::kill(static_cast<pid_t>(processId), 0);
		return result == 0 || errno == EPERM;
#endif
	}

	int64_t CurrentUnixSeconds() {
		return std::chrono::duration_cast<std::chrono::seconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	std::string MakeOwnerToken(std::string_view runId) {
		const uint64_t sequence = g_OwnerTokenCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		const uint64_t monotonicTicks = static_cast<uint64_t>(
			std::chrono::steady_clock::now().time_since_epoch().count());
		std::ostringstream stream;
		stream << runId << '-' << CurrentProcessId() << '-' << monotonicTicks << '-' << sequence;
		return stream.str();
	}

	template<typename Number>
	bool ParseNumber(std::string_view text, Number& value) {
		const char* begin = text.data();
		const char* end = begin + text.size();
		const auto result = std::from_chars(begin, end, value);
		return result.ec == std::errc() && result.ptr == end;
	}

	bool ParseQuotedString(const std::string& text, std::string& value) {
		std::istringstream stream(text);
		stream >> std::quoted(value);
		return !stream.fail();
	}

	bool NativeEndsWith(
		const std::filesystem::path::string_type& value,
		const std::filesystem::path::string_type& suffix) {
		return value.size() >= suffix.size()
			&& value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
	}
}

struct ExportOutputTransaction::LockMetadata {
	uint32_t schemaVersion = 0;
	uint64_t processId = 0;
	int64_t createdUnixSeconds = 0;
	std::string runId;
	std::string ownerToken;
	std::filesystem::path finalDirectory;
	std::filesystem::path stagingDirectory;
	std::filesystem::path scratchDirectory;
	std::filesystem::path backupDirectory;
	std::string state;
};

std::string_view ToString(ExportOutputTransactionState state) {
	switch(state) {
		case ExportOutputTransactionState::Created:
			return "Created";
		case ExportOutputTransactionState::Locked:
			return "Locked";
		case ExportOutputTransactionState::StagingPrepared:
			return "StagingPrepared";
		case ExportOutputTransactionState::StagedAndValidated:
			return "StagedAndValidated";
		case ExportOutputTransactionState::PreviousOutputBackedUp:
			return "PreviousOutputBackedUp";
		case ExportOutputTransactionState::Published:
			return "Published";
		case ExportOutputTransactionState::Cleaned:
			return "Cleaned";
		case ExportOutputTransactionState::RolledBack:
			return "RolledBack";
	}
	return "Created";
}

ExportOutputTransaction::ExportOutputTransaction(
	std::filesystem::path finalDirectory,
	std::string runId,
	ExportOutputTransactionOptions options)
	: m_RunId(SanitizeRunId(std::move(runId))),
	  m_Options(options) {
	std::error_code pathError;
	std::filesystem::path absolutePath = std::filesystem::absolute(finalDirectory, pathError);
	if(pathError) {
		m_InitializationError = "output_path_invalid: Could not make the final output path absolute: " + pathError.message();
		absolutePath = std::move(finalDirectory);
	}

	// weakly_canonical resolves an existing prefix while still accepting a new
	// final directory. This gives every caller the same lock key for the output.
	std::error_code canonicalError;
	const std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absolutePath, canonicalError);
	m_FinalDirectory = (canonicalError ? absolutePath : canonicalPath).lexically_normal();
	if(m_FinalDirectory.empty() || m_FinalDirectory.filename().empty()) {
		m_InitializationError = "output_path_invalid: The final output must name a directory below a parent directory.";
		return;
	}

	m_StagingDirectory = MakeTransactionSibling(m_FinalDirectory, m_RunId, ".staging");
	m_ScratchDirectory = MakeTransactionSibling(m_FinalDirectory, m_RunId, ".scratch");
	m_BackupDirectory = MakeTransactionSibling(m_FinalDirectory, m_RunId, ".backup");
	m_LockPath = MakeLockPath(m_FinalDirectory);
	m_OwnerToken = MakeOwnerToken(m_RunId);
}

ExportOutputTransaction::~ExportOutputTransaction() {
	RollbackBestEffort();
}

bool ExportOutputTransaction::Acquire(std::string& error) {
	error.clear();
	if(!m_InitializationError.empty()) {
		error = m_InitializationError;
		return false;
	}
	if(m_State != ExportOutputTransactionState::Created) {
		error = "output_lock_state_invalid: The output lock may only be acquired once.";
		return false;
	}

	std::error_code parentError;
	std::filesystem::create_directories(NativeIoPath(m_FinalDirectory.parent_path()), parentError);
	if(parentError) {
		error = "output_parent_unavailable: Could not create the output parent directory: " + parentError.message();
		return false;
	}

	std::error_code typeError;
	if(std::filesystem::exists(NativeIoPath(m_FinalDirectory), typeError)
		&& !std::filesystem::is_directory(NativeIoPath(m_FinalDirectory), typeError)) {
		error = "output_path_invalid: The final output path exists but is not a directory.";
		return false;
	}
	if(typeError) {
		error = "output_path_invalid: Could not inspect the final output path: " + typeError.message();
		return false;
	}

	if(!TryCreateLock(error)) {
		if(!error.empty()) {
			return false;
		}
		if(!m_Options.recoverStaleLocks) {
			error = "output_lock_conflict: Another export already owns the requested output.";
			return false;
		}
		if(!RecoverExistingLock(error)) {
			return false;
		}
		if(!TryCreateLock(error)) {
			if(error.empty()) {
				error = "output_lock_conflict: Another export acquired the output while stale-lock recovery completed.";
			}
			return false;
		}
	}

	m_OwnsLock = true;
	m_State = ExportOutputTransactionState::Locked;
	if(!WriteLockMetadata(error)) {
		// No other process can own this directory yet, so a failed initial metadata
		// write may safely remove the lock that this object just created.
		std::error_code cleanupError;
		std::filesystem::remove_all(NativeIoPath(m_LockPath), cleanupError);
		m_OwnsLock = false;
		m_State = ExportOutputTransactionState::RolledBack;
		return false;
	}
	return true;
}

bool ExportOutputTransaction::Prepare(std::string& error) {
	error.clear();
	if(m_State != ExportOutputTransactionState::Locked || !m_OwnsLock) {
		error = "output_staging_state_invalid: Prepare requires an owned output lock.";
		return false;
	}
	return PrepareRunDirectories(error);
}

bool ExportOutputTransaction::Begin(std::string& error) {
	if(!Acquire(error)) {
		return false;
	}
	if(Prepare(error)) {
		return true;
	}

	std::string rollbackError;
	if(!Rollback(rollbackError) && !rollbackError.empty()) {
		error += " " + rollbackError;
	}
	return false;
}

bool ExportOutputTransaction::MarkStagedAndValidated(std::string& error) {
	error.clear();
	if(m_State != ExportOutputTransactionState::StagingPrepared || !m_OwnsLock) {
		error = "output_validation_state_invalid: Validation may only complete after staging is prepared.";
		return false;
	}

	std::error_code directoryError;
	if(!std::filesystem::is_directory(NativeIoPath(m_StagingDirectory), directoryError) || directoryError) {
		error = "output_staging_missing: The transaction staging directory disappeared before validation completed.";
		return false;
	}

	m_State = ExportOutputTransactionState::StagedAndValidated;
	if(!WriteLockMetadata(error)) {
		m_State = ExportOutputTransactionState::StagingPrepared;
		return false;
	}
	return true;
}

bool ExportOutputTransaction::Commit(std::string& error, const std::shared_ptr<ExportCancellationState>& cancellation) {
	if (!cancellation) return Commit(error);
	const bool committed = cancellation->Commit([&] { return Commit(error); });
	if (!committed && cancellation->requested.load(std::memory_order_relaxed)) error = "Export canceled before output publication.";
	return committed;
}

bool ExportOutputTransaction::Commit(std::string& error) {
	// Allocations and path conversions can throw after an earlier file publishes.
	// Unwind the tracked prefix without replacing that original exception with a
	// secondary cleanup exception. Unrestored originals remain in the journal.
	struct ExceptionRollback {
		ExportOutputTransaction& owner;
		int entered = std::uncaught_exceptions();
		~ExceptionRollback() noexcept {
			if(std::uncaught_exceptions() <= entered) return;
			try { std::string failure; if(!owner.RollbackPublication(failure)) owner.AppendCleanupWarning(std::move(failure)); }
			catch(...) { /* Preserve the original exception and retryable journal. */ }
		}
	} exceptionRollback{*this};
	error.clear();
	if(m_State != ExportOutputTransactionState::StagedAndValidated || !m_OwnsLock) {
		error = "output_publication_state_invalid: Commit requires a validated staging directory and an owned lock.";
		return false;
	}

	if(m_PublicationJournalOwned) {
		error = "output_publication_state_invalid: Previous publication rollback is still pending.";
		return false;
	}
	std::error_code publishError;
	const auto rootType = PublicationType(m_FinalDirectory, publishError);
	if(publishError || (rootType != std::filesystem::file_type::not_found && rootType != std::filesystem::file_type::directory)) {
		error = "output_publication_failed: Final output must be an ordinary directory.";
		return false;
	}
	m_HadPreviousOutput = rootType != std::filesystem::file_type::not_found;
	if(PublicationType(m_StagingDirectory, publishError) != std::filesystem::file_type::directory || publishError) {
		error = "output_publication_failed: Validated staging is missing or redirected.";
		return false;
	}
	// ExportPipeline deliberately removes packaging scratch before validation.
	// Recreate only this owned, reserved sibling when absent; never follow a
	// replacement junction or adopt a conflicting entry as a journal parent.
	if(!IsSafeTransactionSibling(m_ScratchDirectory, ".scratch")) {
		error = "output_publication_failed: Publication scratch is outside its reserved sibling.";
		return false;
	}
	const auto scratchType = PublicationType(m_ScratchDirectory, publishError);
	if(!publishError && scratchType == std::filesystem::file_type::not_found) {
		if(!std::filesystem::create_directory(NativeIoPath(m_ScratchDirectory), publishError) && !publishError)
			publishError = std::make_error_code(std::errc::file_exists);
	} else if(!publishError && scratchType != std::filesystem::file_type::directory) {
		publishError = std::make_error_code(std::errc::not_a_directory);
	}
	if(publishError) {
		error = "output_publication_failed: Could not safely reserve publication scratch: " + publishError.message();
		return false;
	}

	// Preflight the complete overlay before creating even one final directory.
	// Unmentioned destination content is never enumerated, copied, or removed.
	std::vector<std::filesystem::path> directories{m_FinalDirectory}, files;
	std::vector<PublicationDirectoryTime> directoryTimes;
	for(std::filesystem::recursive_directory_iterator iterator(NativeIoPath(m_StagingDirectory), publishError), end;
		iterator != end && !publishError; iterator.increment(publishError)) {
		const auto type = PublicationType(iterator->path(), publishError);
		if(publishError) break;
		const auto relative = iterator->path().lexically_relative(NativeIoPath(m_StagingDirectory));
		if(type == std::filesystem::file_type::directory) directories.push_back(m_FinalDirectory / relative);
		else if(type == std::filesystem::file_type::regular) files.push_back(relative);
		else { publishError = std::make_error_code(std::errc::operation_not_supported); break; }
	}
	std::sort(directories.begin(), directories.end());
	std::sort(files.begin(), files.end());
	for(const auto& directory : directories) {
		if(publishError) break;
		const auto type = PublicationType(directory, publishError);
		if(!publishError && type != std::filesystem::file_type::not_found && type != std::filesystem::file_type::directory)
			publishError = std::make_error_code(std::errc::not_a_directory);
		if(!publishError && type == std::filesystem::file_type::directory) {
			if(!IsPublicationVolume(m_ScratchDirectory, directory, publishError)) break;
			const auto time = std::filesystem::last_write_time(NativeIoPath(directory), publishError);
			if(!publishError) directoryTimes.push_back({directory, time});
		}
	}
	for(const auto& relative : files) {
		if(publishError) break;
		const auto type = PublicationType(m_FinalDirectory / relative, publishError);
		if(!publishError && type != std::filesystem::file_type::not_found && type != std::filesystem::file_type::regular)
			publishError = std::make_error_code(std::errc::invalid_argument);
	}
	if(publishError) {
		error = "output_publication_failed: Unsafe overlay path or type conflict: " + publishError.message();
		return false;
	}

	const auto journal = m_ScratchDirectory / (".publication-" + m_OwnerToken);
	// Copy the path before creation; acquiring ownership afterward is a no-throw
	// bool update. A failed exclusive create never owns an existing journal.
	m_PublicationJournalDirectory = journal;
	if(!std::filesystem::create_directory(NativeIoPath(journal), publishError) || publishError) {
		m_PublicationJournalDirectory.clear();
		error = "output_publication_failed: Could not exclusively reserve publication journal: " + publishError.message();
		return false;
	}
	m_PublicationJournalOwned = true;
	m_PublicationDirectoryTimes = std::move(directoryTimes);
	m_PublicationFiles.reserve(files.size());
	for(const auto& relative : files) {
		PublicationFile entry;
		entry.destination = m_FinalDirectory / relative;
		entry.replacement = journal / (std::to_string(m_PublicationFiles.size()) + ".candidate");
		entry.original = journal / (std::to_string(m_PublicationFiles.size()) + ".original");
		const auto type = PublicationType(entry.destination, publishError);
		if(publishError) break;
		entry.hadOriginal = type == std::filesystem::file_type::regular;
		if(entry.hadOriginal) {
			entry.originalTime = std::filesystem::last_write_time(NativeIoPath(entry.destination), publishError);
			if(!publishError) std::filesystem::copy_file(NativeIoPath(entry.destination), NativeIoPath(entry.original), std::filesystem::copy_options::none, publishError);
		}
		if(!publishError) std::filesystem::copy_file(NativeIoPath(m_StagingDirectory / relative), NativeIoPath(entry.replacement), std::filesystem::copy_options::none, publishError);
		if(publishError) break;
		m_PublicationFiles.push_back(std::move(entry));
	}
	// Allocate every ownership record before the first final-tree mutation.
	// Merely reserving vector capacity would still allow a path copy to throw
	// after mkdir. Once mkdir succeeds, marking its bool cannot allocate.
	m_PublicationDirectories.reserve(directories.size());
	for(const auto& directory : directories) m_PublicationDirectories.push_back({directory, false});
	for(auto& directory : m_PublicationDirectories) {
		if(publishError) break;
		const auto type = PublicationType(directory.path, publishError);
		if(publishError) break;
		if(type == std::filesystem::file_type::not_found) {
			if(std::filesystem::create_directory(NativeIoPath(directory.path), publishError)) directory.created = true;
			else if(!publishError) publishError = std::make_error_code(std::errc::file_exists);
		} else if(type != std::filesystem::file_type::directory) publishError = std::make_error_code(std::errc::not_a_directory);
	}
	for(auto& entry : m_PublicationFiles) {
		if(publishError) break;
		const auto type = PublicationType(entry.destination, publishError);
		if(!publishError && type != (entry.hadOriginal ? std::filesystem::file_type::regular : std::filesystem::file_type::not_found))
			publishError = std::make_error_code(std::errc::file_exists);
		if(!publishError) ReplacePublicationFile(entry.replacement, entry.destination, publishError);
		if(!publishError) entry.published = true;
	}
	if(publishError) {
		error = "output_publication_failed: Could not publish validated overlay: " + publishError.message();
		std::string rollbackError;
		if(!RollbackPublication(rollbackError)) error += " " + rollbackError;
		return false;
	}
	// Persist the decision before releasing either journal ownership or the lock.
	// Completion is a sibling-directory rename, so partial cleanup cannot erase
	// a marker first and misclassify leftover originals as an interrupted commit.
	const auto completedJournal = m_ScratchDirectory / (".completed-publication-" + m_OwnerToken);
	if(PublicationType(completedJournal, publishError) != std::filesystem::file_type::not_found && !publishError)
		publishError = std::make_error_code(std::errc::file_exists);
	if(!publishError) ReplacePublicationFile(journal, completedJournal, publishError);
	if(publishError) {
		error = "output_publication_failed: Could not record completed publication: " + publishError.message();
		std::string rollbackError;
		if(!RollbackPublication(rollbackError)) error += " " + rollbackError;
		return false;
	}
	// Once every replacement succeeds, the originals are cleanup-only. A later
	// Rollback must retain this published build rather than undoing success.
	m_PublicationFiles.clear();
	m_PublicationDirectories.clear();
	m_PublicationDirectoryTimes.clear();
	m_PublicationJournalDirectory.clear();
	m_PublicationJournalOwned = false;

	m_WasPublished = true;
	m_State = ExportOutputTransactionState::Published;
	try {
		std::string metadataWarning;
		if(!WriteLockMetadata(metadataWarning)) {
			AppendCleanupWarning(std::move(metadataWarning));
		}

		// Publication is already complete. Staging, scratch, and lock cleanup failures
		// are retained as warnings instead of incorrectly claiming the output failed.
		std::string cleanupWarning;
		if(!CleanupPath(m_StagingDirectory, cleanupWarning)) {
			AppendCleanupWarning(std::move(cleanupWarning));
		}
		cleanupWarning.clear();
		if(!CleanupPath(m_ScratchDirectory, cleanupWarning)) {
			AppendCleanupWarning(std::move(cleanupWarning));
		}
	} catch(...) {
		// All files are committed now. Even allocation failure in diagnostics or
		// cleanup must not turn that success into a failed publication callback.
		try { AppendCleanupWarning("output_cleanup_failed: Cleanup raised an exception after publication; leftovers remain retryable."); }
		catch(...) { /* Reporting itself may be out of memory; keep success true. */ }
	}
	// Even a throwing cleanup/diagnostic must still attempt the independent lock
	// release. A completed journal is safe for a later owner to clean or retry.
	try {
		std::string releaseWarning;
		if(!ReleaseOwnedLock(releaseWarning)) AppendCleanupWarning(std::move(releaseWarning));
	} catch(...) {
		// Ownership stays explicit if even reading/removing lock metadata fails.
		// The destructor or stale-lock recovery may retry; publication stays true.
	}
	m_State = ExportOutputTransactionState::Cleaned;
	return true;
}

bool ExportOutputTransaction::RollbackPublication(std::string& error) {
	error.clear();
	if(m_WasPublished || !m_PublicationJournalOwned) return true;
	const auto remember = [&](const std::filesystem::path& path, const std::error_code& failure) {
		if(!failure) return;
		if(!error.empty()) error += " ";
		error += "output_rollback_failed: '" + PathToUtf8(path) + "': " + failure.message();
	};
	for(auto entry = m_PublicationFiles.rbegin(); entry != m_PublicationFiles.rend(); ++entry) {
		if(!entry->published && !entry->restoreTimePending) continue;
		std::error_code failure;
		const auto type = PublicationType(entry->destination, failure);
		if(!failure && type != std::filesystem::file_type::regular && type != std::filesystem::file_type::not_found)
			failure = std::make_error_code(std::errc::invalid_argument);
		if(!failure && entry->published) {
			if(entry->hadOriginal) {
				ReplacePublicationFile(entry->original, entry->destination, failure);
				if(!failure) entry->restoreTimePending = true;
			} else std::filesystem::remove(NativeIoPath(entry->destination), failure);
			if(!failure) entry->published = false;
		}
		if(!failure && entry->restoreTimePending) {
			std::filesystem::last_write_time(NativeIoPath(entry->destination), entry->originalTime, failure);
			if(!failure) entry->restoreTimePending = false;
		}
		remember(entry->destination, failure);
	}
	for(auto directory = m_PublicationDirectories.rbegin(); directory != m_PublicationDirectories.rend(); ++directory) {
		if(!directory->created) continue;
		std::error_code failure;
		// Non-recursive removal preserves any unrelated content introduced by
		// another owner. A secondary failure retains the journal and owned lock.
		const auto type = PublicationType(directory->path, failure);
		if(!failure && type == std::filesystem::file_type::directory) std::filesystem::remove(NativeIoPath(directory->path), failure);
		else if(!failure && type != std::filesystem::file_type::not_found) failure = std::make_error_code(std::errc::invalid_argument);
		if(!failure) directory->created = false;
		remember(directory->path, failure);
	}
	for(const auto& directory : m_PublicationDirectoryTimes) {
		std::error_code failure;
		if(PublicationType(directory.path, failure) != std::filesystem::file_type::directory && !failure) failure = std::make_error_code(std::errc::not_a_directory);
		if(!failure) std::filesystem::last_write_time(NativeIoPath(directory.path), directory.originalTime, failure);
		remember(directory.path, failure);
	}
	if(!error.empty()) return false;
	if(!CleanupPath(m_PublicationJournalDirectory, error)) { error = "output_rollback_failed: " + error; return false; }
	m_PublicationFiles.clear(); m_PublicationDirectories.clear(); m_PublicationDirectoryTimes.clear(); m_PublicationJournalDirectory.clear();
	m_PublicationJournalOwned = false;
	return true;
}

bool ExportOutputTransaction::Rollback(std::string& error) {
	error.clear();
	if(m_RequiresManualRecovery) {
		error = "output_rollback_failed: An orphan publication journal requires manual recovery; originals and lock were retained.";
		return false;
	}
	if(m_State == ExportOutputTransactionState::Created
		|| ((m_State == ExportOutputTransactionState::Cleaned
			|| m_State == ExportOutputTransactionState::RolledBack) && !m_OwnsLock)) {
		return true;
	}
	if(m_CompletedJournalCleanupOnly) {
		// No new staging/publication started: only a previously committed run's
		// disposable leftovers were found. Cleanup can be retried, but its failure
		// must not retain a fresh lock and block every other export indefinitely.
		std::string cleanupError, releaseError;
		const bool cleaned = CleanupPath(m_StagingDirectory, cleanupError) && CleanupPath(m_ScratchDirectory, cleanupError);
		const bool released = ReleaseOwnedLock(releaseError);
		if(released) m_State = ExportOutputTransactionState::RolledBack;
		error = std::move(cleanupError);
		if(!releaseError.empty()) { if(!error.empty()) error += " "; error += releaseError; }
		return cleaned && released;
	}
	if(!RollbackPublication(error)) return false;

	std::error_code backupExistsError;
	const bool backupExists = std::filesystem::exists(NativeIoPath(m_BackupDirectory), backupExistsError);
	if(backupExistsError) {
		error = "output_rollback_failed: Could not inspect the backup directory: " + backupExistsError.message();
		return false;
	}
	if(!m_WasPublished && backupExists) {
		if(!RestoreBackup(error)) {
			return false;
		}
	}

	std::string cleanupError;
	if(!CleanupPath(m_StagingDirectory, cleanupError)) {
		error = "output_rollback_failed: " + cleanupError;
		return false;
	}
	if(!CleanupPath(m_ScratchDirectory, cleanupError)) {
		error = "output_rollback_failed: " + cleanupError;
		return false;
	}
	if(!m_WasPublished && !CleanupPath(m_BackupDirectory, cleanupError)) {
		error = "output_rollback_failed: " + cleanupError;
		return false;
	}

	if(m_OwnsLock) {
		std::string releaseError;
		if(!ReleaseOwnedLock(releaseError)) {
			error = "output_rollback_failed: " + releaseError;
			return false;
		}
	}
	m_State = m_WasPublished ? ExportOutputTransactionState::Cleaned : ExportOutputTransactionState::RolledBack;
	return true;
}

const std::filesystem::path& ExportOutputTransaction::FinalDirectory() const {
	return m_FinalDirectory;
}

const std::filesystem::path& ExportOutputTransaction::StagingDirectory() const {
	return m_StagingDirectory;
}

const std::filesystem::path& ExportOutputTransaction::ScratchDirectory() const {
	return m_ScratchDirectory;
}

const std::filesystem::path& ExportOutputTransaction::BackupDirectory() const {
	return m_BackupDirectory;
}

const std::filesystem::path& ExportOutputTransaction::LockPath() const {
	return m_LockPath;
}

std::filesystem::path ExportOutputTransaction::LockMetadataPath() const {
	return m_LockPath / kLockMetadataFileName;
}

ExportOutputTransactionState ExportOutputTransaction::State() const {
	return m_State;
}

bool ExportOutputTransaction::OwnsLock() const {
	return m_OwnsLock;
}

bool ExportOutputTransaction::HadPreviousOutput() const {
	return m_HadPreviousOutput;
}

bool ExportOutputTransaction::WasPublished() const {
	return m_WasPublished;
}

bool ExportOutputTransaction::RecoveredStaleLock() const {
	return m_RecoveredStaleLock;
}

const std::string& ExportOutputTransaction::CleanupWarning() const {
	return m_CleanupWarning;
}

bool ExportOutputTransaction::TryCreateLock(std::string& error) {
	error.clear();
	std::error_code createError;
	const bool created = std::filesystem::create_directory(NativeIoPath(m_LockPath), createError);
	if(createError) {
		error = "output_lock_unavailable: Could not create the exclusive output lock: " + createError.message();
	}
	return created;
}

bool ExportOutputTransaction::RecoverExistingLock(std::string& error) {
	LockMetadata metadata;
	std::string metadataError;
	if(!ReadLockMetadata(metadata, metadataError)) {
		error = "output_lock_conflict: The existing lock owner could not be verified; " + metadataError;
		return false;
	}

	if(metadata.finalDirectory.lexically_normal() != m_FinalDirectory
		|| !IsSafeTransactionSibling(metadata.stagingDirectory, ".staging")
		|| !IsSafeTransactionSibling(metadata.scratchDirectory, ".scratch")
		|| !IsSafeTransactionSibling(metadata.backupDirectory, ".backup")) {
		error = "output_lock_conflict: Existing lock metadata contains paths outside the reserved output siblings.";
		return false;
	}

	const int64_t ageSeconds = CurrentUnixSeconds() - metadata.createdUnixSeconds;
	if(ageSeconds < 0 || ageSeconds < m_Options.staleLockMinimumAge.count()) {
		error = "output_lock_conflict: The existing lock is too recent to recover safely.";
		return false;
	}
	if(IsProcessActive(metadata.processId)) {
		error = "output_lock_conflict: Another live export owns the requested output.";
		return false;
	}
	// A dead owner can leave a failed per-file rollback journal. There is no
	// durable full-tree commit protocol here: retain originals and fail closed
	// rather than treating these private files as disposable stale scratch.
	std::error_code journalError;
	bool completedJournal = false;
	if(HasPendingPublicationJournal(metadata.scratchDirectory, journalError, &completedJournal, metadata.runId, metadata.ownerToken) || journalError) {
		error = "output_lock_conflict: A prior publication journal requires manual recovery; originals were retained.";
		return false;
	}
	const auto cleanupStalePrivate = [&](const std::filesystem::path& path, std::string& cleanupError) {
		if(CleanupPath(path, cleanupError)) return true;
		if(!completedJournal) return false;
		// The dead owner's completion decision is authoritative even if it crashed
		// before updating lock metadata. Disposable leftovers need not retain its
		// stale lock; the next same-run preparation can retry their cleanup.
		AppendCleanupWarning("output_cleanup_failed: Completed stale publication retained private leftovers: " + cleanupError);
		cleanupError.clear();
		return true;
	};

	// If the previous process stopped after moving the old output aside, prefer
	// the known previous success over a new directory whose caller never received
	// a successful commit result.
	std::error_code existsError;
	if(std::filesystem::exists(NativeIoPath(metadata.backupDirectory), existsError)) {
		const bool metadataClaimsPublished = completedJournal || metadata.state == ToString(ExportOutputTransactionState::Published)
			|| metadata.state == ToString(ExportOutputTransactionState::Cleaned);
		if(metadataClaimsPublished && std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError)) {
			std::string cleanupError;
			if(!cleanupStalePrivate(metadata.backupDirectory, cleanupError)) {
				error = "output_lock_recovery_failed: " + cleanupError;
				return false;
			}
		} else {
			if(std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError)) {
				std::string cleanupError;
				if(!CleanupPath(m_FinalDirectory, cleanupError)) {
					error = "output_lock_recovery_failed: " + cleanupError;
					return false;
				}
			}
			std::error_code restoreError;
			std::filesystem::rename(
				NativeIoPath(metadata.backupDirectory),
				NativeIoPath(m_FinalDirectory),
				restoreError);
			if(restoreError) {
				error = "output_lock_recovery_failed: Could not restore the previous output: " + restoreError.message();
				return false;
			}
		}
	}
	if(existsError) {
		error = "output_lock_recovery_failed: Could not inspect the stale transaction paths: " + existsError.message();
		return false;
	}

	std::string cleanupError;
	if(!cleanupStalePrivate(metadata.stagingDirectory, cleanupError)
		|| !cleanupStalePrivate(metadata.scratchDirectory, cleanupError)) {
		error = "output_lock_recovery_failed: " + cleanupError;
		return false;
	}

	// Re-read the token immediately before removal. If it changed, an external
	// recovery or owner update raced us, so we refuse to delete that lock.
	LockMetadata currentMetadata;
	if(!ReadLockMetadata(currentMetadata, metadataError)
		|| currentMetadata.ownerToken != metadata.ownerToken) {
		error = "output_lock_conflict: The existing lock changed while recovery was in progress.";
		return false;
	}

	std::error_code removeError;
	std::filesystem::remove_all(NativeIoPath(m_LockPath), removeError);
	if(removeError) {
		error = "output_lock_recovery_failed: Could not remove the stale lock: " + removeError.message();
		return false;
	}
	m_RecoveredStaleLock = true;
	return true;
}

bool ExportOutputTransaction::PrepareRunDirectories(std::string& error) {
	std::error_code journalError;
	bool completedJournal = false;
	if(HasPendingPublicationJournal(m_ScratchDirectory, journalError, &completedJournal, m_RunId) || journalError) {
		m_RequiresManualRecovery = true;
		error = "output_staging_failed: An orphan publication journal requires manual recovery; originals were retained.";
		return false;
	}
	// A missing lock with same-run orphan directories can occur after forceful
	// process termination. Once this transaction owns the output lock, those
	// reserved sibling paths are safe to repair and reuse.
	std::error_code existsError;
	if(std::filesystem::exists(NativeIoPath(m_BackupDirectory), existsError)) {
		if(std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError)) {
			if(!CleanupPath(m_BackupDirectory, error)) {
				return false;
			}
		} else {
			std::error_code restoreError;
			std::filesystem::rename(
				NativeIoPath(m_BackupDirectory),
				NativeIoPath(m_FinalDirectory),
				restoreError);
			if(restoreError) {
				error = "output_staging_failed: Could not restore an orphaned previous output: " + restoreError.message();
				return false;
			}
		}
	}
	if(existsError) {
		error = "output_staging_failed: Could not inspect orphan transaction directories: " + existsError.message();
		return false;
	}

	m_CompletedJournalCleanupOnly = completedJournal;
	if(!CleanupPath(m_StagingDirectory, error) || !CleanupPath(m_ScratchDirectory, error)) {
		error = "output_staging_failed: " + error;
		return false;
	}
	m_CompletedJournalCleanupOnly = false;

	std::error_code stagingError;
	std::filesystem::create_directory(NativeIoPath(m_StagingDirectory), stagingError);
	if(stagingError) {
		error = "output_staging_failed: Could not create the private staging directory: " + stagingError.message();
		return false;
	}
	std::filesystem::create_directory(NativeIoPath(m_ScratchDirectory), stagingError);
	if(stagingError) {
		std::string ignoredCleanup;
		(void)CleanupPath(m_StagingDirectory, ignoredCleanup);
		error = "output_staging_failed: Could not create the private scratch directory: " + stagingError.message();
		return false;
	}

	m_HadPreviousOutput = std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError);
	if(existsError) {
		error = "output_staging_failed: Could not inspect the previous output: " + existsError.message();
		return false;
	}
	m_State = ExportOutputTransactionState::StagingPrepared;
	if(!WriteLockMetadata(error)) {
		m_State = ExportOutputTransactionState::Locked;
		return false;
	}
	return true;
}

bool ExportOutputTransaction::WriteLockMetadata(std::string& error) const {
	error.clear();
	const std::filesystem::path temporaryPath = m_LockPath / ("owner-" + m_OwnerToken + ".tmp");
	{
		std::ofstream stream(NativeIoPath(temporaryPath), std::ios::binary | std::ios::trunc);
		if(!stream.is_open()) {
			error = "output_lock_metadata_failed: Could not open temporary lock owner metadata.";
			return false;
		}

		stream << "schemaVersion=" << kLockMetadataSchemaVersion << '\n';
		stream << "processId=" << CurrentProcessId() << '\n';
		stream << "createdUnixSeconds=" << CurrentUnixSeconds() << '\n';
		stream << "runId=" << std::quoted(m_RunId) << '\n';
		stream << "ownerToken=" << std::quoted(m_OwnerToken) << '\n';
		stream << "finalDirectory=" << std::quoted(PathToUtf8(m_FinalDirectory)) << '\n';
		stream << "stagingDirectory=" << std::quoted(PathToUtf8(m_StagingDirectory)) << '\n';
		stream << "scratchDirectory=" << std::quoted(PathToUtf8(m_ScratchDirectory)) << '\n';
		stream << "backupDirectory=" << std::quoted(PathToUtf8(m_BackupDirectory)) << '\n';
		stream << "state=" << std::quoted(std::string(ToString(m_State))) << '\n';
		stream.flush();
		if(!stream.good()) {
			error = "output_lock_metadata_failed: Could not flush lock owner metadata.";
			std::error_code ignoredError;
			std::filesystem::remove(NativeIoPath(temporaryPath), ignoredError);
			return false;
		}
	}

	const std::filesystem::path metadataPath = LockMetadataPath();
	std::error_code replaceError;
#if defined(_WIN32)
	const std::filesystem::path nativeTemporaryPath = NativeIoPath(temporaryPath);
	const std::filesystem::path nativeMetadataPath = NativeIoPath(metadataPath);
	if(!::MoveFileExW(
		nativeTemporaryPath.c_str(),
		nativeMetadataPath.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		replaceError = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
	}
#else
	std::filesystem::rename(NativeIoPath(temporaryPath), NativeIoPath(metadataPath), replaceError);
#endif
	if(replaceError) {
		std::error_code ignoredError;
		std::filesystem::remove(NativeIoPath(temporaryPath), ignoredError);
		error = "output_lock_metadata_failed: Could not publish lock owner metadata: " + replaceError.message();
		return false;
	}
	return true;
}

bool ExportOutputTransaction::ReadLockMetadata(LockMetadata& metadata, std::string& error) const {
	error.clear();
	std::ifstream stream(NativeIoPath(LockMetadataPath()), std::ios::binary);
	if(!stream.is_open()) {
		error = "lock owner metadata is missing";
		return false;
	}

	std::unordered_map<std::string, std::string> values;
	std::string line;
	while(std::getline(stream, line)) {
		const std::size_t separator = line.find('=');
		if(separator == std::string::npos) {
			error = "lock owner metadata contains an invalid line";
			return false;
		}
		values.emplace(line.substr(0, separator), line.substr(separator + 1));
	}

	auto readQuoted = [&values](std::string_view key, std::string& value) {
		const auto found = values.find(std::string(key));
		return found != values.end() && ParseQuotedString(found->second, value);
	};
	auto readPath = [&readQuoted](std::string_view key, std::filesystem::path& value) {
		std::string encoded;
		if(!readQuoted(key, encoded)) {
			return false;
		}
		value = PathFromUtf8(encoded).lexically_normal();
		return true;
	};

	const auto schema = values.find("schemaVersion");
	const auto process = values.find("processId");
	const auto created = values.find("createdUnixSeconds");
	if(schema == values.end() || !ParseNumber(schema->second, metadata.schemaVersion)
		|| process == values.end() || !ParseNumber(process->second, metadata.processId)
		|| created == values.end() || !ParseNumber(created->second, metadata.createdUnixSeconds)
		|| !readQuoted("runId", metadata.runId)
		|| !readQuoted("ownerToken", metadata.ownerToken)
		|| !readPath("finalDirectory", metadata.finalDirectory)
		|| !readPath("stagingDirectory", metadata.stagingDirectory)
		|| !readPath("scratchDirectory", metadata.scratchDirectory)
		|| !readPath("backupDirectory", metadata.backupDirectory)
		|| !readQuoted("state", metadata.state)) {
		error = "lock owner metadata is incomplete or malformed";
		return false;
	}
	if(metadata.schemaVersion != kLockMetadataSchemaVersion || metadata.ownerToken.empty()) {
		error = "lock owner metadata uses an unsupported schema or empty owner token";
		return false;
	}
	return true;
}

bool ExportOutputTransaction::ReleaseOwnedLock(std::string& warning) {
	warning.clear();
	if(!m_OwnsLock) {
		return true;
	}

	LockMetadata metadata;
	std::string metadataError;
	if(!ReadLockMetadata(metadata, metadataError)) {
		warning = "output_lock_cleanup_failed: " + metadataError;
		return false;
	}
	if(metadata.ownerToken != m_OwnerToken) {
		// The path no longer represents our lock. Forget local ownership but never
		// remove a directory that may now protect a different writer.
		m_OwnsLock = false;
		warning = "output_lock_cleanup_failed: Lock ownership changed before cleanup.";
		return false;
	}

	std::error_code removeError;
	std::filesystem::remove_all(NativeIoPath(m_LockPath), removeError);
	if(removeError) {
		warning = "output_lock_cleanup_failed: Could not remove the owned output lock: " + removeError.message();
		return false;
	}
	m_OwnsLock = false;
	return true;
}

bool ExportOutputTransaction::RestoreBackup(std::string& error) {
	error.clear();
	std::error_code existsError;
	if(!std::filesystem::exists(NativeIoPath(m_BackupDirectory), existsError)) {
		if(existsError) {
			error = "output_rollback_failed: Could not inspect the backup directory: " + existsError.message();
			return false;
		}
		return true;
	}

	if(std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError)) {
		std::string cleanupError;
		if(!CleanupPath(m_FinalDirectory, cleanupError)) {
			error = "output_rollback_failed: " + cleanupError;
			return false;
		}
	}
	if(existsError) {
		error = "output_rollback_failed: Could not inspect the final directory during restoration: " + existsError.message();
		return false;
	}

	std::error_code restoreError;
	std::filesystem::rename(
		NativeIoPath(m_BackupDirectory),
		NativeIoPath(m_FinalDirectory),
		restoreError);
	if(restoreError) {
		error = "output_rollback_failed: Could not restore the previous output: " + restoreError.message();
		return false;
	}
	return true;
}

bool ExportOutputTransaction::CleanupPath(const std::filesystem::path& path, std::string& error) const {
	error.clear();
	if(path.empty()) {
		return true;
	}
	std::error_code cleanupError;
	// A per-transaction, default-null error seam exercises the same cleanup and
	// lock-release branches without depending on host privileges or IO timing.
	if(m_Options.cleanupFailureForTests) cleanupError = m_Options.cleanupFailureForTests(path);
	if(!cleanupError) std::filesystem::remove_all(NativeIoPath(path), cleanupError);
	if(cleanupError) {
		error = "Could not remove '" + PathToUtf8(path) + "': " + cleanupError.message();
		return false;
	}
	return true;
}

bool ExportOutputTransaction::IsSafeTransactionSibling(
	const std::filesystem::path& path,
	std::string_view suffix) const {
	if(path.parent_path().lexically_normal() != m_FinalDirectory.parent_path().lexically_normal()) {
		return false;
	}

	std::filesystem::path::string_type prefix;
	prefix.push_back(static_cast<std::filesystem::path::value_type>('.'));
	prefix.append(m_FinalDirectory.filename().native());
	AppendAscii(prefix, ".export-");
	std::filesystem::path::string_type nativeSuffix;
	AppendAscii(nativeSuffix, suffix);
	const std::filesystem::path::string_type name = path.filename().native();
	return name.size() > prefix.size() + nativeSuffix.size()
		&& name.compare(0, prefix.size(), prefix) == 0
		&& NativeEndsWith(name, nativeSuffix);
}

void ExportOutputTransaction::AppendCleanupWarning(std::string warning) {
	if(warning.empty()) {
		return;
	}
	if(!m_CleanupWarning.empty()) {
		m_CleanupWarning += ' ';
	}
	m_CleanupWarning += std::move(warning);
}

void ExportOutputTransaction::RollbackBestEffort() noexcept {
	if(m_State == ExportOutputTransactionState::Created
		|| ((m_State == ExportOutputTransactionState::Cleaned
			|| m_State == ExportOutputTransactionState::RolledBack) && !m_OwnsLock)) {
		return;
	}
	try {
		std::string ignoredError;
		(void)Rollback(ignoredError);
	} catch(...) {
		// Destructors must never terminate the editor or CLI. Any filesystem state
		// that could not be repaired remains protected by owner metadata for the
		// next stale-lock recovery attempt.
	}
}
