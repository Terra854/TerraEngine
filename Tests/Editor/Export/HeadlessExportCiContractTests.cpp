#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		std::string ReadRepositoryText(const std::filesystem::path& relativePath) {
			// Source-level contract tests intentionally read the checked-in workflow.
			// This catches CI drift without requiring a hosted runner for every local test.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream stream(sourcePath, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		bool ContainsInOrder(const std::string& text, const std::vector<std::string_view>& tokens) {
			std::size_t searchOffset = 0;
			for(const std::string_view token : tokens) {
				const std::size_t tokenOffset = text.find(token, searchOffset);
				if(tokenOffset == std::string::npos) {
					return false;
				}
				searchOffset = tokenOffset + token.size();
			}
			return true;
		}

		std::size_t CountOccurrences(const std::string& text, const std::string_view token) {
			std::size_t count = 0;
			std::size_t searchOffset = 0;
			while((searchOffset = text.find(token, searchOffset)) != std::string::npos) {
				++count;
				searchOffset += token.size();
			}
			return count;
		}
	}

	void RegisterHeadlessExportCiContractTests(TestRegistry& registry) {
		registry.Add("Editor.HeadlessExport.CI.WorkflowTriggersAndPermissionsStayBounded", [](TestContext& context) {
			const std::string workflow = ReadRepositoryText(".github/workflows/export-baseline.yml");

			context.Expect(!workflow.empty(), "The headless export workflow should exist at its stable repository path.");
			context.Expect(workflow.find("pull_request:") != std::string::npos, "The export gate should run for pull requests.");
			context.Expect(workflow.find("push:") != std::string::npos, "The export gate should run for main-branch pushes.");
			context.Expect(workflow.find("workflow_dispatch:") != std::string::npos, "The export gate should support manual diagnosis runs.");
			context.Expect(CountOccurrences(workflow, "- main") >= 2U, "Pull-request and push triggers should both name the main branch.");
			context.Expect(workflow.find("contents: read") != std::string::npos, "Workflow repository permission should remain read-only.");
			context.Expect(workflow.find("pull-requests: write") == std::string::npos, "The export gate should not request pull-request write permission.");
			context.Expect(workflow.find("contents: write") == std::string::npos, "The export gate should not request repository write permission.");
		});

		registry.Add("Editor.HeadlessExport.CI.CleanRunnerAndToolchainAreExplicit", [](TestContext& context) {
			const std::string workflow = ReadRepositoryText(".github/workflows/export-baseline.yml");

			context.Expect(workflow.find("runs-on: windows-latest") != std::string::npos, "The supported CI gate should use a Windows hosted runner.");
			context.Expect(workflow.find("timeout-minutes: 60") != std::string::npos, "The export job should enforce the sixty-minute budget.");
			context.Expect(workflow.find("actions/checkout@v4") != std::string::npos, "The workflow should use the supported checkout action.");
			context.Expect(workflow.find("submodules: recursive") != std::string::npos, "A clean runner should initialize every nested dependency recursively.");
			context.Expect(workflow.find("actions/cache") == std::string::npos, "The clean export gate should not restore build or generated-asset caches.");
			context.Expect(workflow.find("ilammy/msvc-dev-cmd@v1") != std::string::npos, "The workflow should initialize x64 MSVC explicitly.");
			context.Expect(workflow.find("actions/setup-dotnet@v4") != std::string::npos, "The workflow should install its supported .NET SDK explicitly.");
			context.Expect(workflow.find("dotnet-version: 10.0.x") != std::string::npos, "The workflow should use the documented .NET 10 SDK line.");
		});

		registry.Add("Editor.HeadlessExport.CI.RequiredStagesRemainDependencyOrdered", [](TestContext& context) {
			const std::string workflow = ReadRepositoryText(".github/workflows/export-baseline.yml");
			const std::vector<std::string_view> orderedStages = {
				"Checkout repository and submodules",
				"Initialize x64 MSVC",
				"Install .NET 10 SDK",
				"Configure Debug",
				"Build export gate targets",
				"Run focused export tests",
				"Run full CTest suite",
				"Install Debug tools and runtime",
				"Verify installed tool layout",
				"Run clean headless export baseline",
				"Validate headless export evidence",
				"Stage export evidence artifact",
				"Upload export evidence artifact"
			};

			context.Expect(ContainsInOrder(workflow, orderedStages), "Configure, build, test, install, export, inspect, and upload stages should remain in dependency order.");
			context.Expect(workflow.find("TerraEngine-EngineTests") != std::string::npos, "The gate build should compile engine tests.");
			context.Expect(workflow.find("TerraEngine-EditorTests") != std::string::npos, "The gate build should compile editor tests.");
			context.Expect(workflow.find("TerraEngineModelPackageCompiler") != std::string::npos, "The gate build should compile the installed baseline asset compiler.");
			context.Expect(workflow.find("TerraEngine-Export") != std::string::npos, "The gate build should compile the installed headless export command.");
			context.Expect(workflow.find("TerraEngine-Editor") != std::string::npos, "The gate should continue compiling the editor adapter that shares export support.");
		});

		registry.Add("Editor.HeadlessExport.CI.FailuresRemainFailuresAndEvidenceAlwaysUploads", [](TestContext& context) {
			const std::string workflow = ReadRepositoryText(".github/workflows/export-baseline.yml");

			context.Expect(CountOccurrences(workflow, "$LASTEXITCODE") >= 6U, "Each native configure, build, test, install, and export command should preserve its exit code.");
			context.Expect(workflow.find("continue-on-error: true") == std::string::npos, "Required stages should not convert failures into successful steps.");
			context.Expect(workflow.find("|| true") == std::string::npos, "The workflow should not mask a failed command through shell fallback syntax.");
			context.Expect(CountOccurrences(workflow, "if: ${{ always() }}") >= 2U, "Artifact staging and upload should run after both success and failure.");
			context.Expect(workflow.find("actions/upload-artifact@v4") != std::string::npos, "The workflow should use the supported artifact action.");
			context.Expect(workflow.find("name: headless-export-evidence-windows-debug") != std::string::npos, "The diagnostic artifact should keep its stable automation name.");
			context.Expect(workflow.find("configure.log") != std::string::npos && workflow.find("build.log") != std::string::npos && workflow.find("ctest.log") != std::string::npos && workflow.find("install.log") != std::string::npos && workflow.find("baseline-prepare.log") != std::string::npos, "The always-run artifact should retain logs from every required native stage.");
		});

		registry.Add("Editor.HeadlessExport.CI.BaselineInspectionRejectsIncompleteEvidence", [](TestContext& context) {
			const std::string workflow = ReadRepositoryText(".github/workflows/export-baseline.yml");
			const std::vector<std::string_view> evidenceFiles = {
				"export-result.json",
				"resolved-manifest.json",
				"content-inventory.json",
				"validation-report.json",
				"progress.jsonl",
				"package-validation.txt"
			};

			context.Expect(ContainsInOrder(workflow, evidenceFiles), "The workflow should require the complete six-file evidence bundle in contract order.");
			context.Expect(workflow.find("$result.schemaVersion -ne 1") != std::string::npos, "The workflow should reject an incompatible result schema.");
			context.Expect(workflow.find("$inventory.entryCount -le 0") != std::string::npos, "The workflow should reject an empty baseline inventory.");
			context.Expect(workflow.find("$_.required -and $_.placeholderDetected") != std::string::npos, "The workflow should reject placeholder-marked required content.");
			context.Expect(workflow.find("$_.required -and $_.byteSize -le 0") != std::string::npos, "The workflow should reject empty required content.");
			context.Expect(workflow.find("$finalRecords.Count -ne 1") != std::string::npos, "The workflow should require exactly one final progress result.");
			context.Expect(workflow.find("Manifest asset references: PASS") != std::string::npos, "Package inspection should prove every manifest asset reference resolves in TEPAK.");
			context.Expect(workflow.find("Manifest evidence match: PASS") != std::string::npos, "Package inspection should prove archived game.json bytes match resolved-manifest evidence.");
			context.Expect(workflow.find("Placeholder scan: PASS") != std::string::npos, "Package inspection should report a clean placeholder scan.");
			context.Expect(workflow.find("Final: PASS") != std::string::npos, "Package inspection should end with an unambiguous pass signal.");
		});
	}
}
