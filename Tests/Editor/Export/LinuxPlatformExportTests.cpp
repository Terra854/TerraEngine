#include "TestHarness.h"

#include "BuildProfile.h"
#include "ExportPackageValidator.h"
#include "TEPAK/PackWriter.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class LinuxExportWorkspace {
		public:
			LinuxExportWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "LinuxPlatformExport") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(InputRoot());
				std::filesystem::create_directories(OutputRoot());
			}

			~LinuxExportWorkspace() {
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

		void WriteRequiredEntries(const LinuxExportWorkspace& workspace) {
			const std::vector<std::string> requiredFiles = {
				"game.json",
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
				workspace.WriteText(file, file == "game.json" ? "{ \"name\": \"Linux Export\" }" : "payload");
			}
		}
	}

	void RegisterLinuxPlatformExportTests(TestRegistry& registry) {
		registry.Add("Editor.LinuxPlatformExport.UsesNativeArtifactNames", [](TestContext& context) {
			context.ExpectEqual(std::string("win-x64"), GetExportRuntimeIdentifier(ExportPlatform::Windows), "Windows profiles must keep publishing win-x64 managed payloads.");
			context.ExpectEqual(std::string("linux-x64"), GetExportRuntimeIdentifier(ExportPlatform::Linux), "Linux exports should publish linux-x64 managed payloads.");
			context.ExpectEqual(std::string(""), GetExportExecutableSuffix(ExportPlatform::Linux), "Linux launchers should not use .exe.");
			context.ExpectEqual(std::string(".so"), GetExportSharedLibrarySuffix(ExportPlatform::Linux), "Linux native payloads should use .so.");
			context.ExpectEqual(std::string("Game"), GetExportLauncherFileName("Game", ExportPlatform::Linux), "Linux launcher filenames should be extensionless.");
			context.ExpectEqual(std::string("Assembly-CSharp.so"), GetExportManagedRuntimeLibraryName(ExportPlatform::Linux), "Linux NativeAOT payload should be a shared object.");
			context.ExpectEqual(std::string("libTerraEngine.so"), GetExportEngineLibraryName(ExportPlatform::Linux), "Linux engine runtime should use the lib prefix and .so suffix.");
		});

		registry.Add("Editor.LinuxPlatformExport.ValidatesPlatformNeutralPackage", [](TestContext& context) {
			LinuxExportWorkspace workspace;
			WriteRequiredEntries(workspace);
			workspace.CreatePack();

			std::string error;
			const bool valid = ValidateExportPackage(workspace.PackPath(), ExportPlatform::Linux, error);

			context.Expect(valid, error.empty() ? "Linux package should satisfy platform-neutral TEPAK contract." : error);
			context.Expect(error.empty(), "Successful Linux package validation should not leave an error string.");
		});
	}
}
