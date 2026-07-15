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
			// Documentation is an executable contract for contributors. Reading it
			// from the source tree makes stale local commands fail in the editor suite.
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
		registry.Add("Editor.HeadlessExport.Documentation.LocalCommandsRemainReproducible", [](TestContext& context) {
			const std::string quickstart = ReadDocumentationText("specs/043-optimize-engine-performance/quickstart.md");
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> sharedCommandTokens = {
				"cmake --preset RelWithDebInfo",
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

			context.Expect(ContainsEvery(quickstart, sharedCommandTokens), "The feature quickstart should reproduce the local validation command surface.");
			context.Expect(ContainsEvery(baselineGuide, sharedCommandTokens), "The baseline guide should reproduce the local validation command surface.");
			context.Expect(ContainsEvery(testingGuide, sharedCommandTokens), "The testing guide should reproduce the local validation command surface.");
		});

		registry.Add("Editor.HeadlessExport.Documentation.InstalledPathsStayAuthoritative", [](TestContext& context) {
			const std::string quickstart = ReadDocumentationText("specs/043-optimize-engine-performance/quickstart.md");
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> installedPaths = {
				"out/build/RelWithDebInfo/Build/Editor/TerraEngine-Export.exe",
				"out/build/RelWithDebInfo/Build/Editor/TerraEngineModelPackageCompiler.exe",
				"out/build/RelWithDebInfo/Build/Editor/Managed/TerraEngine.CSharp.dll",
				"out/build/RelWithDebInfo/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll",
				"out/build/RelWithDebInfo/Build/Launcher/TerraEngine-Launcher.exe"
			};

			context.Expect(ContainsEvery(quickstart, installedPaths), "The quickstart should list every installed export dependency.");
			context.Expect(ContainsEvery(baselineGuide, installedPaths), "The baseline guide should list every installed export dependency.");
			context.Expect(ContainsEvery(testingGuide, installedPaths), "The testing guide should list every installed export dependency.");
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

		registry.Add("Editor.HeadlessExport.Documentation.LocalGateScopeIsDiscoverable", [](TestContext& context) {
			const std::string baselineGuide = ReadDocumentationText("Docs/ExportRuntimeBaseline.md");
			const std::string testingGuide = ReadDocumentationText("Docs/Testing.md");
			const std::vector<std::string_view> discoveryTokens = {
				"Windows x64",
				"local",
				"RelWithDebInfo",
				"out/ci/export-runtime-baseline"
			};

			context.Expect(ContainsEvery(baselineGuide, discoveryTokens), "The baseline guide should document the supported local validation scope and retained work root.");
			context.Expect(ContainsEvery(testingGuide, discoveryTokens), "The testing guide should document the supported local validation scope and retained work root.");
			context.Expect(baselineGuide.find(".github/workflows/export-baseline.yml") == std::string::npos, "The baseline guide should not advertise the removed hosted workflow.");
			context.Expect(testingGuide.find(".github/workflows/export-baseline.yml") == std::string::npos, "The testing guide should not advertise the removed hosted workflow.");
		});
	}
}
