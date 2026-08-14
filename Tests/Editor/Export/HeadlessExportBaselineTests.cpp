#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		std::filesystem::path RepositoryRoot() {
			return (std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "..").lexically_normal();
		}

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		void ExpectContains(TestContext& context, const std::string& text, const std::string& token, std::string_view message) {
			context.Expect(text.find(token) != std::string::npos, message);
		}
	}

	void RegisterHeadlessExportBaselineTests(TestRegistry& registry) {
		registry.Add("Editor.HeadlessExportBaseline.UsesDisposableAuthoredInputCopy", [](TestContext& context) {
			const std::string helper = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "RunBaseline.ps1");
			ExpectContains(context, helper, "out/ci/export-runtime-baseline", "Helper should default to a disposable out/ workspace.");
			ExpectContains(context, helper, "Assert-PathIsBelow", "Helper should guard its recursive cleanup boundary.");
			ExpectContains(context, helper, "foreach ($directoryName in @(\"Assets\", \"BuildProfiles\", \"ProjectSettings\"))", "Helper should copy project configuration and the authored Assets root.");
			ExpectContains(context, helper, "foreach ($directoryName in @(\"Scenes\", \"Scripts\", \"UI\"))", "Helper should stage packageable source folders beneath the disposable Assets root.");
			context.Expect(helper.find("Copy-Item -LiteralPath $fixtureSource -Recurse") == std::string::npos, "Helper must not copy ignored Build/obj state that happens to be beside a local fixture.");
		});

		registry.Add("Editor.HeadlessExportBaseline.UsesInstalledCompilersAndManagedReferences", [](TestContext& context) {
			const std::string helper = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "RunBaseline.ps1");
			const std::vector<std::string> requiredTokens = {
				"InstallRoot",
				"Join-Path $resolvedInstallRoot \"Editor\"",
				"TerraEngineModelPackageCompiler",
				"TerraEngine-Export",
				"Managed/TerraEngine.CSharp.dll",
				"Managed/Analyzers/TerraEngine.Export.Generator.dll",
				"Build/Assembly-CSharp/References",
				"--source-relative",
				"--package-guid",
				"--shader-guid"
			};
			for (const std::string& token : requiredTokens) {
				context.Expect(helper.find(token) != std::string::npos, "Baseline helper is missing an installed compiler or managed-reference contract token.");
			}
		});

		registry.Add("Editor.HeadlessExportBaseline.ValidatesGeneratedChildFormatsAndPlaceholders", [](TestContext& context) {
			const std::string helper = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "RunBaseline.ps1");
			ExpectContains(context, helper, "generatedMeshes", "Helper should validate generated mesh records from the compiler sidecar.");
			ExpectContains(context, helper, "generatedMaterials", "Helper should validate generated material records from the compiler sidecar.");
			ExpectContains(context, helper, "generatedTextures", "Helper should validate generated texture records from the compiler sidecar.");
			ExpectContains(context, helper, "Assert-CompiledAsset", "Helper should validate every generated compiled child.");
			ExpectContains(context, helper, "$bytes.Length -le 4", "Helper should reject empty or truncated generated children.");
			ExpectContains(context, helper, "0x54455446", "Helper should validate the binary TETF magic value.");
			ExpectContains(context, helper, "placeholder", "Helper should reject old placeholder payload markers.");
			ExpectContains(context, helper, "$LASTEXITCODE", "Native compiler and export failures should propagate to the caller.");
		});

		registry.Add("Editor.HeadlessExportBaseline.InspectsStableEvidenceBundle", [](TestContext& context) {
			const std::string helper = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "RunBaseline.ps1");
			const std::vector<std::string> evidenceFiles = {
				"export-result.json",
				"resolved-manifest.json",
				"content-inventory.json",
				"validation-report.json",
				"progress.jsonl",
				"package-validation.txt"
			};
			for (const std::string& evidenceFile : evidenceFiles) {
				context.Expect(helper.find(evidenceFile) != std::string::npos, "Baseline helper should require every evidence contract file.");
			}
			ExpectContains(context, helper, "placeholderEntries", "Helper should reject placeholder-marked inventory entries.");
			ExpectContains(context, helper, "$manifest.startupScene -ne $profile.startupScene", "Helper should compare manifest and standalone profile startup scenes.");
			ExpectContains(context, helper, "Manifest evidence match:", "Helper should require exact archived-manifest/evidence comparison from shared package inspection.");
			ExpectContains(context, helper, "baseline-prepare.log", "Helper should retain a preparation summary for CI artifacts.");
			ExpectContains(context, helper, "archive-inspection.log", "Helper should retain its final package/evidence inspection summary.");
		});

		registry.Add("Editor.HeadlessExportBaseline.FailsClosedOnManagedWorkspaceLeaks", [](TestContext& context) {
			const std::string helper = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "RunBaseline.ps1");
			ExpectContains(context, helper, "Build/Assembly-CSharp/Assembly-CSharp.dll", "The installed editor lane must prove its ordinary managed build at the disposable project path.");
			ExpectContains(context, helper, "Library/AssetIndex.json", "The installed editor lane must inspect durable script compilation state.");
			ExpectContains(context, helper, "compileStatus -ne \"Ready\"", "The baseline must reject an editor script build that did not reach Ready.");
			ExpectContains(context, helper, "Library/NativeAot", "The export lane must reject project-local NativeAOT workspace leakage.");
		});

		registry.Add("Editor.HeadlessExportBaseline.LegacyEntryPointForwardsToCanonicalTool", [](TestContext& context) {
			const std::string legacy = ReadText(RepositoryRoot() / "Tests" / "Tools" / "ExportRuntimeBaseline" / "ExportSample.ps1");
			ExpectContains(context, legacy, "Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1", "Historical helper should forward to the canonical installed-tree gate.");
			ExpectContains(context, legacy, "$Profile -ne \"baseline\"", "Historical profile parameter should fail explicitly for unsupported names.");
			context.Expect(legacy.find("Run the editor export command") == std::string::npos, "Historical helper must no longer be report-only.");
		});
	}
}
