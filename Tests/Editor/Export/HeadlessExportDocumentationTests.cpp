#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		std::string ReadDocumentationText(const std::filesystem::path& relativePath) {
			// Documentation is an executable contract for contributors and CI. Read
			// from the source tree so stale commands fail in the normal editor suite.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream stream(sourcePath, std::ios::binary);
			return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
		}

		bool ContainsEvery(const std::string& text, const std::vector<std::string_view>& tokens) {
			for(const std::string_view token : tokens) {
				if(text.find(token) == std::string::npos) {
					return false;
				}
			}
			return true;
		}
	}

	void RegisterHeadlessExportDocumentationTests(TestRegistry& registry) {
		registry.Add("Editor.HeadlessExport.Documentation.LocalCommandsMatchCiGate", [](TestContext& context) {
			const std::string workflow = ReadDocumentationText(".github/workflows/export-baseline.yml");
			const std::string quickstart = ReadDocumentationText("specs/038-headless-export-ci/quickstart.md");
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> sharedCommandTokens = {
				"cmake --preset Debug",
				"TerraEngine-EngineTests",
				"TerraEngine-EditorTests",
				"TerraEngineModelPackageCompiler",
				"TerraEngine-Export",
				"TerraEngine-Editor",
				"Editor.ExportUnitTests|Editor.HeadlessExport",
				"--output-on-failure",
				"--target install",
				"Tools/ExportRuntimeBaseline/RunBaseline.ps1",
				"-InstallRoot",
				"out/ci/export-runtime-baseline"
			};

			context.Expect(ContainsEvery(workflow, sharedCommandTokens), "The workflow should contain every authoritative configure, build, test, install, and baseline token.");
			context.Expect(ContainsEvery(quickstart, sharedCommandTokens), "The feature quickstart should reproduce the workflow command surface.");
			context.Expect(ContainsEvery(baselineGuide, sharedCommandTokens), "The baseline guide should reproduce the workflow command surface.");
			context.Expect(ContainsEvery(testingGuide, sharedCommandTokens), "The testing guide should reproduce the workflow command surface.");
		});

		registry.Add("Editor.HeadlessExport.Documentation.InstalledPathsStayAuthoritative", [](TestContext& context) {
			const std::string quickstart = ReadDocumentationText("specs/038-headless-export-ci/quickstart.md");
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string workflow = ReadDocumentationText(".github/workflows/export-baseline.yml");
			const std::vector<std::string_view> installedPaths = {
				"out/build/Debug/Build/Editor/TerraEngine-Export.exe",
				"out/build/Debug/Build/Editor/TerraEngineModelPackageCompiler.exe",
				"out/build/Debug/Build/Editor/Managed/TerraEngine.CSharp.dll",
				"out/build/Debug/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll",
				"out/build/Debug/Build/Launcher/TerraEngine-Launcher.exe"
			};

			context.Expect(ContainsEvery(quickstart, installedPaths), "The quickstart should list every installed export dependency.");
			context.Expect(ContainsEvery(baselineGuide, installedPaths), "The baseline guide should list every installed export dependency.");
			context.Expect(workflow.find("INSTALL_ROOT: out/build/Debug/Build") != std::string::npos, "The workflow should verify the documented install root.");
			context.Expect(ContainsEvery(workflow, {"Editor/TerraEngine-Export.exe", "Editor/TerraEngineModelPackageCompiler.exe", "Editor/Managed/TerraEngine.CSharp.dll", "Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll", "Launcher/TerraEngine-Launcher.exe"}), "The workflow should verify every documented installed dependency before export.");
		});

		registry.Add("Editor.HeadlessExport.Documentation.EvidenceAndFailureCategoriesAreComplete", [](TestContext& context) {
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> evidenceFiles = {
				"export-result.json",
				"resolved-manifest.json",
				"content-inventory.json",
				"validation-report.json",
				"progress.jsonl",
				"package-validation.txt"
			};
			const std::vector<std::string_view> failureCategories = {
				"invalid_arguments",
				"invalid_runtime_layout",
				"output_conflict",
				"validation_failed",
				"managed_compilation_failed",
				"asset_staging_failed",
				"packaging_failed",
				"package_validation_failed",
				"runtime_staging_failed",
				"evidence_write_failed",
				"publication_failed",
				"unexpected_failure"
			};

			context.Expect(ContainsEvery(baselineGuide, evidenceFiles), "The baseline guide should name all six evidence files.");
			context.Expect(ContainsEvery(testingGuide, evidenceFiles), "The testing guide should name all six evidence files.");
			context.Expect(ContainsEvery(baselineGuide, failureCategories), "The failure guide should list every stable headless category.");
			context.Expect(ContainsEvery(testingGuide, failureCategories), "The testing guide should keep the same stable failure categories as the baseline guide.");
		});

		registry.Add("Editor.HeadlessExport.Documentation.WorkflowScopeAndArtifactAreDiscoverable", [](TestContext& context) {
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> discoveryTokens = {
				".github/workflows/export-baseline.yml",
				"headless-export-evidence-windows-debug",
				"Windows x64",
				"60-minute",
				"always"
			};

			context.Expect(ContainsEvery(baselineGuide, discoveryTokens), "The baseline guide should document the supported workflow scope and retained artifact.");
			context.Expect(testingGuide.find(".github/workflows/export-baseline.yml") != std::string::npos, "The testing guide should link contributors to the authoritative workflow path.");
			context.Expect(testingGuide.find("headless-export-evidence-windows-debug") != std::string::npos, "The testing guide should name the retained CI artifact.");
		});
	}
}
