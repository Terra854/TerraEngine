#include "TestHarness.h"

#include "ProfilerCaptureStore.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

		TerraEngine::Profiling::ProfilerCaptureDocument MakeProfilerDocument(const std::string& projectName) {
			using namespace TerraEngine::Profiling;

			ProfilerCaptureDocument document;
			document.metadata.terraEngineVersion = "unit-test";
			document.metadata.projectName = projectName;
			document.metadata.scenePath = "Scenes/Test.tescene";
			document.metadata.startedAt = "2026-05-29 12:00:00";
			document.metadata.savedAt = "2026-05-29 12:00:01";
			document.metadata.settings.maxFrames = 32U;

			ProfilerModuleSummary module;
			module.moduleId = ProfilerModuleId::CpuEngine;
			module.available = true;
			module.totalMs = 3.0;
			module.metrics.push_back({"Frame duration", 3.0, "ms"});

			ProfilerFrameRecord frame;
			frame.frameIndex = 7U;
			frame.frameStartMs = 21.0;
			frame.durationMs = 3.0;
			frame.complete = true;
			frame.moduleSummaries.push_back(module);
			frame.samples.push_back({1U, 0U, ProfilerModuleId::CpuEngine, "Editor Frame", "main", "", 0.0, 3.0, 0U, 1U, true});
			document.frames.push_back(std::move(frame));

			return document;
		}

		bool FileNameStartsWith(const std::filesystem::path& path, const std::string& prefix) {
			const std::string fileName = path.filename().string();
			return fileName.rfind(prefix, 0U) == 0U;
		}

		bool HasArtifactNamed(const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact>& artifacts, const std::string& fileName) {
			return std::any_of(artifacts.begin(), artifacts.end(), [&fileName](const TerraEngine::Profiling::ProfilerCaptureArtifact& artifact) {
				return artifact.fileName == fileName;
			});
		}
	}

	void RegisterProfilerCaptureStoreTests(TestRegistry& registry) {
		registry.Add("Editor.ProfilerCaptureStore.NoProjectRootFailsClosed", [](TestContext& context) {
			ProfilerCaptureStore store;
			std::filesystem::path outputPath;
			std::string error;
			TerraEngine::Profiling::ProfilerCaptureDocument loaded;

			context.Expect(!store.HasProjectRoot(), "A default capture store should start without an open project.");
			context.Expect(store.GetProfilerDirectory().empty(), "A default capture store should not expose a profiler directory.");
			context.Expect(!store.IsPathInsideProject(std::filesystem::temp_directory_path()), "Path checks should fail when no project is open.");
			context.Expect(!store.SaveNewCapture(MakeProfilerDocument("No Project"), outputPath, error), "Saving should fail when no project is open.");
			context.ExpectEqual(std::string("No project is open."), error, "Save failures without a project should report a clear diagnostic.");
			context.Expect(!store.LoadCapture(std::filesystem::temp_directory_path() / "missing.teprof", loaded, error), "Loading should fail when no project is open.");
			context.ExpectEqual(std::string("No project is open."), error, "Load failures without a project should report a clear diagnostic.");

			const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures = store.ListCaptures(error);
			context.Expect(captures.empty(), "Listing captures without a project should return an empty list.");
			context.Expect(error.empty(), "Listing captures without a project should not report a filesystem error.");
		});

		registry.Add("Editor.ProfilerCaptureStore.SaveNewCaptureWritesAndLoadsProjectCapture", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::filesystem::path outputPath;
			std::string error;

			const TerraEngine::Profiling::ProfilerCaptureDocument document = MakeProfilerDocument(R"(Project:Bad/Name?)");
			context.Expect(store.SaveNewCapture(document, outputPath, error), "Saving a capture inside an open project should succeed.");
			context.Expect(std::filesystem::exists(outputPath), "Saving a capture should create a profiler artifact on disk.");
			context.Expect(store.IsPathInsideProject(outputPath), "Saved capture paths should stay inside the open project.");
			context.Expect(outputPath.parent_path().filename() == "Profiler", "Saved captures should be placed under the project Profiler directory.");
			context.Expect(FileNameStartsWith(outputPath, "Project_Bad_Name_-"), "Capture file names should sanitize invalid project-name characters.");

			TerraEngine::Profiling::ProfilerCaptureDocument loaded;
			context.Expect(store.LoadCapture(outputPath, loaded, error), "Saved project captures should load through the capture store.");
			context.ExpectEqual(std::string(R"(Project:Bad/Name?)"), loaded.metadata.projectName, "Capture metadata should preserve the authored project name.");
			context.ExpectEqual(project.Root().string(), loaded.metadata.projectPathHint, "Saved captures should record the current project path hint.");
			context.Expect(loaded.metadata.settings.captureMode == TerraEngine::Profiling::ProfilerCaptureMode::LoadedReadOnly, "Loaded captures should be marked read-only.");
			context.Expect(loaded.sourcePath.has_value(), "Loaded captures should remember their canonical source path.");
			context.ExpectEqual(1u, static_cast<unsigned int>(loaded.frames.size()), "Loaded captures should preserve saved frame data.");
			context.ExpectEqual(std::string("Editor Frame"), loaded.frames.front().samples.front().name, "Loaded captures should preserve sample names.");
		});

		registry.Add("Editor.ProfilerCaptureStore.LoadRejectsOutsideProjectAndWrongFolder", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::string error;
			TerraEngine::Profiling::ProfilerCaptureDocument loaded;
			const TerraEngine::Profiling::ProfilerCaptureDocument document = MakeProfilerDocument("Path Checks");
			const std::filesystem::path outsidePath = project.OutsideRoot() / "outside.teprof";
			const std::filesystem::path wrongFolderPath = project.Root() / "root-level.teprof";

			context.Expect(TerraEngine::Profiling::SaveCaptureDocument(document, outsidePath, error), "The outside fixture capture should be writable.");
			context.Expect(TerraEngine::Profiling::SaveCaptureDocument(document, wrongFolderPath, error), "The wrong-folder fixture capture should be writable.");

			context.Expect(!store.LoadCapture(outsidePath, loaded, error), "The store should reject captures outside the open project.");
			context.Expect(error.find("Profiler captures can only be loaded") != std::string::npos, "Outside-project load failures should explain the Profiler folder constraint.");
			context.Expect(!store.LoadCapture(wrongFolderPath, loaded, error), "The store should reject captures inside the project but outside the Profiler folder.");
			context.Expect(error.find("Profiler captures can only be loaded") != std::string::npos, "Wrong-folder load failures should explain the Profiler folder constraint.");
			context.Expect(!store.IsPathInsideProject(outsidePath), "Path checks should reject outside project paths.");
			context.Expect(store.IsPathInsideProject(wrongFolderPath), "Path checks should accept project-local paths even when loading later applies a stricter folder rule.");
		});

		registry.Add("Editor.ProfilerCaptureStore.ListCapturesReturnsSupportedArtifactsSorted", [](TestContext& context) {
			TempProfilerProject project;
			ProfilerCaptureStore store(project.Root());
			std::string error;
			const std::filesystem::path profilerRoot = project.ProfilerRoot();
			std::filesystem::create_directories(profilerRoot);

			context.Expect(TerraEngine::Profiling::SaveCaptureDocument(MakeProfilerDocument("First"), profilerRoot / "a-first.teprof", error), "The first listed capture fixture should be writable.");
			context.Expect(TerraEngine::Profiling::SaveCaptureDocument(MakeProfilerDocument("Second"), profilerRoot / "z-second.teprof", error), "The second listed capture fixture should be writable.");
			project.WriteTextFile(profilerRoot / "notes.txt", "not a capture");
			project.WriteTextFile(profilerRoot / "broken.teprof", R"({"format":"Wrong","payloadVersion":1,"frames":[]})");

			const std::vector<TerraEngine::Profiling::ProfilerCaptureArtifact> captures = store.ListCaptures(error);

			context.Expect(error.empty(), "Listing a readable profiler directory should not report a filesystem error.");
			context.ExpectEqual(2u, static_cast<unsigned int>(captures.size()), "Listing should include only supported .teprof captures.");
			context.ExpectEqual(std::string("z-second.teprof"), captures[0].fileName, "Listed captures should be sorted newest-name first.");
			context.ExpectEqual(std::string("a-first.teprof"), captures[1].fileName, "Listed captures should preserve descending file-name order.");
			context.Expect(HasArtifactNamed(captures, "a-first.teprof") && HasArtifactNamed(captures, "z-second.teprof"), "Listing should include every valid capture artifact.");
			context.Expect(!HasArtifactNamed(captures, "broken.teprof"), "Listing should ignore unsupported .teprof files.");
			context.ExpectEqual(1ULL, captures[0].frameCount, "Listed artifacts should expose their capture frame count.");
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
