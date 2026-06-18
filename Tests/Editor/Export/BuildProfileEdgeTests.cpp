#include "TestHarness.h"

#include "BuildProfile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace TerraEngine::EditorTests {
	namespace {
		class TempBuildProfileEdgeDirectory {
		public:
			TempBuildProfileEdgeDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "BuildProfileEdges") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempBuildProfileEdgeDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

			[[nodiscard]] std::filesystem::path WriteTextFile(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = root_ / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath);
				stream << contents;
				return fullPath;
			}

		private:
			std::filesystem::path root_;
		};

		bool IsLowerHexString(const std::string& text) {
			return std::all_of(text.begin(), text.end(), [](char value) {
				return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
			});
		}
	}

	void RegisterBuildProfileEdgeTests(TestRegistry& registry) {
		registry.Add("Editor.BuildProfile.LoadMalformedProfileReturnsDefaultProfile", [](TestContext& context) {
			TempBuildProfileEdgeDirectory tempDirectory;
			const std::filesystem::path profilePath = tempDirectory.WriteTextFile("malformed.json", "{ this is not valid json");

			const BuildProfile loaded = LoadBuildProfile(profilePath);

			context.ExpectEqual(std::string("Default"), loaded.name, "Malformed profile JSON should return a default profile name.");
			context.Expect(loaded.id.empty(), "Malformed profile JSON should return a profile without an id.");
			context.Expect(loaded.filePath.empty(), "Malformed profile JSON should not claim a source file path was loaded.");
			context.Expect(loaded.platform == ExportPlatform::Windows, "Malformed profile JSON should preserve the default platform.");
			context.Expect(loaded.compressionPreset == CompressionPreset::Normal, "Malformed profile JSON should preserve the default compression preset.");
		});

		registry.Add("Editor.BuildProfile.LoadPartialProfileKeepsMissingDefaults", [](TestContext& context) {
			TempBuildProfileEdgeDirectory tempDirectory;
			const std::filesystem::path profilePath = tempDirectory.WriteTextFile("partial.json",
R"({
	"name": "Partial",
	"id": "partial01",
	"startupScene": "Scenes/Partial.tescene"
})");

			const BuildProfile loaded = LoadBuildProfile(profilePath);

			context.ExpectEqual(std::string("Partial"), loaded.name, "Partial profile names should load from JSON.");
			context.ExpectEqual(std::string("partial01"), loaded.id, "Partial profile ids should load from JSON.");
			context.ExpectEqual(std::string("Scenes/Partial.tescene"), loaded.startupScene, "Partial profile startup scenes should load from JSON.");
			context.Expect(loaded.platform == ExportPlatform::Windows, "Missing platform should keep the default Windows value.");
			context.Expect(loaded.compressionPreset == CompressionPreset::Normal, "Missing compression preset should keep the default normal value.");
			context.Expect(loaded.outputDirectory.empty(), "Missing output directory should keep the default empty path.");
			context.Expect(loaded.enableManagedCompilation, "Missing managed compilation flag should keep the default enabled value.");
			context.Expect(!loaded.includeDebugSymbols, "Missing debug symbol flag should keep the default disabled value.");
			context.ExpectEqual(profilePath.string(), loaded.filePath.string(), "Successfully loaded partial profiles should remember their source path.");
		});

		registry.Add("Editor.BuildProfile.UnknownEnumStringsFallbackSafely", [](TestContext& context) {
			TempBuildProfileEdgeDirectory tempDirectory;
			const std::filesystem::path profilePath = tempDirectory.WriteTextFile("unknown-enums.json",
R"({
	"name": "Unknown Enums",
	"id": "unknown01",
	"platform": "macos",
	"compressionPreset": "extreme",
	"startupScene": "Scenes/Unknown.tescene"
})");

			const BuildProfile loaded = LoadBuildProfile(profilePath);

			context.Expect(loaded.platform == ExportPlatform::Windows, "Unknown platform strings should fall back to Windows.");
			context.Expect(loaded.compressionPreset == CompressionPreset::Normal, "Unknown compression preset strings should fall back to normal.");
			context.ExpectEqual(std::string("unknown01"), loaded.id, "Unknown enum values should not discard the rest of a valid profile.");
		});

		registry.Add("Editor.BuildProfile.SaveCreatesNestedParentDirectories", [](TestContext& context) {
			TempBuildProfileEdgeDirectory tempDirectory;
			const std::filesystem::path profilePath = tempDirectory.Root() / "Nested" / "Profiles" / "saved.json";

			BuildProfile profile{};
			profile.name = "Nested Save";
			profile.id = "nestedsave";
			profile.startupScene = "Scenes/Nested.tescene";
			profile.enableManagedCompilation = false;

			context.Expect(SaveBuildProfile(profile, profilePath), "Saving a profile should create missing parent directories.");
			context.Expect(std::filesystem::exists(profilePath), "The profile file should exist after saving into nested directories.");
			const BuildProfile loaded = LoadBuildProfile(profilePath);
			context.ExpectEqual(profile.id, loaded.id, "Profiles saved into nested directories should load normally.");
		});

		registry.Add("Editor.BuildProfile.GenerateIdReturnsEightLowerHexCharacters", [](TestContext& context) {
			const std::string id = GenerateBuildProfileId();

			context.ExpectEqual(8u, static_cast<unsigned int>(id.size()), "Generated build profile ids should be eight characters long.");
			context.Expect(IsLowerHexString(id), "Generated build profile ids should use lowercase hexadecimal characters.");
		});
	}
}
