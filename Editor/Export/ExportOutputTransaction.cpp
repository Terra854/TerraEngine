// ExportOutputTransaction.cpp - Validated staging and conservative output overlay.

#include "ExportOutputTransaction.h"

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

#if defined(_WIN32)
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <Windows.h>
#else
	#include <cerrno>
	#include <csignal>
	#include <unistd.h>
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

bool ExportOutputTransaction::Commit(std::string& error) {
	error.clear();
	if(m_State != ExportOutputTransactionState::StagedAndValidated || !m_OwnsLock) {
		error = "output_publication_state_invalid: Commit requires a validated staging directory and an owned lock.";
		return false;
	}

	std::error_code typeError;
	m_HadPreviousOutput = std::filesystem::exists(NativeIoPath(m_FinalDirectory), typeError);
	if(typeError) {
		error = "output_publication_failed: Could not inspect the previous output: " + typeError.message();
		return false;
	}

	// Publish as an in-place overlay so shells, debuggers, and other tools can
	// keep the destination directory open. Matching files receive the staged
	// version, while paths absent from staging remain entirely user-owned and are
	// never deleted as an implicit side effect of export.
	std::error_code publishError;
	std::filesystem::copy(
		NativeIoPath(m_StagingDirectory),
		NativeIoPath(m_FinalDirectory),
		std::filesystem::copy_options::overwrite_existing | std::filesystem::copy_options::recursive,
		publishError);
	if(publishError) {
		error = "output_publication_failed: Could not publish the validated staging directory: " + publishError.message();
		return false;
	}

	m_WasPublished = true;
	m_State = ExportOutputTransactionState::Published;
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
	cleanupWarning.clear();
	if(!ReleaseOwnedLock(cleanupWarning)) {
		AppendCleanupWarning(std::move(cleanupWarning));
	}
	m_State = ExportOutputTransactionState::Cleaned;
	return true;
}

bool ExportOutputTransaction::Rollback(std::string& error) {
	error.clear();
	if(m_State == ExportOutputTransactionState::Created
		|| ((m_State == ExportOutputTransactionState::Cleaned
			|| m_State == ExportOutputTransactionState::RolledBack) && !m_OwnsLock)) {
		return true;
	}

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

	// If the previous process stopped after moving the old output aside, prefer
	// the known previous success over a new directory whose caller never received
	// a successful commit result.
	std::error_code existsError;
	if(std::filesystem::exists(NativeIoPath(metadata.backupDirectory), existsError)) {
		const bool metadataClaimsPublished = metadata.state == ToString(ExportOutputTransactionState::Published)
			|| metadata.state == ToString(ExportOutputTransactionState::Cleaned);
		if(metadataClaimsPublished && std::filesystem::exists(NativeIoPath(m_FinalDirectory), existsError)) {
			std::string cleanupError;
			if(!CleanupPath(metadata.backupDirectory, cleanupError)) {
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
	if(!CleanupPath(metadata.stagingDirectory, cleanupError)
		|| !CleanupPath(metadata.scratchDirectory, cleanupError)) {
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

	if(!CleanupPath(m_StagingDirectory, error) || !CleanupPath(m_ScratchDirectory, error)) {
		error = "output_staging_failed: " + error;
		return false;
	}

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
	std::filesystem::remove_all(NativeIoPath(path), cleanupError);
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
