#include "TestHarness.h"

#include "ExportOutputTransaction.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		std::atomic<uint64_t> g_TransactionWorkspaceSequence = 0;

		/// Gives every test its own directory so concurrently-running writer tests
		/// cannot remove another test's files.
		class TempExportOutputWorkspace {
		public:
			TempExportOutputWorkspace() {
				const uint64_t sequence = g_TransactionWorkspaceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
				m_Root = std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests"
					/ "ExportOutputTransaction"
					/ std::to_string(sequence);
				std::error_code ignoredError;
				std::filesystem::remove_all(m_Root, ignoredError);
				std::filesystem::create_directories(m_Root);
			}

			~TempExportOutputWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(m_Root, ignoredError);
			}

			[[nodiscard]] std::filesystem::path FinalDirectory() const {
				return m_Root / "Output With Spaces";
			}

			void WriteFinalFile(const std::filesystem::path& relativePath, const std::string& contents) const {
				WriteText(FinalDirectory() / relativePath, contents);
			}

			static void WriteText(const std::filesystem::path& path, const std::string& contents) {
				std::filesystem::create_directories(path.parent_path());
				std::ofstream stream(path, std::ios::binary | std::ios::trunc);
				stream << contents;
			}

			static std::string ReadText(const std::filesystem::path& path) {
				std::ifstream stream(path, std::ios::binary);
				return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
			}

		private:
			std::filesystem::path m_Root;
		};

		void ReplaceMetadataValue(std::string& metadata, const std::string& key, const std::string& replacement) {
			const std::string prefix = key + '=';
			const std::size_t begin = metadata.find(prefix);
			if(begin == std::string::npos) {
				return;
			}
			const std::size_t valueBegin = begin + prefix.size();
			const std::size_t end = metadata.find('\n', valueBegin);
			metadata.replace(valueBegin, end == std::string::npos ? std::string::npos : end - valueBegin, replacement);
		}
	}

	void RegisterExportOutputTransactionTests(TestRegistry& registry) {
		registry.Add("Editor.ExportOutputTransaction.CreatesUniqueSameVolumeWorkDirectories", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			std::string error;
			std::filesystem::path firstStaging;
			{
				ExportOutputTransaction first(workspace.FinalDirectory(), "first-run");
				context.Expect(first.Begin(error), error.empty() ? "The first transaction should begin." : error);
				firstStaging = first.StagingDirectory();
				context.ExpectEqual(first.FinalDirectory().parent_path(), first.StagingDirectory().parent_path(), "Staging must be a sibling so publication stays on one volume.");
				context.ExpectEqual(first.FinalDirectory().parent_path(), first.ScratchDirectory().parent_path(), "Scratch work must stay beside the final directory.");
				context.Expect(std::filesystem::is_directory(first.StagingDirectory()), "Begin should create the private staging directory.");
				context.Expect(std::filesystem::is_directory(first.ScratchDirectory()), "Begin should create the private scratch directory.");
				context.Expect(first.Rollback(error), error.empty() ? "The first transaction should roll back cleanly." : error);
			}

			ExportOutputTransaction second(workspace.FinalDirectory(), "second-run");
			context.Expect(second.Begin(error), error.empty() ? "The second transaction should begin." : error);
			context.Expect(firstStaging != second.StagingDirectory(), "Different run ids must produce unique staging directories.");
			context.Expect(second.Rollback(error), error.empty() ? "The second transaction should roll back cleanly." : error);
		});

		registry.Add("Editor.ExportOutputTransaction.RequiresValidatedStagingBeforeCommit", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			ExportOutputTransaction transaction(workspace.FinalDirectory(), "validation-boundary");
			std::string error;
			context.Expect(transaction.Begin(error), error.empty() ? "The transaction should begin." : error);
			TempExportOutputWorkspace::WriteText(transaction.StagingDirectory() / "new.txt", "new output");

			context.Expect(!transaction.Commit(error), "Commit must reject staging content that has not crossed the validation boundary.");
			context.Expect(error.find("output_publication_state_invalid") != std::string::npos, "The rejected commit should return a stable state diagnostic.");
			context.Expect(!std::filesystem::exists(workspace.FinalDirectory()), "Rejected staging must remain invisible.");
			context.Expect(transaction.Rollback(error), error.empty() ? "Rejected staging should roll back cleanly." : error);
		});

		registry.Add("Editor.ExportOutputTransaction.OverlaysValidatedOutputAndPreservesUnmentionedFiles", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("previous.txt", "previous output");
			workspace.WriteFinalFile("shared.txt", "older shared output");
			ExportOutputTransaction transaction(workspace.FinalDirectory(), "successful-publish");
			std::string error;
			context.Expect(transaction.Begin(error), error.empty() ? "The replacement transaction should begin." : error);
			TempExportOutputWorkspace::WriteText(transaction.StagingDirectory() / "current.txt", "current output");
			TempExportOutputWorkspace::WriteText(transaction.StagingDirectory() / "shared.txt", "new shared output");
			context.Expect(transaction.MarkStagedAndValidated(error), error.empty() ? "The staging directory should be marked validated." : error);
			context.Expect(transaction.Commit(error), error.empty() ? "Validated staging should publish." : error);

			context.Expect(transaction.WasPublished(), "A successful commit should record that publication occurred.");
			context.Expect(transaction.HadPreviousOutput(), "The transaction should record that an older output was replaced.");
			context.Expect(transaction.State() == ExportOutputTransactionState::Cleaned, "A committed transaction should finish in the cleaned state.");
			context.ExpectEqual(std::string("current output"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "current.txt"), "The staged payload should become the visible output.");
			context.ExpectEqual(std::string("new shared output"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "shared.txt"), "A staged file should overwrite the older destination version.");
			context.ExpectEqual(std::string("previous output"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "previous.txt"), "Files absent from staging should remain untouched.");
			context.Expect(!std::filesystem::exists(transaction.BackupDirectory()), "A successful commit should remove its temporary backup.");
			context.Expect(!std::filesystem::exists(transaction.ScratchDirectory()), "A successful commit should remove scratch work.");
			context.Expect(!std::filesystem::exists(transaction.LockPath()), "A successful commit should release the output lock.");
		});

		registry.Add("Editor.ExportOutputTransaction.FailedPublishRestoresPreviousOutput", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "known good output");
			ExportOutputTransaction transaction(workspace.FinalDirectory(), "failed-publish");
			std::string error;
			context.Expect(transaction.Begin(error), error.empty() ? "The replacement transaction should begin." : error);
			TempExportOutputWorkspace::WriteText(transaction.StagingDirectory() / "replacement.txt", "replacement output");
			context.Expect(transaction.MarkStagedAndValidated(error), error.empty() ? "The replacement should reach validation." : error);

			// Removing staging deterministically fails the second publication rename.
			// The implementation has already moved the prior output to its backup at
			// that point, so this exercises the real restoration path on every OS.
			std::filesystem::remove_all(transaction.StagingDirectory());
			context.Expect(!transaction.Commit(error), "A missing staging directory should fail publication.");
			context.Expect(error.find("output_publication_failed") != std::string::npos, "Publication failure should use a stable diagnostic code.");
			context.ExpectEqual(std::string("known good output"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "Failed publication must restore the known-good output.");
			context.Expect(!std::filesystem::exists(transaction.BackupDirectory()), "A restored prior output should no longer remain in the backup path.");
			context.Expect(transaction.Rollback(error), error.empty() ? "The failed transaction should clean up after restoration." : error);
			context.Expect(!std::filesystem::exists(transaction.LockPath()), "Rollback should release the output lock after restoration.");
		});

		registry.Add("Editor.ExportOutputTransaction.DestructorPreservesPreviousOutput", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "preserved");
			std::filesystem::path stagingPath;
			std::filesystem::path lockPath;
			{
				ExportOutputTransaction transaction(workspace.FinalDirectory(), "scope-rollback");
				std::string error;
				context.Expect(transaction.Begin(error), error.empty() ? "The transaction should begin." : error);
				stagingPath = transaction.StagingDirectory();
				lockPath = transaction.LockPath();
				TempExportOutputWorkspace::WriteText(stagingPath / "partial.txt", "partial output");
			}

			context.ExpectEqual(std::string("preserved"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "RAII rollback must preserve the previous successful output.");
			context.Expect(!std::filesystem::exists(stagingPath), "RAII rollback should remove partial staging content.");
			context.Expect(!std::filesystem::exists(lockPath), "RAII rollback should release the lock.");
		});

		registry.Add("Editor.ExportOutputTransaction.RejectsConcurrentWriterBeforeStaging", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "preserved");
			ExportOutputTransaction owner(workspace.FinalDirectory(), "lock-owner");
			std::string error;
			context.Expect(owner.Begin(error), error.empty() ? "The owning transaction should begin." : error);

			ExportOutputTransaction contender(workspace.FinalDirectory(), "lock-contender");
			context.Expect(!contender.Acquire(error), "A second writer should be rejected while the first lock is live.");
			context.Expect(error.find("output_lock_conflict") != std::string::npos, "Concurrent rejection should report the stable lock-conflict code.");
			context.Expect(!std::filesystem::exists(contender.StagingDirectory()), "A rejected writer must not prepare staging content.");
			context.ExpectEqual(std::string("preserved"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "Lock rejection must not alter the visible output.");
			context.Expect(owner.Rollback(error), error.empty() ? "The owner should roll back cleanly." : error);
		});

		registry.Add("Editor.ExportOutputTransaction.ConcurrentWriterStressRejectsEveryContender", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			ExportOutputTransaction owner(workspace.FinalDirectory(), "stress-owner");
			std::string error;
			context.Expect(owner.Begin(error), error.empty() ? "The stress-test owner should begin." : error);

			// Independent contender tasks exercise the atomic create-directory lock
			// concurrently while the owner remains alive for the entire fan-out.
			std::vector<std::future<bool>> contenders;
			for(uint32_t index = 0; index < 12; ++index) {
				contenders.push_back(std::async(std::launch::async, [finalDirectory = workspace.FinalDirectory(), index]() {
					ExportOutputTransactionOptions options;
					options.recoverStaleLocks = false;
					ExportOutputTransaction contender(finalDirectory, "stress-contender-" + std::to_string(index), options);
					std::string contenderError;
					return contender.Acquire(contenderError);
				}));
			}

			uint32_t acquiredCount = 0;
			for(std::future<bool>& contender : contenders) {
				if(contender.get()) {
					++acquiredCount;
				}
			}
			context.ExpectEqual(0U, acquiredCount, "No concurrent contender should acquire the owner's output lock.");
			context.Expect(owner.Rollback(error), error.empty() ? "The stress-test owner should roll back cleanly." : error);
		});

		registry.Add("Editor.ExportOutputTransaction.RecoversDeadOwnerAndInterruptedBackup", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "recover me");
			std::filesystem::path staleLock;
			std::filesystem::path staleMetadataPath;
			std::filesystem::path staleStaging;
			std::filesystem::path staleScratch;
			std::filesystem::path staleBackup;
			std::string staleMetadata;
			{
				ExportOutputTransaction staleOwner(workspace.FinalDirectory(), "stale-run");
				std::string error;
				context.Expect(staleOwner.Acquire(error), error.empty() ? "The metadata fixture lock should be acquired." : error);
				staleLock = staleOwner.LockPath();
				staleMetadataPath = staleOwner.LockMetadataPath();
				staleStaging = staleOwner.StagingDirectory();
				staleScratch = staleOwner.ScratchDirectory();
				staleBackup = staleOwner.BackupDirectory();
				staleMetadata = TempExportOutputWorkspace::ReadText(staleMetadataPath);
			}

			// Recreate the on-disk state left by a process that stopped after moving
			// the known-good output aside. PID 2147483647 is outside normal OS ranges.
			ReplaceMetadataValue(staleMetadata, "processId", "2147483647");
			ReplaceMetadataValue(staleMetadata, "createdUnixSeconds", "0");
			std::filesystem::create_directories(staleLock);
			TempExportOutputWorkspace::WriteText(staleMetadataPath, staleMetadata);
			std::filesystem::create_directories(staleStaging);
			std::filesystem::create_directories(staleScratch);
			TempExportOutputWorkspace::WriteText(staleStaging / "partial.txt", "partial");
			std::filesystem::rename(workspace.FinalDirectory(), staleBackup);

			ExportOutputTransactionOptions recoveryOptions;
			recoveryOptions.staleLockMinimumAge = std::chrono::seconds(0);
			ExportOutputTransaction recovery(workspace.FinalDirectory(), "recovery-run", recoveryOptions);
			std::string error;
			context.Expect(recovery.Acquire(error), error.empty() ? "A dead lock owner should be recovered." : error);
			context.Expect(recovery.RecoveredStaleLock(), "The transaction should expose that stale-lock recovery occurred.");
			context.ExpectEqual(std::string("recover me"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "Recovery must restore the previous successful output.");
			context.Expect(!std::filesystem::exists(staleStaging), "Recovery should remove stale staging content.");
			context.Expect(!std::filesystem::exists(staleScratch), "Recovery should remove stale scratch content.");
			context.Expect(!std::filesystem::exists(staleBackup), "Recovery should move the previous output back from its backup.");
			context.Expect(recovery.Rollback(error), error.empty() ? "The recovery lock should release cleanly." : error);
		});

		registry.Add("Editor.ExportOutputTransaction.RefusesMalformedStaleLock", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "untouched");
			ExportOutputTransaction transaction(workspace.FinalDirectory(), "malformed-lock");
			std::filesystem::create_directories(transaction.LockPath());
			TempExportOutputWorkspace::WriteText(transaction.LockMetadataPath(), "not valid owner metadata\n");

			std::string error;
			context.Expect(!transaction.Acquire(error), "Unverifiable lock metadata must never be deleted automatically.");
			context.Expect(error.find("output_lock_conflict") != std::string::npos, "Malformed metadata should be reported as an output conflict.");
			context.Expect(std::filesystem::exists(transaction.LockPath()), "The unverifiable lock should remain available for manual inspection.");
			context.ExpectEqual(std::string("untouched"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "Malformed lock handling must not alter the final output.");
		});

		registry.Add("Editor.ExportOutputTransaction.CleansSameRunOrphansOnlyAfterLock", [](TestContext& context) {
			TempExportOutputWorkspace workspace;
			workspace.WriteFinalFile("known-good.txt", "still good");
			ExportOutputTransaction transaction(workspace.FinalDirectory(), "orphan-run");
			TempExportOutputWorkspace::WriteText(transaction.StagingDirectory() / "orphan.txt", "orphan staging");
			TempExportOutputWorkspace::WriteText(transaction.ScratchDirectory() / "orphan.txt", "orphan scratch");

			std::string error;
			context.Expect(transaction.Begin(error), error.empty() ? "The transaction should clean reserved same-run orphans after locking." : error);
			context.Expect(!std::filesystem::exists(transaction.StagingDirectory() / "orphan.txt"), "Begin should replace stale same-run staging with an empty directory.");
			context.Expect(!std::filesystem::exists(transaction.ScratchDirectory() / "orphan.txt"), "Begin should replace stale same-run scratch with an empty directory.");
			context.ExpectEqual(std::string("still good"), TempExportOutputWorkspace::ReadText(workspace.FinalDirectory() / "known-good.txt"), "Orphan cleanup must preserve the visible output.");
			context.Expect(transaction.Rollback(error), error.empty() ? "The orphan-cleanup transaction should roll back." : error);
		});
	}
}
