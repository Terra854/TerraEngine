#include "TestHarness.h"

#include "ExportEvidence.h"
#include "ExportRuntimeLayout.h"
#include "ExportTypes.h"
#include "HeadlessExportCommandLine.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempHeadlessContractDirectory {
		public:
			TempHeadlessContractDirectory()
				: root_(std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests" / "Headless Export Contract") {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				std::filesystem::create_directories(root_);
			}

			~TempHeadlessContractDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

			std::filesystem::path WriteText(
				const std::filesystem::path& relativePath,
				const std::string& contents) const
			{
				const std::filesystem::path path = root_ / relativePath;
				std::filesystem::create_directories(path.parent_path());
				std::ofstream output(path, std::ios::binary | std::ios::trunc);
				output << contents;
				return path;
			}

		private:
			std::filesystem::path root_;
		};

		struct HeadlessContractFixture {
			std::filesystem::path executable;
			std::filesystem::path project;
			std::filesystem::path profile;
			std::filesystem::path output;
			std::filesystem::path evidence;
		};

		void Touch(const std::filesystem::path& path, const std::string& contents = "fixture") {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output << contents;
		}

		std::string PathArgument(const std::filesystem::path& path) {
			// The CLI contract defines argv path bytes as UTF-8. Convert through the
			// filesystem's char8_t view so Windows never consults its legacy code page.
			const std::u8string utf8 = path.generic_u8string();
			return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
		}

		HeadlessContractFixture MakeValidFixture(
			const TempHeadlessContractDirectory& tempDirectory,
			bool useUnicodePaths = false)
		{
			HeadlessContractFixture fixture;
			const std::filesystem::path installRoot = tempDirectory.Root() / "Installed Runtime";
			const std::filesystem::path editorDirectory = installRoot / "Editor";
			// Only binaries copied into a game belong to the export runtime payload.
			const std::filesystem::path exportRuntimeDirectory = editorDirectory / "ExportRuntime";

#ifdef _WIN32
			fixture.executable = editorDirectory / "TerraEngine-Export.exe";
			const ExportPlatform platform = ExportPlatform::Windows;
#else
			fixture.executable = editorDirectory / "TerraEngine-Export";
			const ExportPlatform platform = ExportPlatform::Linux;
#endif
			Touch(fixture.executable);
			Touch(editorDirectory / ("TerraEngineModelPackageCompiler" + GetExportExecutableSuffix(platform)));
			Touch(exportRuntimeDirectory / GetExportLauncherFileName("TerraEngine-Launcher", platform));
			Touch(exportRuntimeDirectory / GetExportEngineLibraryName(platform));
			Touch(exportRuntimeDirectory / GetExportGlfwLibraryName(platform));
			std::filesystem::create_directories(editorDirectory / "Shaders");
			std::filesystem::create_directories(editorDirectory / "Fonts");
			std::filesystem::create_directories(editorDirectory / "Managed");
			std::filesystem::create_directories(editorDirectory / "DotNet");
#ifdef _WIN32
			Touch(editorDirectory / "DotNet" / "dotnet.exe");
#else
			Touch(editorDirectory / "DotNet" / "dotnet");
#endif
			std::filesystem::create_directories(editorDirectory / "NativeAotNuGet" / "feed");
			Touch(editorDirectory / "NativeAotNuGet" / "NuGet.Config");
			Touch(editorDirectory / "NativeAotNuGet" / "global.json");

			fixture.project = tempDirectory.Root() / (useUnicodePaths
				? std::filesystem::path(std::u8string(u8"Project \u00e9 space"))
				: std::filesystem::path("Project With Spaces"));
			Touch(fixture.project / "game.json", R"({"name":"Contract fixture"})");
			fixture.profile = tempDirectory.Root() / "Profiles" / "headless.json";
			Touch(fixture.profile,
				std::string("{\n")
				+ "\t\"name\": \"Headless Contract\",\n"
				+ "\t\"id\": \"headless-contract\",\n"
				+ "\t\"platform\": \"" + ExportPlatformToString(platform) + "\",\n"
				+ "\t\"outputDirectory\": \"Builds/StoredValue\",\n"
				+ "\t\"startupScene\": \"Scenes/Startup.tescene\",\n"
				+ "\t\"enableManagedCompilation\": false\n"
				+ "}\n");
			fixture.output = tempDirectory.Root() / (useUnicodePaths
				? std::filesystem::path(std::u8string(u8"Published \u00e9"))
				: std::filesystem::path("Published Output"));
			fixture.evidence = tempDirectory.Root() / (useUnicodePaths
				? std::filesystem::path(std::u8string(u8"Evidence \u00e9"))
				: std::filesystem::path("Evidence Output"));
			return fixture;
		}

		std::vector<std::string> ValidArguments(const HeadlessContractFixture& fixture) {
			return {
				"--project", PathArgument(fixture.project),
				"--profile", PathArgument(fixture.profile),
				"--output", PathArgument(fixture.output),
				"--evidence", PathArgument(fixture.evidence),
				"--run-id", "contract-run"
			};
		}
	}

	void RegisterHeadlessExportContractTests(TestRegistry& registry) {
		registry.Add("Editor.HeadlessExport.HelpDoesNotReadProjectOrInstalledRuntime", [](TestContext& context) {
			const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
				{"--help"},
				"Z:/definitely-missing/Editor/TerraEngine-Export.exe",
				"Z:/definitely-missing");

			context.Expect(parsed.success, "Help should succeed before any filesystem or runtime-layout checks.");
			context.Expect(parsed.showHelp, "Help parsing should tell the caller to print usage instead of exporting.");
			context.Expect(parsed.failureCategory == ExportFailureCategory::None, "Help should map to the successful process result.");
			context.Expect(GetHeadlessExportUsage().find("--project <project-root>") != std::string::npos, "Usage should document the explicit project argument.");
		});

		registry.Add("Editor.HeadlessExport.RejectsDuplicateUnknownAndMissingArguments", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			const HeadlessContractFixture fixture = MakeValidFixture(tempDirectory);
			std::vector<std::string> duplicateArguments = ValidArguments(fixture);
			duplicateArguments.insert(duplicateArguments.end(), {"--output", PathArgument(tempDirectory.Root() / "Other")});

			const HeadlessExportParseResult duplicate = ParseHeadlessExportCommandLine(
				duplicateArguments, fixture.executable, tempDirectory.Root());
			const HeadlessExportParseResult unknown = ParseHeadlessExportCommandLine(
				{"--project", PathArgument(fixture.project), "--bogus", "value"},
				fixture.executable, tempDirectory.Root());
			const HeadlessExportParseResult missingValue = ParseHeadlessExportCommandLine(
				{"--project"}, fixture.executable, tempDirectory.Root());

			context.Expect(!duplicate.success && duplicate.message.find("Duplicate") != std::string::npos, "A duplicate option should be rejected rather than silently overwritten.");
			context.Expect(!unknown.success && unknown.message.find("Unknown") != std::string::npos, "Unknown options should fail with an actionable parser message.");
			context.Expect(!missingValue.success && missingValue.message.find("Missing value") != std::string::npos, "Options without values should fail before filesystem checks.");
			context.Expect(duplicate.failureCategory == ExportFailureCategory::InvalidArguments, "Parser contract failures should use invalid_arguments.");
		});

		registry.Add("Editor.HeadlessExport.ParsesSpacesUnicodeAndRequestLocalOutputOverride", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			const HeadlessContractFixture fixture = MakeValidFixture(tempDirectory, true);

			const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
				ValidArguments(fixture), fixture.executable, tempDirectory.Root());

			context.Expect(parsed.success, "Host-supported paths containing spaces and non-ASCII characters should parse successfully.");
			context.Expect(fixture.output.lexically_normal() == parsed.request.finalOutputDirectory, "The explicit output should become the final publication path.");
			context.Expect(fixture.evidence.lexically_normal() == parsed.request.evidenceDirectory,
				"An explicit evidence path should remain available for CI and audit workflows.");
			context.Expect(fixture.output.lexically_normal() == parsed.request.profile.outputDirectory, "The output override should affect only the request-local profile copy.");
			context.ExpectEqual(std::string("contract-run"), parsed.request.runId, "The test run id should remain stable for evidence correlation.");

			const BuildProfile sourceProfile = LoadBuildProfile(fixture.profile);
			context.Expect(std::filesystem::path("Builds/StoredValue") == sourceProfile.outputDirectory, "Parsing must not persist the command-line output override to the profile source.");
		});

		registry.Add("Editor.HeadlessExport.RejectsProjectWithoutCanonicalGameManifest", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			const HeadlessContractFixture fixture = MakeValidFixture(tempDirectory);
			std::filesystem::remove(fixture.project / "game.json");
			Touch(fixture.project / "project.json", R"({"name":"Unsupported project marker"})");

			const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
				ValidArguments(fixture), fixture.executable, tempDirectory.Root());

			context.Expect(!parsed.success,
				"Headless export must reject a directory that lacks the editor's canonical game.json marker.");
			context.Expect(parsed.message.find("game.json") != std::string::npos,
				"The invalid-project error should identify the required canonical manifest.");
		});

		registry.Add("Editor.HeadlessExport.ResolvesRelativePathsAgainstLaunchDirectory", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			const HeadlessContractFixture fixture = MakeValidFixture(tempDirectory);
			const std::filesystem::path launchDirectory = tempDirectory.Root();
			const std::vector<std::string> arguments = {
				"--project", PathArgument(fixture.project.lexically_relative(launchDirectory)),
				"--profile", PathArgument(fixture.profile.lexically_relative(launchDirectory)),
				"--output", PathArgument(fixture.output.lexically_relative(launchDirectory)),
				"--run-id", "relative-run"
			};

			const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
				arguments, fixture.executable, launchDirectory);

			context.Expect(parsed.success, "Relative request paths should resolve against the captured launch directory.");
			context.Expect(std::filesystem::canonical(fixture.project) == parsed.request.projectRoot, "Project canonicalization should not depend on a later current-directory change.");
			context.Expect(fixture.output.lexically_normal() == parsed.request.finalOutputDirectory, "A not-yet-created output should still resolve deterministically.");
			context.Expect(
				parsed.request.evidenceDirectory
					== parsed.request.projectRoot / "Library" / "ExportEvidence" / "relative-run",
				"Default evidence should stay in disposable project Library state instead of beside published output.");
		});

		registry.Add("Editor.HeadlessExport.RejectsMalformedProfileAndMissingRuntimeFile", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			HeadlessContractFixture fixture = MakeValidFixture(tempDirectory);
			Touch(fixture.profile, "{ malformed json");

			const HeadlessExportParseResult malformed = ParseHeadlessExportCommandLine(
				ValidArguments(fixture), fixture.executable, tempDirectory.Root());
			context.Expect(!malformed.success, "Malformed standalone profile JSON should be rejected before export begins.");
			context.Expect(malformed.failureCategory == ExportFailureCategory::InvalidArguments, "Malformed profiles should use the stable invalid_arguments category.");

			fixture = MakeValidFixture(tempDirectory);
			std::filesystem::remove(fixture.executable.parent_path()
				/ ("TerraEngineModelPackageCompiler" + GetExportExecutableSuffix(GetHostExportPlatform())));
			const HeadlessExportParseResult missingRuntime = ParseHeadlessExportCommandLine(
				ValidArguments(fixture), fixture.executable, tempDirectory.Root());
			context.Expect(!missingRuntime.success, "A missing installed compiler should reject the executable-relative runtime layout.");
			context.Expect(missingRuntime.failureCategory == ExportFailureCategory::InvalidRuntimeLayout, "Missing installed files should use invalid_runtime_layout.");
		});

		registry.Add("Editor.HeadlessExport.ParsingDoesNotModifyExistingReadOnlyOutput", [](TestContext& context) {
			TempHeadlessContractDirectory tempDirectory;
			const HeadlessContractFixture fixture = MakeValidFixture(tempDirectory);
			const std::filesystem::path sentinel = fixture.output / "previous-success.txt";
			Touch(sentinel, "old successful export");
			std::filesystem::permissions(sentinel, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace);

			const HeadlessExportParseResult parsed = ParseHeadlessExportCommandLine(
				ValidArguments(fixture), fixture.executable, tempDirectory.Root());
			std::ifstream input(sentinel, std::ios::binary);
			std::string contents;
			std::getline(input, contents);

			context.Expect(parsed.success, "Argument parsing should defer publication and output-permission checks to the transaction boundary.");
			context.ExpectEqual(std::string("old successful export"), contents, "Parsing must never clean or modify a previous published output.");
			std::filesystem::permissions(sentinel, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
		});

		registry.Add("Editor.HeadlessExport.JsonRecordsAndExitMappingsAreStable", [](TestContext& context) {
			ExportProgress progress;
			progress.phase = ExportPhase::Packaging;
			progress.stage = ExportStage::Packaging;
			progress.overallProgress = 0.65f;
			progress.statusMessage = "Packaging project content.";
			const nlohmann::json progressJson = nlohmann::json::parse(
				SerializeExportProgressRecord("json-contract", progress));

			ExportResult result;
			result.runId = "json-contract";
			result.success = false;
			result.stage = ExportStage::PackageValidation;
			result.failureCategory = ExportFailureCategory::PackageValidationFailed;
			result.exitCode = ExitCodeForFailure(result.failureCategory);
			result.message = "Archive inspection failed.";
			const nlohmann::json resultJson = nlohmann::json::parse(SerializeExportResultRecord(result));

			context.ExpectEqual(std::string("progress"), progressJson.at("type").get<std::string>(), "Every progress line should identify its record type.");
			context.ExpectEqual(1, progressJson.at("schemaVersion").get<int>(), "Progress records should retain schema version 1.");
			context.ExpectEqual(std::string("result"), resultJson.at("type").get<std::string>(), "The final line should identify itself as the result record.");
			context.ExpectEqual(std::string("package_validation_failed"), resultJson.at("failureCategory").get<std::string>(), "Result records should serialize the stable category text.");

			const std::array<std::pair<ExportFailureCategory, int>, 13> expectedMappings = {{
				{ExportFailureCategory::None, 0},
				{ExportFailureCategory::InvalidArguments, 2},
				{ExportFailureCategory::InvalidRuntimeLayout, 2},
				{ExportFailureCategory::OutputConflict, 3},
				{ExportFailureCategory::ValidationFailed, 4},
				{ExportFailureCategory::ManagedCompilationFailed, 5},
				{ExportFailureCategory::AssetStagingFailed, 6},
				{ExportFailureCategory::PackagingFailed, 7},
				{ExportFailureCategory::PackageValidationFailed, 7},
				{ExportFailureCategory::RuntimeStagingFailed, 8},
				{ExportFailureCategory::EvidenceWriteFailed, 8},
				{ExportFailureCategory::PublicationFailed, 9},
				{ExportFailureCategory::UnexpectedFailure, 10}
			}};
			for(const auto& [category, exitCode] : expectedMappings) {
				context.ExpectEqual(exitCode, ExitCodeForFailure(category), "Every failure category should keep its documented automation exit code.");
			}
		});
	}
}
