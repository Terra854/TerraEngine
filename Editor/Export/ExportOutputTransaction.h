// ExportOutputTransaction.h - Filesystem publication boundary for exported builds.
//
// An export is assembled in private sibling directories and published only after
// validation. This class owns the output lock and overlays validated files without
// replacing the destination directory or deleting unrelated destination content.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <memory>

class ExportCancellationState;

/// Identifies the current visibility state of one output transaction.
enum class ExportOutputTransactionState {
	Created,
	Locked,
	StagingPrepared,
	StagedAndValidated,
	PreviousOutputBackedUp,
	Published,
	Cleaned,
	RolledBack,
};

/// Returns the stable diagnostic spelling used in lock owner metadata.
[[nodiscard]] std::string_view ToString(ExportOutputTransactionState state);

/// Configures conservative stale-lock handling without changing normal exports.
struct ExportOutputTransactionOptions {
	/// Recently-created locks are never reclaimed, even if their owner lookup
	/// fails. This protects the short interval while another process writes its
	/// owner metadata. Tests may use zero seconds for deterministic recovery.
	std::chrono::seconds staleLockMinimumAge = std::chrono::seconds(30);

	/// Disabling recovery makes every existing lock an output conflict. This is
	/// useful to callers that prefer manual inspection over automatic cleanup.
	bool recoverStaleLocks = true;
};

/// Owns the exclusive lock, private work directories, commit, and rollback for
/// one final export directory.
class ExportOutputTransaction final {
public:
	ExportOutputTransaction(
		std::filesystem::path finalDirectory,
		std::string runId,
		ExportOutputTransactionOptions options = {});
	~ExportOutputTransaction();

	ExportOutputTransaction(const ExportOutputTransaction&) = delete;
	ExportOutputTransaction& operator=(const ExportOutputTransaction&) = delete;
	ExportOutputTransaction(ExportOutputTransaction&&) = delete;
	ExportOutputTransaction& operator=(ExportOutputTransaction&&) = delete;

	/// Acquires the sibling lock without creating or deleting staging content.
	/// A conflicting live or unverified lock returns false and leaves the final
	/// output unchanged.
	[[nodiscard]] bool Acquire(std::string& error);

	/// Removes only safe orphan paths for this run and creates empty staging and
	/// scratch directories on the same volume as the final output.
	[[nodiscard]] bool Prepare(std::string& error);

	/// Convenience operation used by the pipeline before it starts writing.
	[[nodiscard]] bool Begin(std::string& error);

	/// Records the validation boundary that callers must cross before Commit().
	[[nodiscard]] bool MarkStagedAndValidated(std::string& error);

	/// Overwrites matching destination files from validated staging while keeping
	/// the destination directory identity and every unmentioned path intact.
	[[nodiscard]] bool Commit(std::string& error);
	// Cancellation accepted before this protected publication wins. A completed
	// commit permanently rejects later cancellation without relabeling success.
	[[nodiscard]] bool Commit(std::string& error, const std::shared_ptr<ExportCancellationState>& cancellation);

	/// Removes this run's private files and restores any backed-up prior output.
	/// Calling Rollback after a successful Commit only performs leftover cleanup.
	[[nodiscard]] bool Rollback(std::string& error);

	[[nodiscard]] const std::filesystem::path& FinalDirectory() const;
	[[nodiscard]] const std::filesystem::path& StagingDirectory() const;
	[[nodiscard]] const std::filesystem::path& ScratchDirectory() const;
	[[nodiscard]] const std::filesystem::path& BackupDirectory() const;
	[[nodiscard]] const std::filesystem::path& LockPath() const;
	[[nodiscard]] std::filesystem::path LockMetadataPath() const;
	[[nodiscard]] ExportOutputTransactionState State() const;
	[[nodiscard]] bool OwnsLock() const;
	[[nodiscard]] bool HadPreviousOutput() const;
	[[nodiscard]] bool WasPublished() const;
	[[nodiscard]] bool RecoveredStaleLock() const;

	/// Cleanup failures after a successful publication are warnings because the
	/// complete validated output is already visible and must not be misreported
	/// as a partially-published build.
	[[nodiscard]] const std::string& CleanupWarning() const;

private:
	struct LockMetadata;

	[[nodiscard]] bool TryCreateLock(std::string& error);
	[[nodiscard]] bool RecoverExistingLock(std::string& error);
	[[nodiscard]] bool PrepareRunDirectories(std::string& error);
	[[nodiscard]] bool WriteLockMetadata(std::string& error) const;
	[[nodiscard]] bool ReadLockMetadata(LockMetadata& metadata, std::string& error) const;
	[[nodiscard]] bool ReleaseOwnedLock(std::string& warning);
	[[nodiscard]] bool RestoreBackup(std::string& error);
	[[nodiscard]] bool CleanupPath(const std::filesystem::path& path, std::string& error) const;
	[[nodiscard]] bool IsSafeTransactionSibling(const std::filesystem::path& path, std::string_view suffix) const;
	void AppendCleanupWarning(std::string warning);
	void RollbackBestEffort() noexcept;

	std::filesystem::path m_FinalDirectory;
	std::filesystem::path m_StagingDirectory;
	std::filesystem::path m_ScratchDirectory;
	std::filesystem::path m_BackupDirectory;
	std::filesystem::path m_LockPath;
	std::string m_RunId;
	std::string m_OwnerToken;
	std::string m_InitializationError;
	std::string m_CleanupWarning;
	ExportOutputTransactionOptions m_Options;
	ExportOutputTransactionState m_State = ExportOutputTransactionState::Created;
	bool m_OwnsLock = false;
	bool m_HadPreviousOutput = false;
	bool m_WasPublished = false;
	bool m_RecoveredStaleLock = false;
};
