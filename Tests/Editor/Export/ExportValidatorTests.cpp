#include "TestHarness.h"

#include "ExportValidator.h"

#include <algorithm>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		bool HasProfileDiagnosticContaining(const ExportValidationResult& result, const std::string& text) {
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&text](const ExportDiagnostic& diagnostic) {
				return diagnostic.category == "Profile" && diagnostic.message.find(text) != std::string::npos;
			});
		}
	}

	void RegisterExportValidatorTests(TestRegistry& registry) {
		registry.Add("Editor.ExportValidator.InvalidProfileReportsRequiredFields", [](TestContext& context) {
			BuildProfile profile{};
			profile.name.clear();
			profile.id.clear();
			profile.outputDirectory.clear();
			profile.startupScene.clear();
			profile.enableManagedCompilation = false;

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateProfile(profile);

			context.Expect(!result.passed, "A profile missing required fields should fail validation.");
			context.ExpectEqual(3u, result.errorCount, "Missing name, id, and startup scene should produce three errors.");
			context.ExpectEqual(1u, result.warningCount, "Missing output directory should produce one warning.");
			context.Expect(result.diagnostics.size() >= 4u, "Every required-field diagnostic should be preserved alongside informational render diagnostics.");
		});

		registry.Add("Editor.ExportValidator.MinimalProfilePassesWithoutManagedCompilation", [](TestContext& context) {
			BuildProfile profile{};
			profile.name = "Minimal";
			profile.id = "minimal";
			profile.outputDirectory = "Builds/Minimal";
			profile.startupScene = "Scenes/Minimal.tescene";
			profile.enableManagedCompilation = false;

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateAll(profile, "MissingProjectRootIsIgnoredWhenManagedCompilationIsDisabled");

			context.Expect(result.passed, "A complete profile should pass when managed source validation is disabled.");
			context.ExpectEqual(0u, result.errorCount, "A complete profile should not produce validation errors.");
			context.ExpectEqual(0u, result.warningCount, "A complete profile should not produce validation warnings.");
		});

		registry.Add("Editor.ExportValidator.LinuxProfileReportsRuntimeArtifactExpectations", [](TestContext& context) {
			BuildProfile profile{};
			profile.name = "Linux";
			profile.id = "linux";
			profile.platform = ExportPlatform::Linux;
			profile.outputDirectory = "Builds/Linux";
			profile.startupScene = "Scenes/Linux.tescene";
			profile.enableManagedCompilation = false;

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateProfile(profile);

			context.Expect(result.passed, "A complete Linux profile should pass validation.");
			context.Expect(HasProfileDiagnosticContaining(result, "Managed/Assembly-CSharp.so"), "Linux diagnostics should name the managed runtime shared object.");
			context.Expect(HasProfileDiagnosticContaining(result, "libTerraEngine.so"), "Linux diagnostics should name the engine shared object.");
			context.Expect(HasProfileDiagnosticContaining(result, "display server"), "Linux diagnostics should mention display startup requirements.");
			context.Expect(HasProfileDiagnosticContaining(result, "Vulkan-capable GPU"), "Linux diagnostics should mention GPU startup requirements.");
		});

		registry.Add("Editor.ExportValidator.BlocksCrossOsNativeAotTargets", [](TestContext& context) {
			BuildProfile profile{};
			profile.name = "Cross OS";
			profile.id = "crossos";
			profile.platform = GetHostExportPlatform() == ExportPlatform::Linux ? ExportPlatform::Windows : ExportPlatform::Linux;
			profile.outputDirectory = "Builds/CrossOs";
			profile.startupScene = "Scenes/CrossOs.tescene";
			profile.enableManagedCompilation = true;

			const ExportValidator validator;
			const ExportValidationResult result = validator.ValidateProfile(profile);

			context.Expect(!result.passed, "Cross-OS NativeAOT profiles should fail validation before dotnet publish runs.");
			context.Expect(HasProfileDiagnosticContaining(result, GetExportRuntimeIdentifier(profile.platform)), "The diagnostic should name the selected target runtime identifier.");
			context.Expect(HasProfileDiagnosticContaining(result, GetExportRuntimeIdentifier(GetHostExportPlatform())), "The diagnostic should name the host runtime identifier.");
		});
	}
}
