#include "TestHarness.h"

#include "ExportPackageValidator.h"
#include "TEPAK/PackWriter.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempExportPackageWorkspace {
		public:
			TempExportPackageWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ExportPackageValidator") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(InputRoot());
				std::filesystem::create_directories(OutputRoot());
			}

			~TempExportPackageWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] std::filesystem::path InputRoot() const {
				return root_ / "Input";
			}

			[[nodiscard]] std::filesystem::path OutputRoot() const {
				return root_ / "Output";
			}

			[[nodiscard]] std::filesystem::path PackPath() const {
				return OutputRoot() / "Game.tepak";
			}

			void WriteText(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = InputRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream << contents;
			}

			void CreatePack() const {
				tepak::PackagingRequest request;
				request.inputRoot = InputRoot();
				request.outputPackPath = PackPath();
				request.overwriteExisting = true;
				request.enableCompression = false;
				request.enableDictionaryTraining = false;
				(void)tepak::PackWriter::Create(request);
			}

		private:
			std::filesystem::path root_;
		};

		void WriteRequiredExportRuntimeEntries(const TempExportPackageWorkspace& workspace) {
			const std::vector<std::string> requiredFiles = {
				"Fonts/Roboto-Medium.ttf",
				"Shaders/Default.task.tspf",
				"Shaders/Default.mesh.tspf",
				"Shaders/Default.frag.tspf",
				"Shaders/Editor/SceneViewGrid.vert.spv",
				"Shaders/Editor/SceneViewGrid.frag.spv",
				"Shaders/Sky/SkyBackground.task.tspf",
				"Shaders/Sky/SkyBackground.mesh.tspf",
				"Shaders/Sky/SkyBackground.frag.tspf",
				"Shaders/UI/UI_Instanced.vert.spv",
				"Shaders/UI/UI_Instanced.frag.spv",
				"Shaders/UI/UI_Static.vert.spv",
				"Shaders/UI/UI_Static.frag.spv",
				"Shaders/UI/UI_SDF_Text.vert.spv",
				"Shaders/UI/UI_SDF_Text.frag.spv",
				"Shaders/Lighting/ClusterDepthMask.comp.spv",
				"Shaders/Lighting/ClusterGrid.comp.spv",
				"Shaders/Lighting/ClusterLightCount.comp.spv",
				"Shaders/Lighting/ClusterLightFill.comp.spv",
				"Shaders/Lighting/ClusterLightPrefix.comp.spv",
				"Shaders/Lighting/LightCull.comp.spv",
				"Shaders/PostProcess/HiZGenerate.comp.spv",
				"Shaders/PostProcess/VRSGenerate.comp.spv",
				"Shaders/PostProcess/GTAO.comp.spv",
				"Shaders/PostProcess/GTAOBlur.comp.spv",
			};

			for (const std::string& file : requiredFiles) {
				workspace.WriteText(file, "required runtime payload");
			}
		}

		void WriteManifestAndStartupScene(const TempExportPackageWorkspace& workspace) {
			workspace.WriteText("Scenes/Main.tescene", "{\n\t\"name\": \"Startup\"\n}\n");
			workspace.WriteText(
				"game.json",
				"{\n"
				"\t\"name\": \"Validator Test\",\n"
				"\t\"startupScene\": \"Scenes/Main.tescene\",\n"
				"\t\"assets\": [\n"
				"\t\t{ \"guid\": \"00000000000000000000000000000001\", \"path\": \"Scenes/Main.tescene\", \"type\": 0 }\n"
				"\t]\n"
				"}\n");
		}
	}

	void RegisterExportPackageValidatorTests(TestRegistry& registry) {
		registry.Add("Editor.ExportPackageValidator.AcceptsCompleteRuntimePackage", [](TestContext& context) {
			TempExportPackageWorkspace workspace;
			WriteRequiredExportRuntimeEntries(workspace);
			WriteManifestAndStartupScene(workspace);
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), error);

			context.Expect(valid, error.empty() ? "A package with required runtime entries should validate." : error);
			context.Expect(error.empty(), "Successful package validation should not leave an error string.");
		});

		registry.Add("Editor.ExportPackageValidator.AcceptsWindowsRuntimePackageWithPlatformOverload", [](TestContext& context) {
			TempExportPackageWorkspace workspace;
			WriteRequiredExportRuntimeEntries(workspace);
			WriteManifestAndStartupScene(workspace);
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Windows, error);

			context.Expect(valid, error.empty() ? "Windows package validation should keep the original package contract." : error);
			context.Expect(error.empty(), "Successful Windows package validation should not leave an error string.");
		});

		registry.Add("Editor.ExportPackageValidator.RejectsMissingRuntimeFont", [](TestContext& context) {
			TempExportPackageWorkspace workspace;
			WriteRequiredExportRuntimeEntries(workspace);
			WriteManifestAndStartupScene(workspace);
			std::filesystem::remove(workspace.InputRoot() / "Fonts" / "Roboto-Medium.ttf");
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), error);

			context.Expect(!valid, "A package missing the required runtime font should fail validation.");
			context.Expect(error.find("fonts/roboto-medium.ttf") != std::string::npos, "The validation error should name the missing font path.");
		});
	}
}
