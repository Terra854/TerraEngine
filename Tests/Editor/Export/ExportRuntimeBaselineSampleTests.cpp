#include "TestHarness.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		std::filesystem::path SampleRoot() {
			return (std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "Fixtures" / "ExportRuntimeBaseline").lexically_normal();
		}

		std::string ReadText(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			return buffer.str();
		}

		bool IsCompiledBaselineExtension(const std::filesystem::path& path) {
			const std::string extension = path.extension().string();
			return extension == ".temf" || extension == ".tmat" || extension == ".tetf";
		}
	}

	void RegisterExportRuntimeBaselineSampleTests(TestRegistry& registry) {
		registry.Add("Editor.ExportRuntimeBaseline.UsesStandaloneProfileAndManagedProject", [](TestContext& context) {
			const std::filesystem::path root = SampleRoot();
			const nlohmann::json project = nlohmann::json::parse(ReadText(root / "game.json"));
			context.ExpectEqual(std::string("Export Runtime Baseline"), project.value("name", ""), "Sample project should have stable product metadata.");
			context.ExpectEqual(std::string("Scenes/Startup.tescene"), project.value("startupScene", ""), "Sample project should identify its startup scene.");
			context.Expect(!project.contains("buildProfiles"), "game.json must not duplicate the standalone build profile.");

			const std::filesystem::path profilePath = root / "BuildProfiles" / "baseline.json";
			context.Expect(std::filesystem::exists(profilePath), "Baseline sample should include its standalone editor/headless profile.");
			const nlohmann::json profile = nlohmann::json::parse(ReadText(profilePath));
			context.ExpectEqual(std::string("baseline"), profile.value("id", ""), "Standalone profile should keep the stable baseline id.");
			context.ExpectEqual(std::string("Scenes/Startup.tescene"), profile.value("startupScene", ""), "Standalone profile and project should select the same startup scene.");
			context.Expect(profile.value("enableManagedCompilation", false), "Baseline profile should exercise NativeAOT export.");
			context.Expect(profile.value("enableVulkanValidationLayers", false), "Baseline profile should make export validation explicit.");

			const std::string managedProject = ReadText(root / "Assembly-CSharp.csproj");
			context.Expect(managedProject.find("net10.0") != std::string::npos, "Baseline managed project should use the repository's .NET target.");
			context.Expect(managedProject.find("Build/Assembly-CSharp/References/TerraEngine.CSharp.dll") != std::string::npos, "Managed project should consume the helper-staged engine reference.");
			context.Expect(managedProject.find("TerraEngine.Export.Generator.dll") != std::string::npos, "Managed project should use the installed export source generator.");
			const std::string managedScript = ReadText(root / "Scripts" / "BaselineScript.cs");
			context.Expect(managedScript.find("partial class BaselineScript : Script") != std::string::npos, "Baseline script should be discoverable by the NativeAOT export generator.");
			context.Expect(managedScript.find("override void OnStart") != std::string::npos, "Baseline script should exercise the runtime lifecycle callback.");
		});

		registry.Add("Editor.ExportRuntimeBaseline.SourceRecipeEmbedsMeshMaterialAndTexture", [](TestContext& context) {
			const std::filesystem::path root = SampleRoot();
			const std::filesystem::path sourcePath = root / "Assets" / "Models" / "Baseline.gltf";
			const nlohmann::json source = nlohmann::json::parse(ReadText(sourcePath));
			context.ExpectEqual(std::string("2.0"), source["asset"].value("version", ""), "Baseline source should be a textual glTF 2.0 document.");
			context.ExpectEqual(1u, static_cast<unsigned int>(source.value("meshes", nlohmann::json::array()).size()), "Baseline source should contain one reviewable mesh.");
			context.ExpectEqual(1u, static_cast<unsigned int>(source.value("materials", nlohmann::json::array()).size()), "Baseline source should contain one material.");
			context.ExpectEqual(1u, static_cast<unsigned int>(source.value("images", nlohmann::json::array()).size()), "Baseline source should contain one embedded texture image.");
			context.Expect(source["accessors"][3].value("count", 0u) / 3u >= 512u, "Baseline mesh should contain enough source triangles to exercise the production virtual-geometry cook.");
			context.Expect(source["buffers"][0].value("uri", std::string{}).starts_with("data:application/octet-stream;base64,"), "Geometry should be embedded so clean runners need no external binary fixture.");
			context.Expect(source["images"][0].value("uri", std::string{}).starts_with("data:image/png;base64,"), "Texture bytes should be embedded in the textual source recipe.");

			const nlohmann::json metadata = nlohmann::json::parse(ReadText(sourcePath.string() + ".meta"));
			context.ExpectEqual(std::string("11111111222222223333333344444444"), metadata.value("guid", ""), "Model package should keep the stable recipe GUID used to derive child ids.");
			context.ExpectEqual(std::string("GltfPackageImporter"), metadata.value("importerName", ""), "Metadata should route the source through the production package importer.");
			context.ExpectEqual(std::string("Models/Baseline.gltf"), metadata.value("sourcePath", ""), "Metadata source path should be project-asset relative.");
		});

		registry.Add("Editor.ExportRuntimeBaseline.SceneUsesStableGeneratedChildren", [](TestContext& context) {
			const std::filesystem::path root = SampleRoot();
			const std::string sceneText = ReadText(root / "Scenes" / "Startup.tescene");

			context.Expect(sceneText.find("Models/Baseline_Generated/BaselineTriangle.temf") != std::string::npos, "Startup scene should reference the compiler-generated mesh path.");
			context.Expect(sceneText.find("c8ff317261139cf962f2cedc60952f64") != std::string::npos, "Startup scene should preserve the generated mesh GUID.");
			context.Expect(sceneText.find("Models/Baseline_Generated/BaselineMaterial.tmat") != std::string::npos, "Startup scene should reference the compiler-generated material path.");
			context.Expect(sceneText.find("5cb8d797dc72ae521a7dd2fa784b0f9f") != std::string::npos, "Startup scene should preserve the generated material GUID.");
			context.Expect(sceneText.find("Models/Baseline_Generated/BaselineTexture.tetf") != std::string::npos, "Startup scene should name the compiled texture used by the generated material.");
			context.Expect(sceneText.find("UI/Baseline.ui") != std::string::npos, "Startup scene should reference the sample UI document.");
			context.Expect(sceneText.find("BaselineScript") != std::string::npos, "Startup scene should reference the sample script.");
		});

		registry.Add("Editor.ExportRuntimeBaseline.SourceTreeRejectsCompiledPlaceholders", [](TestContext& context) {
			const std::filesystem::path assetsRoot = SampleRoot() / "Assets";
			std::size_t compiledFileCount = 0;
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(assetsRoot)) {
				if (!entry.is_regular_file()) {
					continue;
				}
				// Authored words are legal; only prebuilt compiled children violate this source-only fixture contract.
				if (IsCompiledBaselineExtension(entry.path())) {
					++compiledFileCount;
				}
			}
			context.ExpectEqual(0u, static_cast<unsigned int>(compiledFileCount), "Compiled TEMF/TMAT/TETF children must be generated only in disposable Build output.");
		});

		registry.Add("Editor.ExportRuntimeBaseline.RenderSettingsRemainExplicit", [](TestContext& context) {
			const std::filesystem::path renderSettingsPath = SampleRoot() / "ProjectSettings" / "RenderSettings.json";
			context.Expect(std::filesystem::exists(renderSettingsPath), "Baseline sample should keep render settings in ProjectSettings.");
			const nlohmann::json renderSettings = nlohmann::json::parse(ReadText(renderSettingsPath));
			context.Expect(renderSettings.contains("settings"), "Render settings should contain the renderer-owned settings object.");
			context.Expect(renderSettings.value("validationEnabled", false), "Sample render settings should enable validation.");
		});
	}
}
