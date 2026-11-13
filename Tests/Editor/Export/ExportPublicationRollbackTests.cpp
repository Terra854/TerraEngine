// Qualify the actual output transaction, including a failed published prefix.
// All files and native locks belong to an exclusively created private fixture.
#include "../TestHarness.h"
#include "ExportOutputTransaction.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cstdint>
#include <utility>
#include <algorithm>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#undef min
#undef max
#else
#include <sys/stat.h>
#endif

namespace TerraEngine::EditorTests {
	namespace {
		struct PublicationWorkspace {
			// Reuse the bounded exclusive-path owner. Its member destructor also
			// cleans up when creating the final child throws during construction.
			TerraEngine::Tests::Correctness::DisposableFixturePath owner;
			std::filesystem::path root = owner.Root();
			PublicationWorkspace() {
				std::filesystem::create_directory(Final());
			}
			std::filesystem::path Final() const { return root / "Final"; }
			static void Write(const std::filesystem::path& path, std::string_view bytes) {
				std::filesystem::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
			}
			static std::string Read(const std::filesystem::path& path) {
				std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			}
		};
		std::pair<std::uint64_t, std::uint64_t> DirectoryIdentity(const std::filesystem::path& path) {
#ifdef _WIN32
			const HANDLE handle = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if(handle == INVALID_HANDLE_VALUE) throw TestFailure("Cannot inspect fixture directory identity.");
			BY_HANDLE_FILE_INFORMATION info{}; const bool read = GetFileInformationByHandle(handle, &info) != FALSE; CloseHandle(handle);
			if(!read) throw TestFailure("Cannot read fixture directory identity.");
			return {info.dwVolumeSerialNumber, (std::uint64_t{info.nFileIndexHigh} << 32U) | info.nFileIndexLow};
#else
			struct stat info{}; if(::stat(path.c_str(), &info) != 0) throw TestFailure("Cannot read fixture directory identity.");
			return {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino)};
#endif
		}
#ifdef _WIN32
		struct FileReadLock {
			HANDLE handle = INVALID_HANDLE_VALUE;
			explicit FileReadLock(const std::filesystem::path& path) {
				// Original backup reads succeed, while atomic replacement is denied.
				handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
				if(handle == INVALID_HANDLE_VALUE) throw TestFailure("Cannot acquire deterministic destination lock.");
			}
			~FileReadLock() { CloseHandle(handle); }
		};
		struct PrivateJunction {
			std::filesystem::path path;
			PrivateJunction(const std::filesystem::path& entry, const std::filesystem::path& target) : path(entry) {
				if(!CreateDirectoryW(path.c_str(), nullptr)) throw TestFailure("Cannot exclusively create fixture junction.");
				const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
				struct JunctionData { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[2048]; } data{};
				const auto substitute = L"\\??\\" + std::filesystem::absolute(target).native(); const auto print = target.native();
				bool success = false;
				if(handle != INVALID_HANDLE_VALUE) {
					if(substitute.size() + print.size() + 2U < 2048U) {
						data.tag = IO_REPARSE_TAG_MOUNT_POINT; data.subLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
						data.printOffset = data.subLength + sizeof(wchar_t); data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
						std::copy(substitute.begin(), substitute.end(), data.paths); std::copy(print.begin(), print.end(), data.paths + substitute.size() + 1U);
						data.length = static_cast<WORD>(8U + (substitute.size() + print.size() + 2U) * sizeof(wchar_t)); DWORD returned = 0;
						success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8U + data.length, nullptr, 0, &returned, nullptr) != FALSE;
					}
					CloseHandle(handle);
				}
				if(!success) { RemoveDirectoryW(path.c_str()); throw TestFailure("Mandatory fixture junction creation failed."); }
			}
			~PrivateJunction() { RemoveDirectoryW(path.c_str()); }
		};
