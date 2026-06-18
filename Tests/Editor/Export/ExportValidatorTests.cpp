#include "TestHarness.h"

#include "ExportValidator.h"

namespace TerraEngine::EditorTests {
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
	}
}
