#include "TestHarness.h"

#include "ProfilerCaptureStore.h"
#include "Profiling/ProfilerJournalV1.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempProfilerProject {
		public:
			TempProfilerProject()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ProfilerCaptureStore")
				, outsideRoot_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ProfilerCaptureStoreOutside") {
				std::filesystem::remove_all(root_);
				std::filesystem::remove_all(outsideRoot_);
				std::filesystem::create_directories(root_);
				std::filesystem::create_directories(outsideRoot_);
			}

			~TempProfilerProject() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				std::filesystem::remove_all(outsideRoot_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path Root() const {
				return root_;
			}

			[[nodiscard]] std::filesystem::path ProfilerRoot() const {
				return root_ / "Profiler";
			}

			[[nodiscard]] std::filesystem::path OutsideRoot() const {
				return outsideRoot_;
			}

			void WriteTextFile(const std::filesystem::path& path, const std::string& contents) const {
				std::filesystem::create_directories(path.parent_path());
				std::ofstream stream(path, std::ios::binary | std::ios::trunc);
				stream << contents;
			}

		private:
			std::filesystem::path root_;
			std::filesystem::path outsideRoot_;
		};

		bool FileNameStartsWith(const std::filesystem::path& path, const std::string& prefix) {
			const std::string fileName = path.filename().string();
			return fileName.rfind(prefix, 0U) == 0U;
		}

		bool HasArtifactNamed(const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact>& artifacts, const std::string& fileName) {
			return std::any_of(artifacts.begin(), artifacts.end(), [&fileName](const TerraEngine::Profiling::ProfilerCaptureArtifact& artifact) {
				return artifact.fileName == fileName;
			});
		}

		bool WriteSingleFrameJournal(const std::filesystem::path& path, std::string projectName,
			std::uint64_t frameIndex, std::string& error)
		{
			using namespace TerraEngine::Profiling;
			ProfilerCaptureMetadata metadata;
			metadata.projectName = std::move(projectName);
			metadata.settings.maximumChunkFrames = 1u;
			ProfilerJournalV1Writer writer;
			if (writer.Open(path, metadata, error) != ProfilerJournalV1ResultCode::Ok) return false;
			ProfilerSealedFrame sealed;
			sealed.frame.frameIndex = frameIndex;
			sealed.frame.durationMs = 3.0;
			sealed.frame.complete = true;
			sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(sealed.frame);
			return writer.AppendCommittedSegment(std::span<const ProfilerSealedFrame>(&sealed, 1u), error)
				== ProfilerJournalV1ResultCode::Ok
				&& writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok;
		}
	}

	void RegisterProfilerCaptureStoreTests(TestRegistry& registry) {
		registry.Add("Editor.ProfilerCaptureStore.NoProjectRootFailsClosed", [](TestContext& context) {
			ProfilerCaptureStore store;
			std::filesystem::path outputPath;
			std::string error;
			ProfilerCaptureIndexPage page;

			context.Expect(!store.HasProjectRoot(), "A default capture store should start without an open project.");
			context.Expect(store.GetProfilerDirectory().empty(), "A default capture store should not expose a profiler directory.");
			context.Expect(!store.IsPathInsideProject(std::filesystem::temp_directory_path()), "Path checks should fail when no project is open.");
			context.Expect(!store.ReadJournalIndexPage(std::filesystem::temp_directory_path() / "missing.teprof", 0u, 16u, page, error), "V1 page reads should fail when no project is open.");
			context.Expect(error.find("Profiler captures can only be read") != std::string::npos, "No-project page reads should report the project containment constraint.");

			const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures = store.ListCaptures(error);
			context.Expect(captures.empty(), "Listing captures without a project should return an empty list.");
			context.Expect(error.empty(), "Listing captures without a project should not report a filesystem error.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ReserveJournalKeepsProjectOwnership", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::filesystem::path outputPath;
			std::string error;

			TerraEngine::Profiling::ProfilerCaptureMetadata metadata;
			metadata.projectName = R"(Project:Bad/Name?)";
			context.Expect(store.ReserveJournalArtifact(metadata, outputPath, error), "V1 reservation inside an open project should succeed.");
			context.Expect(store.IsPathInsideProject(outputPath), "Reserved journal paths should stay inside the open project.");
			context.Expect(outputPath.parent_path().filename() == "Profiler", "Reserved journals should be placed under the project Profiler directory.");
			context.Expect(FileNameStartsWith(outputPath, "Project_Bad_Name_-"), "Journal file names should sanitize invalid project-name characters.");
		});

		registry.Add("Editor.ProfilerCaptureStore.LoadRejectsOutsideProjectAndWrongFolder", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::string error;
			ProfilerCaptureIndexPage page;
			const std::filesystem::path outsidePath = project.OutsideRoot() / "outside.teprof";
			const std::filesystem::path wrongFolderPath = project.Root() / "root-level.teprof";

			context.Expect(WriteSingleFrameJournal(outsidePath, "Path Checks", 1u, error), "The outside fixture journal should be writable.");
			context.Expect(WriteSingleFrameJournal(wrongFolderPath, "Path Checks", 2u, error), "The wrong-folder fixture journal should be writable.");

			context.Expect(!store.ReadJournalIndexPage(outsidePath, 0u, 16u, page, error), "The store should reject page reads outside the open project.");
			context.Expect(error.find("Profiler captures can only be read") != std::string::npos, "Outside-project page failures should explain the Profiler folder constraint.");
			context.Expect(!store.ReadJournalIndexPage(wrongFolderPath, 0u, 16u, page, error), "The store should reject page reads inside the project but outside the Profiler folder.");
			context.Expect(error.find("Profiler captures can only be read") != std::string::npos, "Wrong-folder page failures should explain the Profiler folder constraint.");
			context.Expect(!store.IsPathInsideProject(outsidePath), "Path checks should reject outside project paths.");
			context.Expect(store.IsPathInsideProject(wrongFolderPath), "Path checks should accept project-local paths even when loading later applies a stricter folder rule.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ListCapturesReturnsSupportedArtifactsSorted", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::string error;
			const std::filesystem::path profilerRoot = project.ProfilerRoot();
			std::filesystem::create_directories(profilerRoot);

			auto writeJournal = [&error](const std::filesystem::path& path, const std::string& projectName, std::uint64_t frameIndex) {
				TerraEngine::Profiling::ProfilerCaptureMetadata metadata;
				metadata.projectName = projectName;
				metadata.settings.maximumChunkFrames = 1u;
				TerraEngine::Profiling::ProfilerJournalV1Writer writer;
				if (writer.Open(path, metadata, error) != TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok)
					return false;
				TerraEngine::Profiling::ProfilerSealedFrame sealed;
				sealed.frame.frameIndex = frameIndex;
				sealed.frame.complete = true;
				sealed.ownedByteCount = TerraEngine::Profiling::EstimateProfilerFrameOwnedBytes(sealed.frame);
				return writer.AppendCommittedSegment(std::span<const TerraEngine::Profiling::ProfilerSealedFrame>(&sealed, 1u), error) == TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok
					&& writer.Finalize(true, error) == TerraEngine::Profiling::ProfilerJournalV1ResultCode::Ok;
			};
			context.Expect(writeJournal(profilerRoot / "a-first.teprof", "First", 1u), "The first listed V1 journal fixture should be writable: " + error);
			context.Expect(writeJournal(profilerRoot / "z-second.teprof", "Second", 2u), "The second listed V1 journal fixture should be writable: " + error);
			project.WriteTextFile(profilerRoot / "notes.txt", "not a capture");
			project.WriteTextFile(profilerRoot / "broken.teprof", "not a TerraEngine profiler journal");

			const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures = store.ListCaptures(error);

			context.Expect(error.empty(), "Listing a readable profiler directory should not report a filesystem error.");
			context.ExpectEqual(2u, static_cast<unsigned int>(captures.size()), "Listing should include only supported .teprof captures.");
			context.ExpectEqual(std::string("z-second.teprof"), captures[0].fileName, "Listed captures should be sorted newest-name first.");
			context.ExpectEqual(std::string("a-first.teprof"), captures[1].fileName, "Listed captures should preserve descending file-name order.");
			context.Expect(HasArtifactNamed(captures, "a-first.teprof") && HasArtifactNamed(captures, "z-second.teprof"), "Listing should include every valid capture artifact.");
			context.Expect(!HasArtifactNamed(captures, "broken.teprof"), "Listing should ignore unsupported .teprof files.");
			context.ExpectEqual(1ULL, captures[0].frameCount, "Listed artifacts should expose their capture frame count.");
			context.ExpectEqual(std::string("Ready"), captures[0].status, "Capture listing should expose V1 index metadata without loading frame samples.");
			context.ExpectEqual(1ULL, captures[0].committedFrameCount, "Capture listing should expose the recoverable committed-frame boundary.");
			context.Expect(captures[0].complete && !captures[0].recoveredIncompleteTail, "A complete capture should not be presented as a recovered tail.");
			context.ExpectEqual(std::string("Ready"), captures[0].status, "Capture listing should retain the codec's stable ready status when no recovery warning is present.");
			context.ExpectEqual(std::string("Second"), captures[0].metadata.projectName, "Listed artifacts should expose project metadata without fully loading frames.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ListCapturesReadsV1JournalIndexWithoutDocumentDecode", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			const std::filesystem::path path = project.ProfilerRoot() / "indexed-v1.teprof";
			std::filesystem::create_directories(path.parent_path());

			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Indexed V1";
			metadata.settings.maximumChunkFrames = 1u;
			std::string error;
			ProfilerJournalV1Writer writer;
			context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
				"V1 index fixture must reserve and validate its journal header: " + error);
			ProfilerFrameRecord frame;
			frame.frameIndex = 9u;
			frame.durationMs = 4.5;
			frame.complete = true;
			ProfilerSealedFrame sealed;
			sealed.frame = frame;
			sealed.ownedByteCount = EstimateProfilerFrameOwnedBytes(frame);
			context.Expect(writer.AppendCommittedSegment(std::span<const ProfilerSealedFrame>(&sealed, 1u), error) == ProfilerJournalV1ResultCode::Ok,
				"V1 index fixture must commit one bounded segment: " + error);
			context.Expect(writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
				"V1 index fixture must commit a terminal record: " + error);

			const std::vector<ProfilerCaptureArtifact> captures = store.ListCaptures(error);
			context.Expect(error.empty() && captures.size() == 1u,
				"V1 catalog listing should accept the validated journal without decoding frame detail.");
			if (!captures.empty())
			{
				context.ExpectEqual(std::string("Ready"), captures.front().status,
					"The editor catalog bridge should expose the sole V1 journal format state.");
				context.ExpectEqual(1ULL, captures.front().committedFrameCount,
					"The index-only listing must report the authoritative committed frame count.");
				context.ExpectEqual(std::string("Ready"), captures.front().status,
					"A valid V1 terminal should be displayed as a ready capture.");
			}
		});

		registry.Add("Editor.ProfilerCaptureStore.IndexPageRetainsOnlyRequestedRows", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			const std::filesystem::path path = project.ProfilerRoot() / "paged-v1.teprof";
			std::filesystem::create_directories(path.parent_path());
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Paged V1";
			metadata.settings.maximumChunkFrames = 8u;
			std::string error;
			ProfilerJournalV1Writer writer;
			context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
				"The paged V1 fixture must open: " + error);
			std::array<ProfilerSealedFrame, 6u> frames{};
			for (std::size_t index = 0u; index < frames.size(); ++index)
			{
				frames[index].frame.frameIndex = index + 1u;
				frames[index].frame.durationMs = static_cast<double>(index + 1u);
				frames[index].frame.complete = true;
				frames[index].ownedByteCount = EstimateProfilerFrameOwnedBytes(frames[index].frame);
			}
			context.Expect(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok
				&& writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
				"The paged V1 fixture must finalize: " + error);
			ProfilerCaptureIndexPage page;
			context.Expect(store.ReadJournalIndexPage(path, 2u, 2u, page, error),
				"A bounded V1 index page should read without a document decode: " + error);
			context.ExpectEqual(std::size_t(6u), page.totalFrameCount, "Page metadata should expose the total compact index count.");
			context.ExpectEqual(std::size_t(2u), page.frames.size(), "The store must retain only the requested index-page capacity.");
			context.ExpectEqual(std::uint64_t(3u), page.frames.front().frameIndex, "The requested offset should select the third compact frame row.");
		});

		registry.Add("Editor.ProfilerCaptureStore.SelectedFrameReadsOnlyRequestedV1Segment", [](TestContext& context) {
			using namespace TerraEngine::Profiling;
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			const std::filesystem::path path = project.ProfilerRoot() / "selected-frame-v1.teprof";
			std::filesystem::create_directories(path.parent_path());
			ProfilerCaptureMetadata metadata;
			metadata.projectName = "Selected Frame V1";
			metadata.settings.maximumChunkFrames = 2u;
			std::string error;
			ProfilerJournalV1Writer writer;
			context.Expect(writer.Open(path, metadata, error) == ProfilerJournalV1ResultCode::Ok,
				"The selected-frame fixture must open its V1 journal: " + error);
			std::array<ProfilerSealedFrame, 2u> frames{};
			for (std::size_t index = 0u; index < frames.size(); ++index)
			{
				frames[index].frame.frameIndex = 41u + index;
				frames[index].frame.durationMs = 6.0 + static_cast<double>(index);
				frames[index].frame.complete = true;
				frames[index].ownedByteCount = EstimateProfilerFrameOwnedBytes(frames[index].frame);
			}
			context.Expect(writer.AppendCommittedSegment(frames, error) == ProfilerJournalV1ResultCode::Ok
				&& writer.Finalize(true, error) == ProfilerJournalV1ResultCode::Ok,
				"The selected-frame fixture must commit one readable V1 segment: " + error);

			ProfilerFrameRecord selected;
			context.Expect(store.ReadSelectedJournalFrame(path, 42u, selected, error),
				"The store must read one requested V1 frame without exposing the full journal: " + error);
			context.ExpectEqual(std::uint64_t(42u), selected.frameIndex,
				"The selected-frame read must return exactly the requested frame from its committed segment.");
			context.ExpectEqual(7.0, selected.durationMs,
				"The selected-frame reader must retain the requested frame payload instead of an index-only substitute.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ListCapturesStopsWhenCancelled", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::string error;
			const std::filesystem::path profilerRoot = project.ProfilerRoot();
			std::filesystem::create_directories(profilerRoot);

			context.Expect(WriteSingleFrameJournal(profilerRoot / "cancel.teprof", "Cancel", 1u, error), "The cancellation fixture journal should be writable.");

			std::atomic_bool cancelRequested = true;
			const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures = store.ListCaptures(error, [&cancelRequested]() {
				return cancelRequested.load();
			});

			context.Expect(captures.empty(), "Cancelled listing should stop before publishing partial capture artifacts.");
			context.Expect(error.empty(), "Cancelled listing should not look like a filesystem failure.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ProjectRootCanBeResetAndCleared", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store;

			store.SetProjectRoot(project.Root());
			context.Expect(store.HasProjectRoot(), "Setting a project root should open the capture store.");
			context.Expect(store.IsPathInsideProject(project.ProfilerRoot() / "capture.teprof"), "Path checks should accept files below the active project.");
			context.Expect(!store.IsPathInsideProject(project.OutsideRoot() / "capture.teprof"), "Path checks should reject files outside the active project.");

			store.ClearProjectRoot();
			context.Expect(!store.HasProjectRoot(), "Clearing the project root should close the capture store.");
			context.Expect(store.GetProfilerDirectory().empty(), "Clearing the project root should clear the profiler directory.");
		});
	}
}