#endif
		void ExpectPrivateCleanup(TestContext& context, ExportOutputTransaction& transaction) {
			std::string error; context.Expect(transaction.Rollback(error), error);
			context.Expect(!std::filesystem::exists(transaction.StagingDirectory()) && !std::filesystem::exists(transaction.ScratchDirectory()) && !std::filesystem::exists(transaction.LockPath()), "Rollback must clean its staging, journal, scratch and owned lock.");
		}
	}
	void RegisterExportPublicationRollbackTests(TestRegistry& registry) {
		registry.Add("Editor.ExportPublicationRollback.CompletedCleanupFailureReleasesLockAndRetries", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old");
			std::filesystem::path blockedScratch; bool refuse = true;
			ExportOutputTransactionOptions options;
			options.cleanupFailureForTests = [&](const auto& path) {
				return refuse && path == blockedScratch ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
			};
			ExportOutputTransaction first(w.Final(), "repeat-run", options); std::string error;
			context.Expect(first.Begin(error), error); blockedScratch = first.ScratchDirectory();
			PublicationWorkspace::Write(first.StagingDirectory() / "a.txt", "published");
			context.Expect(first.MarkStagedAndValidated(error) && first.Commit(error), error);
			context.Expect(first.WasPublished() && !first.OwnsLock() && !std::filesystem::exists(first.LockPath())
				&& !first.CleanupWarning().empty(), "Cleanup failure must preserve publication success and release its normal lock.");
			std::filesystem::path completed;
			for(const auto& entry : std::filesystem::directory_iterator(blockedScratch))
				if(entry.path().filename().string().starts_with(".completed-publication-")) completed = entry.path();
			context.Expect(!completed.empty() && PublicationWorkspace::Read(completed / "0.original") == "old",
				"The on-disk completion decision must survive the failed scratch removal with its original bytes.");
			ExportOutputTransaction retryBlocked(w.Final(), "repeat-run", options);
			context.Expect(!retryBlocked.Begin(error) && !retryBlocked.OwnsLock() && !std::filesystem::exists(first.LockPath()),
				"A repeated cleanup error must reject that attempt without retaining a newly acquired lock.");
			ExportOutputTransaction other(w.Final(), "another-run");
			context.Expect(other.Begin(error), "A cleanup-only leftover must permit another run: " + error);
			PublicationWorkspace::Write(other.StagingDirectory() / "a.txt", "later publication");
			context.Expect(other.MarkStagedAndValidated(error) && other.Commit(error), error);
			refuse = false;
			ExportOutputTransaction retry(w.Final(), "repeat-run", options);
			context.Expect(retry.Begin(error), "The same run must clean completed leftovers and retry: " + error);
			context.Expect(PublicationWorkspace::Read(w.Final() / "a.txt") == "later publication" && !std::filesystem::exists(completed),
				"Retry cleanup must never restore obsolete originals over a later successful publication.");
			ExpectPrivateCleanup(context, retry);
		});
		registry.Add("Editor.ExportPublicationRollback.PartiallyRemovedCompletedJournalIsCleanupOnly", [](TestContext& context) {
			PublicationWorkspace w; std::filesystem::path scratch; bool refuse = true;
			ExportOutputTransactionOptions options;
			options.cleanupFailureForTests = [&](const auto& path) {
				if(!refuse || path != scratch) return std::error_code{};
				// Real recursive cleanup can delete children before encountering a locked
				// entry. Remove completed contents, then inject that final IO refusal.
				for(const auto& journal : std::filesystem::directory_iterator(scratch))
					if(journal.path().filename().string().starts_with(".completed-publication-"))
						for(const auto& file : std::filesystem::directory_iterator(journal.path())) std::filesystem::remove_all(file.path());
				return std::make_error_code(std::errc::permission_denied);
			};
			ExportOutputTransaction first(w.Final(), "partial-cleanup", options); std::string error;
			context.Expect(first.Begin(error), error); scratch = first.ScratchDirectory();
			PublicationWorkspace::Write(first.StagingDirectory() / "new.txt", "complete");
			context.Expect(first.MarkStagedAndValidated(error) && first.Commit(error), error);
			refuse = false;
			ExportOutputTransaction retry(w.Final(), "partial-cleanup", options);
			context.Expect(retry.Begin(error), "A partially removed completed journal must remain cleanup-only: " + error);
			context.Expect(PublicationWorkspace::Read(w.Final() / "new.txt") == "complete", "Cleanup-only recovery must retain published bytes.");
			ExpectPrivateCleanup(context, retry);
		});
		registry.Add("Editor.ExportPublicationRollback.CompletedDecisionRecoversStaleLockBeforeMetadataUpdate", [](TestContext& context) {
			PublicationWorkspace w; std::filesystem::path scratch;
			ExportOutputTransactionOptions options;
			options.cleanupFailureForTests = [&](const auto& path) {
				return path == scratch ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
			};
			ExportOutputTransaction first(w.Final(), "interrupted-cleanup", options); std::string error;
			context.Expect(first.Begin(error), error); scratch = first.ScratchDirectory();
			// Keep the real pre-publication metadata to reproduce a crash after the
			// durable directory decision but before its lock-state update/removal.
			const auto before = PublicationWorkspace::Read(first.LockMetadataPath());
			PublicationWorkspace::Write(first.StagingDirectory() / "a.txt", "completed output");
			context.Expect(first.MarkStagedAndValidated(error) && first.Commit(error), error);
			std::istringstream input(before); std::ostringstream stale; std::string line;
			while(std::getline(input, line)) {
				if(line.starts_with("processId=")) line = "processId=0";
				if(line.starts_with("createdUnixSeconds=")) line = "createdUnixSeconds=0";
				stale << line << '\n';
			}
			PublicationWorkspace::Write(first.LockMetadataPath(), stale.str());
			ExportOutputTransaction next(w.Final(), "after-stale-completion", options);
			context.Expect(next.Begin(error) && next.RecoveredStaleLock(),
				"A completed decision must permit stale-lock recovery despite old metadata and disposable cleanup refusal: " + error);
			context.Expect(!next.CleanupWarning().empty() && std::filesystem::exists(scratch)
				&& PublicationWorkspace::Read(w.Final() / "a.txt") == "completed output",
				"Recovery must report retained private leftovers without restoring obsolete data or blocking the new export.");
			ExpectPrivateCleanup(context, next);
		});
		registry.Add("Editor.ExportPublicationRollback.PendingJournalWinsOverCompletedLeftovers", [](TestContext& context) {
			PublicationWorkspace w; ExportOutputTransaction transaction(w.Final(), "mixed-journals"); std::string error;
			context.Expect(transaction.Acquire(error), error);
			const auto original = transaction.ScratchDirectory() / ".publication-interrupted/0.original";
			PublicationWorkspace::Write(original, "only recovery bytes");
			PublicationWorkspace::Write(transaction.ScratchDirectory() / ".completed-publication-old/0.original", "disposable old bytes");
			context.Expect(!transaction.Prepare(error) && !transaction.Rollback(error) && transaction.OwnsLock(),
				"Any genuinely incomplete journal must retain manual recovery even beside completed leftovers.");
			context.Expect(PublicationWorkspace::Read(original) == "only recovery bytes" && std::filesystem::exists(transaction.LockPath()),
				"Cleanup must not destroy genuine recovery evidence or its protecting lock.");
		});
		registry.Add("Editor.ExportPublicationRollback.ForeignCompletionCannotAuthorizeStaleOwner", [](TestContext& context) {
			PublicationWorkspace w; ExportOutputTransaction prior(w.Final(), "foreign-owner"); std::string error;
			context.Expect(prior.Acquire(error), error);
			const auto before = PublicationWorkspace::Read(prior.LockMetadataPath());
			std::istringstream input(before); std::ostringstream stale; std::string line, token;
			while(std::getline(input, line)) {
				if(line.starts_with("ownerToken=")) { std::istringstream value(line.substr(11)); value >> std::quoted(token); }
				if(line.starts_with("processId=")) line = "processId=0";
				if(line.starts_with("createdUnixSeconds=")) line = "createdUnixSeconds=0";
				stale << line << '\n';
			}
			context.Expect(!token.empty(), "The pending owner's actual token must be retained.");
			PublicationWorkspace::Write(prior.BackupDirectory() / "old.txt", "last known good");
			PublicationWorkspace::Write(w.Final() / "uncommitted.txt", "not accepted");
			// Same-run and well-formed, but a different owner: it cannot prove this
			// stale owner's completion or authorize deletion of that owner's backup.
			PublicationWorkspace::Write(prior.ScratchDirectory() / (".completed-publication-" + token + "9") / "0.original", "foreign cleanup bytes");
			PublicationWorkspace::Write(prior.LockMetadataPath(), stale.str());
			ExportOutputTransactionOptions options;
			options.cleanupFailureForTests = [&](const auto& path) {
				return path == prior.ScratchDirectory() ? std::make_error_code(std::errc::permission_denied) : std::error_code{};
			};
			ExportOutputTransaction next(w.Final(), "new-owner", options);
			context.Expect(!next.Begin(error) && !next.OwnsLock() && std::filesystem::exists(prior.LockPath()),
				"A foreign completed token cannot bypass pending-owner cleanup failures and release its lock.");
			context.Expect(PublicationWorkspace::Read(w.Final() / "old.txt") == "last known good"
				&& !std::filesystem::exists(w.Final() / "uncommitted.txt"),
				"Foreign completion cannot discard the real backup instead of restoring the prior accepted output.");
		});
		registry.Add("Editor.ExportPublicationRollback.MalformedCompletedNamesRetainRecovery", [](TestContext& context) {
			for(const auto name : {".completed-publication-another-run-1-2-3", ".completed-publication-malformed-1-2-invalid"}) {
				PublicationWorkspace w; ExportOutputTransaction transaction(w.Final(), "malformed"); std::string error;
				context.Expect(transaction.Acquire(error), error);
				const auto original = transaction.ScratchDirectory() / name / "0.original";
				PublicationWorkspace::Write(original, "unverified original");
				context.Expect(!transaction.Prepare(error) && !transaction.Rollback(error) && transaction.OwnsLock(),
					"A malformed or different-run completed-looking name must not become disposable by spelling alone.");
				context.Expect(PublicationWorkspace::Read(original) == "unverified original", "Unverified recovery bytes must survive.");
			}
		});
		registry.Add("Editor.ExportPublicationRollback.CompletionRenameFailureRestoresPublishedPrefix", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "original");
			ExportOutputTransaction transaction(w.Final(), "decision-failure"); std::string error;
			context.Expect(transaction.Begin(error), error);
			std::ifstream metadata(transaction.LockMetadataPath()); std::string line, token;
			while(std::getline(metadata, line)) if(line.starts_with("ownerToken=")) {
				std::istringstream value(line.substr(11)); value >> std::quoted(token);
			}
			metadata.close();
			context.Expect(!token.empty(), "The actual lock receipt must identify its exclusively owned journal.");
			const auto blocker = transaction.ScratchDirectory() / (".completed-publication-" + token);
			PublicationWorkspace::Write(blocker, "must not overwrite this entry");
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "candidate");
			context.Expect(transaction.MarkStagedAndValidated(error) && !transaction.Commit(error) && !transaction.WasPublished(),
				"Failure to persist the completion decision must reject publication rather than claim success.");
			context.Expect(PublicationWorkspace::Read(w.Final() / "a.txt") == "original" && PublicationWorkspace::Read(blocker) == "must not overwrite this entry",
				"Completion-decision failure must roll back the replaced prefix and preserve the conflicting entry.");
			ExpectPrivateCleanup(context, transaction);
		});
		registry.Add("Editor.ExportPublicationRollback.SuccessfulOverlayRetainsUnmentionedFiles", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old"); PublicationWorkspace::Write(w.Final() / "User/keep.txt", "user owned");
			const auto identity = DirectoryIdentity(w.Final()); const auto userTime = std::filesystem::last_write_time(w.Final() / "User/keep.txt");
			ExportOutputTransaction transaction(w.Final(), "overlay-success"); std::string error; context.Expect(transaction.Begin(error), error);
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "new"); PublicationWorkspace::Write(transaction.StagingDirectory() / "New/child.txt", "new child");
			context.Expect(transaction.MarkStagedAndValidated(error), error); context.Expect(transaction.Commit(error), error);
			context.Expect(identity == DirectoryIdentity(w.Final()), "Successful overlay must retain destination directory identity.");
			context.ExpectEqual(std::string("new"), PublicationWorkspace::Read(w.Final() / "a.txt"), "Matching destination must receive complete staged bytes.");
			context.ExpectEqual(std::string("new child"), PublicationWorkspace::Read(w.Final() / "New/child.txt"), "New directories and files must publish.");
			context.ExpectEqual(std::string("user owned"), PublicationWorkspace::Read(w.Final() / "User/keep.txt"), "Unmentioned user content must survive.");
			context.ExpectEqual(userTime, std::filesystem::last_write_time(w.Final() / "User/keep.txt"), "Unmentioned user timestamps must survive.");
			ExpectPrivateCleanup(context, transaction);
		});
		registry.Add("Editor.ExportPublicationRollback.TypeConflictRejectsBeforeChanges", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old"); PublicationWorkspace::Write(w.Final() / "z-blocker", "user blocker");
			ExportOutputTransaction transaction(w.Final(), "type-conflict"); std::string error; context.Expect(transaction.Begin(error), error);
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "new"); PublicationWorkspace::Write(transaction.StagingDirectory() / "z-blocker/child.txt", "candidate");
			context.Expect(transaction.MarkStagedAndValidated(error), error); context.Expect(!transaction.Commit(error), "Conflicting directory target must reject the complete overlay.");
			context.ExpectEqual(std::string("old"), PublicationWorkspace::Read(w.Final() / "a.txt"), "Preflight failure must publish no earlier file.");
			context.ExpectEqual(std::string("user blocker"), PublicationWorkspace::Read(w.Final() / "z-blocker"), "Preflight must retain unrelated type-conflicting content.");
			ExpectPrivateCleanup(context, transaction);
		});
		registry.Add("Editor.ExportPublicationRollback.PipelineScratchCleanupBeforeValidation", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old"); PublicationWorkspace::Write(w.Final() / "keep.txt", "user owned");
			const auto identity = DirectoryIdentity(w.Final());
			ExportOutputTransaction transaction(w.Final(), "pipeline-scratch-cleanup"); std::string error; context.Expect(transaction.Begin(error), error);
			PublicationWorkspace::Write(transaction.ScratchDirectory() / "PackageInput/temporary.txt", "packaging scratch");
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "validated replacement");
			// ExportPipeline removes packaging scratch after RuntimeStaging, before
			// MarkStagedAndValidated. Commit must reserve a fresh safe journal parent.
			std::filesystem::remove_all(transaction.ScratchDirectory());
			context.Expect(!std::filesystem::exists(transaction.ScratchDirectory()), "Probe must reproduce the real pipeline's removed scratch root.");
			context.Expect(transaction.MarkStagedAndValidated(error), error);
			const bool committed = transaction.Commit(error);
			context.Expect(committed, "Validated publication must tolerate pipeline scratch cleanup: " + error);
			context.Expect(identity == DirectoryIdentity(w.Final()), "Scratch recreation must preserve final directory identity.");
			context.ExpectEqual(std::string("validated replacement"), PublicationWorkspace::Read(w.Final() / "a.txt"), "Validated source must publish after scratch cleanup.");
			context.ExpectEqual(std::string("user owned"), PublicationWorkspace::Read(w.Final() / "keep.txt"), "Scratch recreation must retain unmentioned user files.");
			ExpectPrivateCleanup(context, transaction);
		});
		registry.Add("Editor.ExportPublicationRollback.OrphanJournalRetainedForManualRecovery", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "keep.txt", "existing output");
			ExportOutputTransaction transaction(w.Final(), "orphan-journal"); std::string error; context.Expect(transaction.Acquire(error), error);
			const auto original = transaction.ScratchDirectory() / ".publication-prior-owner/0.original";
			PublicationWorkspace::Write(original, "retained original");
			context.Expect(!transaction.Prepare(error), "An ambiguous retained journal must fail closed before scratch cleanup.");
			context.Expect(!transaction.Rollback(error), "An unowned prior journal must survive generic rollback cleanup.");
			context.ExpectEqual(std::string("retained original"), PublicationWorkspace::Read(original), "Manual recovery must retain its only original bytes.");
			context.ExpectEqual(std::string("existing output"), PublicationWorkspace::Read(w.Final() / "keep.txt"), "Manual recovery rejection must retain existing output.");
			context.Expect(std::filesystem::exists(transaction.LockPath()), "Unresolved manual recovery must retain its lock.");
		});
