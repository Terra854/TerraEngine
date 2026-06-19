#include "TestHarness.h"

#include "BuildProfile.h"

#include <filesystem>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		std::filesystem::path MakeTempProfilePath() {
			return std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "BuildProfiles" / "round-trip-profile.json";
		}

		class TempProfileDirectory {
		public:
			TempProfileDirectory()
				: root_(MakeTempProfilePath().parent_path().parent_path()) {
				std::filesystem::remove_all(root_);
			}

			~TempProfileDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

		private:
			std::filesystem::path root_;
		};
	}

	void RegisterBuildProfileRoundTripTests(TestRegistry& registry) {
		registry.Add("Editor.BuildProfile.SaveLoadRoundTripPreservesFields", [](TestContext& context) {
			TempProfileDirectory tempDirectory;
			const std::filesystem::path profilePath = MakeTempProfilePath();

			BuildProfile profile{};
			profile.name = "Automated Test Profile";
			profile.id = "testprofile01";
			profile.platform = ExportPlatform::Linux;
			profile.outputDirectory = "Builds/AutomatedTest";
			profile.startupScene = "Scenes/TestScene.tescene";
			profile.compressionPreset = CompressionPreset::Best;
			profile.tepakWorkerCount = 4;
			profile.enableManagedCompilation = false;
			profile.includeDebugSymbols = true;
			profile.stripNativeAotOutput = true;
			profile.additionalPublishArgs = "--self-contained true";
			profile.includedScenes = {"Scenes/TestScene.tescene", "Scenes/Secondary.tescene"};

			context.Expect(SaveBuildProfile(profile, profilePath), "Saving a complete build profile should succeed.");

			const BuildProfile loaded = LoadBuildProfile(profilePath);
			context.ExpectEqual(profile.name, loaded.name, "The profile name should round-trip through JSON.");
			context.ExpectEqual(profile.id, loaded.id, "The profile id should round-trip through JSON.");
			context.Expect(loaded.platform == ExportPlatform::Linux, "The selected platform should round-trip through JSON.");
			context.ExpectEqual(profile.outputDirectory, loaded.outputDirectory, "The output directory should round-trip through JSON.");
			context.ExpectEqual(profile.startupScene, loaded.startupScene, "The startup scene should round-trip through JSON.");
			context.Expect(loaded.compressionPreset == CompressionPreset::Best, "The compression preset should round-trip through JSON.");
			context.ExpectEqual(profile.tepakWorkerCount, loaded.tepakWorkerCount, "The worker count should round-trip through JSON.");
			context.ExpectEqual(profile.additionalPublishArgs, loaded.additionalPublishArgs, "Additional publish arguments should round-trip through JSON.");
			context.ExpectEqual(profile.includedScenes.size(), loaded.includedScenes.size(), "All included scenes should round-trip through JSON.");
			context.ExpectEqual(profilePath.string(), loaded.filePath.string(), "The loaded profile should remember its source file path.");
		});

		registry.Add("Editor.BuildProfile.WindowsArtifactNamesRemainCompatible", [](TestContext& context) {
			// These names protect the original Windows export contract while the
			// platform helpers add Linux-specific runtime naming.
			context.ExpectEqual(std::string("win-x64"), GetExportRuntimeIdentifier(ExportPlatform::Windows), "Windows exports should keep publishing win-x64 managed payloads.");
			context.ExpectEqual(std::string(".exe"), GetExportExecutableSuffix(ExportPlatform::Windows), "Windows launcher filenames should keep the .exe suffix.");
			context.ExpectEqual(std::string(".dll"), GetExportSharedLibrarySuffix(ExportPlatform::Windows), "Windows native payloads should keep the .dll suffix.");
			context.ExpectEqual(std::string("Game.exe"), GetExportLauncherFileName("Game", ExportPlatform::Windows), "Windows launcher staging should keep the executable extension.");
			context.ExpectEqual(std::string("Assembly-CSharp.dll"), GetExportManagedRuntimeLibraryName(ExportPlatform::Windows), "Windows NativeAOT payload should keep the DLL name.");
			context.ExpectEqual(std::string("TerraEngine.dll"), GetExportEngineLibraryName(ExportPlatform::Windows), "Windows engine runtime should keep the original DLL name.");
			context.ExpectEqual(std::string("glfw3.dll"), GetExportGlfwLibraryName(ExportPlatform::Windows), "Windows GLFW runtime should keep the original DLL name.");
		});
	}
}
