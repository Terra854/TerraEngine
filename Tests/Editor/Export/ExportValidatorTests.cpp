#include "TestHarness.h"

#include "ExportValidator.h"

#include <algorithm>
#include <string>
#include <filesystem>
#include <fstream>
#include <array>

namespace TerraEngine::EditorTests {
	namespace {
		bool HasProfileDiagnosticContaining(const ExportValidationResult& result, const std::string& text) {
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&text](const ExportDiagnostic& diagnostic) {
				return diagnostic.category == "Profile" && diagnostic.message.find(text) != std::string::npos;
			});
		}

		class TempProjectWorkspace {
		public:
			TempProjectWorkspace(const std::string& name)
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / name) {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(AssetsRoot());
				std::filesystem::create_directories(BuildAssetsRoot());
			}

			~TempProjectWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path Root() const {
				return root_;
			}

			[[nodiscard]] std::filesystem::path AssetsRoot() const {
				return root_ / "Assets";
			}

			[[nodiscard]] std::filesystem::path BuildAssetsRoot() const {
				return root_ / "Build" / "Assets";
			}

			void WriteFile(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = root_ / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream << contents;
			}

		private:
			std::filesystem::path root_;
		};
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

		registry.Add("Editor.ExportValidator.DetectsMissingCompiledAssetOutput", [](TestContext& context) {
			TempProjectWorkspace workspace("DetectsMissingCompiledAssetOutput");
			BuildProfile profile{};
			profile.name = "Test";
			profile.id = "test";
			profile.outputDirectory = "Builds/Test";
			profile.startupScene = "Scenes/Test.tescene";
			profile.enableManagedCompilation = false;

			workspace.WriteFile("Assets/Textures/stone.png", "dummy-pixels");
			workspace.WriteFile("Assets/Textures/stone.png.meta", R"({
				"guid": "00000000-0000-0000-0000-000000000001",
				"importerName": "TextureImporter",
				"lastModified": 0,
				"sourcePath": "Textures/stone.png",
				"compiledPath": "Textures/stone.tetf"
			})");

			const ExportValidator validator;
			ExportValidationResult result = validator.ValidateAll(profile, workspace.Root());

			context.Expect(!result.passed, "Validation should fail when compiled asset output is missing.");
			
			bool sawMissingAsset = false;
			for (const auto& diagnostic : result.diagnostics) {
				if (diagnostic.category == "Assets" && diagnostic.message.find("Missing compiled asset output: Textures/stone.tetf") != std::string::npos) {
					sawMissingAsset = true;
					break;
				}
			}
			context.Expect(sawMissingAsset, "Should report missing compiled asset output diagnostic.");

			// Write the compiled file to satisfy the check
			workspace.WriteFile("Build/Assets/Textures/stone.tetf", "dummy-compiled");
			result = validator.ValidateAll(profile, workspace.Root());
			context.Expect(result.passed, "Validation should pass once the compiled asset output is written.");
		});

		registry.Add("Editor.ExportValidator.DetectsMetaPathMismatches", [](TestContext& context) {
			TempProjectWorkspace workspace("DetectsMetaPathMismatches");
			BuildProfile profile{};
			profile.name = "Test";
			profile.id = "test";
			profile.outputDirectory = "Builds/Test";
			profile.startupScene = "Scenes/Test.tescene";
			profile.enableManagedCompilation = false;

			// Write mismatch sourcePath
			workspace.WriteFile("Assets/Textures/stone.png", "dummy-pixels");
			workspace.WriteFile("Assets/Textures/stone.png.meta", R"({
				"guid": "00000000-0000-0000-0000-000000000001",
				"importerName": "TextureImporter",
				"lastModified": 0,
				"sourcePath": "Textures/stone_wrong.png",
				"compiledPath": "Textures/stone.tetf"
			})");
			workspace.WriteFile("Build/Assets/Textures/stone.tetf", "dummy-compiled");

			const ExportValidator validator;
			ExportValidationResult result = validator.ValidateAll(profile, workspace.Root());

			context.Expect(!result.passed, "Validation should fail on source path mismatch.");
			bool sawSourceMismatch = false;
			for (const auto& diagnostic : result.diagnostics) {
				if (diagnostic.category == "Assets" && diagnostic.message.find("Source path mismatch in metadata") != std::string::npos) {
					sawSourceMismatch = true;
					break;
				}
			}
			context.Expect(sawSourceMismatch, "Should report source path mismatch error.");

			// Write mismatch compiledPath
			workspace.WriteFile("Assets/Textures/stone.png.meta", R"({
				"guid": "00000000-0000-0000-0000-000000000001",
				"importerName": "TextureImporter",
				"lastModified": 0,
				"sourcePath": "Textures/stone.png",
				"compiledPath": "Textures/stone_wrong.tetf"
			})");
			result = validator.ValidateAll(profile, workspace.Root());

			context.Expect(!result.passed, "Validation should fail on compiled path mismatch.");
			bool sawCompiledMismatch = false;
			for (const auto& diagnostic : result.diagnostics) {
				if (diagnostic.category == "Assets" && diagnostic.message.find("Compiled path mismatch in metadata") != std::string::npos) {
					sawCompiledMismatch = true;
					break;
				}
			}
			context.Expect(sawCompiledMismatch, "Should report compiled path mismatch error.");
		});

		registry.Add("Editor.ExportValidator.DetectsMissingCompiledShaderOutput", [](TestContext& context) {
			TempProjectWorkspace workspace("DetectsMissingCompiledShaderOutput");
			BuildProfile profile{};
			profile.name = "Test";
			profile.id = "test";
			profile.outputDirectory = "Builds/Test";
			profile.startupScene = "Scenes/Test.tescene";
			profile.enableManagedCompilation = false;

			// Write a Shaders directory under the project workspace root with one missing required shader
			workspace.WriteFile("Shaders/default.mesh.tspf", "dummy-shader");

			const ExportValidator validator;
			ExportValidationResult result = validator.ValidateAll(profile, workspace.Root());

			context.Expect(!result.passed, "Validation should fail when a required compiled shader is missing.");
			bool sawMissingShader = false;
			for (const auto& diagnostic : result.diagnostics) {
				if (diagnostic.category == "Shaders" && diagnostic.message.find("Missing compiled shader output: default.task.tspf") != std::string::npos) {
					sawMissingShader = true;
					break;
				}
			}
			context.Expect(sawMissingShader, "Should report missing compiled shader output error.");

			// Populate every shader required by the packaged runtime, including the
			// conventional-visibility fallback and terrain rendering pipeline.
			static constexpr std::array<std::string_view, 29> kRequiredShaders = {
				"default.task.tspf",
				"default.mesh.tspf",
				"default.frag.tspf",
				"default.conventionalvisibilityidentity.frag.spv",
				"editor/sceneviewgrid.vert.spv",
				"editor/sceneviewgrid.frag.spv",
				"sky/skybackground.task.tspf",
				"sky/skybackground.mesh.tspf",
				"sky/skybackground.frag.tspf",
				"terrain/terrainvisibility.task.tspf",
				"terrain/terrainvisibility.mesh.tspf",
				"terrain/terrainvisibility.frag.tspf",
				"terrain/terrainsurface.comp.tspf",
				"ui/ui_instanced.vert.spv",
				"ui/ui_instanced.frag.spv",
				"ui/ui_static.vert.spv",
				"ui/ui_static.frag.spv",
				"ui/ui_sdf_text.vert.spv",
				"ui/ui_sdf_text.frag.spv",
				"lighting/clusterdepthmask.comp.spv",
				"lighting/clustergrid.comp.spv",
				"lighting/clusterlightcount.comp.spv",
				"lighting/clusterlightfill.comp.spv",
				"lighting/clusterlightprefix.comp.spv",
				"lighting/lightcull.comp.spv",
				"postprocess/hizgenerate.comp.spv",
				"postprocess/vrsgenerate.comp.spv",
				"postprocess/gtao.comp.spv",
				"postprocess/gtaoblur.comp.spv"
			};

			for (std::string_view shader : kRequiredShaders) {
				workspace.WriteFile(std::filesystem::path("Shaders") / shader, "dummy-shader");
			}

			result = validator.ValidateAll(profile, workspace.Root());
			context.Expect(result.passed, "Validation should pass once all required shaders are present.");
		});
	}
}
