#include "TestHarness.h"

#include "BuildProfile.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace TerraEngine::EditorTests {
	namespace {
		class TempProfileCollectionDirectory {
		public:
			TempProfileCollectionDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "BuildProfileCollection") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempProfileCollectionDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

		private:
			std::filesystem::path root_;
		};

		BuildProfile MakeProfile(std::string name, std::string id) {
			BuildProfile profile{};
			profile.name = std::move(name);
			profile.id = std::move(id);
			profile.outputDirectory = "Builds/Test";
			profile.startupScene = "Scenes/Test.tescene";
			profile.enableManagedCompilation = false;
			return profile;
		}

		bool HasProfileNamed(const std::vector<BuildProfile>& profiles, const std::string& name) {
			return std::any_of(profiles.begin(), profiles.end(), [&name](const BuildProfile& profile) {
				return profile.name == name;
			});
		}
	}

	void RegisterBuildProfileCollectionTests(TestRegistry& registry) {
		registry.Add("Editor.BuildProfile.LoadAllReturnsOnlyValidJsonProfiles", [](TestContext& context) {
			TempProfileCollectionDirectory tempDirectory;

			context.Expect(SaveBuildProfile(MakeProfile("Windows Debug", "windowsdebug"), tempDirectory.Root() / "windows.json"), "The first valid profile should be written for collection loading.");
			context.Expect(SaveBuildProfile(MakeProfile("Linux Release", "linuxrelease"), tempDirectory.Root() / "linux.json"), "The second valid profile should be written for collection loading.");

			std::ofstream(tempDirectory.Root() / "malformed.json") << "{ this is not json";
			std::ofstream(tempDirectory.Root() / "notes.txt") << "not a profile";

			const std::vector<BuildProfile> profiles = LoadAllBuildProfiles(tempDirectory.Root());

			context.ExpectEqual(2u, static_cast<unsigned int>(profiles.size()), "Only valid JSON profiles with non-empty ids should be returned.");
			context.Expect(HasProfileNamed(profiles, "Windows Debug"), "The Windows profile should be present in the loaded collection.");
			context.Expect(HasProfileNamed(profiles, "Linux Release"), "The Linux profile should be present in the loaded collection.");
		});

		registry.Add("Editor.BuildProfile.LoadAllMissingDirectoryReturnsEmptyCollection", [](TestContext& context) {
			TempProfileCollectionDirectory tempDirectory;
			const std::filesystem::path missingDirectory = tempDirectory.Root() / "Missing";

			const std::vector<BuildProfile> profiles = LoadAllBuildProfiles(missingDirectory);

			context.Expect(profiles.empty(), "Loading profiles from a missing directory should return an empty collection.");
		});

		registry.Add("Editor.BuildProfile.StandaloneProfileIsSharedSourceOfTruth", [](TestContext& context) {
			TempProfileCollectionDirectory tempDirectory;
			const std::filesystem::path profilePath = tempDirectory.Root() / "shared-headless.json";
			BuildProfile source = MakeProfile("Shared Editor And Headless", "sharedprofile");
			source.platform = GetHostExportPlatform();
			source.includedScenes = {"Scenes/Test.tescene", "Scenes/Secondary.tescene"};
			context.Expect(SaveBuildProfile(source, profilePath), "The standalone profile should be written once for both entry points.");

			const std::vector<BuildProfile> editorProfiles = LoadAllBuildProfiles(tempDirectory.Root());
			const BuildProfile headlessProfile = LoadBuildProfile(profilePath);

			context.ExpectEqual(1u, static_cast<unsigned int>(editorProfiles.size()), "Editor collection discovery should find the standalone profile exactly once.");
			context.ExpectEqual(headlessProfile.id, editorProfiles.front().id, "Editor and headless loading should retain the same profile id.");
			context.ExpectEqual(headlessProfile.outputDirectory.string(), editorProfiles.front().outputDirectory.string(), "Editor and headless loading should retain the same stored output value.");
			context.ExpectEqual(headlessProfile.startupScene, editorProfiles.front().startupScene, "Editor and headless loading should retain the same startup scene.");
			context.Expect(headlessProfile.includedScenes == editorProfiles.front().includedScenes, "Editor and headless loading should retain the same included-scene order.");
			context.ExpectEqual(profilePath.string(), editorProfiles.front().filePath.string(), "Collection entries should remember the same standalone source file used by automation.");
		});
	}
}