#ifdef _WIN32
		registry.Add("Editor.ExportPublicationRollback.LockedSecondFileRestoresPublishedPrefix", [](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old first"); PublicationWorkspace::Write(w.Final() / "z.txt", "old second"); PublicationWorkspace::Write(w.Final() / "keep.txt", "unmentioned");
			const auto oldTime = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
			std::filesystem::last_write_time(w.Final() / "a.txt", oldTime); std::filesystem::last_write_time(w.Final() / "z.txt", oldTime);
			const auto firstTime = std::filesystem::last_write_time(w.Final() / "a.txt"), secondTime = std::filesystem::last_write_time(w.Final() / "z.txt");
			const auto rootTime = std::filesystem::last_write_time(w.Final()); const auto identity = DirectoryIdentity(w.Final());
			const auto firstFileIdentity = DirectoryIdentity(w.Final() / "a.txt");
			ExportOutputTransaction transaction(w.Final(), "locked-second"); std::string error; context.Expect(transaction.Begin(error), error);
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "new first"); PublicationWorkspace::Write(transaction.StagingDirectory() / "m-owned/deep/new.txt", "new owned file"); PublicationWorkspace::Write(transaction.StagingDirectory() / "z.txt", "new second");
			// Keep failure restoration covered under the real pipeline's scratch
			// lifecycle too, after its packaging inputs have already been removed.
			std::filesystem::remove_all(transaction.ScratchDirectory());
			context.Expect(transaction.MarkStagedAndValidated(error), error); FileReadLock locked(w.Final() / "z.txt");
			context.Expect(!transaction.Commit(error) && !transaction.WasPublished(), "Second-file lock must reject publication after an earlier complete replacement.");
			context.Expect(error.find("output_publication_failed") != std::string::npos && error.find("output_rollback_failed") == std::string::npos, "Primary failure must report successful prefix restoration.");
			context.Expect(identity == DirectoryIdentity(w.Final()), "Failed overlay must retain destination directory identity.");
			context.ExpectEqual(std::string("old first"), PublicationWorkspace::Read(w.Final() / "a.txt"), "Earlier replacement must restore original bytes.");
			// The backup existed alongside the original, so it had a distinct native
			// file ID. Its restoration proves the first write actually published and
			// rolled back, rather than a preflight failure bypassing the prefix.
			context.Expect(firstFileIdentity != DirectoryIdentity(w.Final() / "a.txt"), "Locked-second probe must exercise a real earlier replacement and restoration.");
			context.ExpectEqual(std::string("old second"), PublicationWorkspace::Read(w.Final() / "z.txt"), "Locked second file must remain byte-exact.");
			context.ExpectEqual(std::string("unmentioned"), PublicationWorkspace::Read(w.Final() / "keep.txt"), "Rollback must retain unmentioned files.");
			context.ExpectEqual(firstTime, std::filesystem::last_write_time(w.Final() / "a.txt"), "Earlier replacement must restore its original timestamp.");
			context.ExpectEqual(secondTime, std::filesystem::last_write_time(w.Final() / "z.txt"), "Locked file timestamp must remain unchanged.");
			context.ExpectEqual(rootTime, std::filesystem::last_write_time(w.Final()), "Rollback must restore the existing destination directory timestamp.");
			context.Expect(!std::filesystem::exists(w.Final() / "m-owned"), "Rollback must remove only its newly owned directories and files.");
			context.ExpectEqual(std::string("new first"), PublicationWorkspace::Read(transaction.StagingDirectory() / "a.txt"), "Failed publication must retain its staged source unchanged until cleanup.");
			context.ExpectEqual(std::string("new second"), PublicationWorkspace::Read(transaction.StagingDirectory() / "z.txt"), "Locked failure must retain the second staged source.");
			context.ExpectEqual(std::string("new owned file"), PublicationWorkspace::Read(transaction.StagingDirectory() / "m-owned/deep/new.txt"), "Rollback must retain the new-file staging source until private cleanup.");
			ExpectPrivateCleanup(context, transaction);
		});
		for(const bool stagingRedirect : {false, true}) registry.Add(std::string("Editor.ExportPublicationRollback.RedirectRejectsBeforeChanges.") + (stagingRedirect ? "Staging" : "Destination"), [stagingRedirect](TestContext& context) {
			PublicationWorkspace w; PublicationWorkspace::Write(w.Final() / "a.txt", "old"); PublicationWorkspace::Write(w.root / "Outside/sentinel.txt", "outside");
			ExportOutputTransaction transaction(w.Final(), "redirect"); std::string error; context.Expect(transaction.Begin(error), error);
			PublicationWorkspace::Write(transaction.StagingDirectory() / "a.txt", "new");
			if(!stagingRedirect) PublicationWorkspace::Write(transaction.StagingDirectory() / "redirect/sentinel.txt", "candidate");
			{ PrivateJunction redirect((stagingRedirect ? transaction.StagingDirectory() : w.Final()) / "redirect", w.root / "Outside");
				context.Expect(transaction.MarkStagedAndValidated(error), error); context.Expect(!transaction.Commit(error), "Redirected target must reject before final writes.");
				context.ExpectEqual(std::string("old"), PublicationWorkspace::Read(w.Final() / "a.txt"), "Redirect preflight must not publish an earlier file.");
				context.ExpectEqual(std::string("outside"), PublicationWorkspace::Read(w.root / "Outside/sentinel.txt"), "Redirect rejection must preserve the private outside sentinel."); }
			ExpectPrivateCleanup(context, transaction);
		});
#endif
	}
}
