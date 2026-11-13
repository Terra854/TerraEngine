#include "TestHarness.h"
#include "BuildProfile.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace TerraEngine::EditorTests {
	namespace {
		// Reserve an exclusive directory without ever deleting another test's
		// preexisting files. Cleanup remains active when a fatal assertion throws.
		class IdentityCollectionDirectory final {
		public:
			IdentityCollectionDirectory() {
				static std::atomic<unsigned long long> serial{0U};
				root_ = std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
				if (root_.filename().empty()) root_ = root_.parent_path();
				for (unsigned attempt = 0U; attempt < 4096U; ++attempt) {
					const auto candidate = root_ / ("TerraEngine_ProfileIdentity-" + std::to_string(serial.fetch_add(1U)));
					std::error_code error;
					if (std::filesystem::create_directory(candidate, error)) { path_ = candidate; return; }
					if (error && error != std::errc::file_exists) throw std::filesystem::filesystem_error("Cannot reserve profile fixture", candidate, error);
				}
				throw std::runtime_error("Profile fixture directory capacity exceeded.");
			}
			~IdentityCollectionDirectory() {
				if (!path_.empty() && path_.is_absolute() && path_.parent_path() == root_) {
					std::error_code ignored;
					std::filesystem::remove_all(path_, ignored);
				}
			}
			IdentityCollectionDirectory(const IdentityCollectionDirectory&) = delete;
			IdentityCollectionDirectory& operator=(const IdentityCollectionDirectory&) = delete;
			const std::filesystem::path& Path() const noexcept { return path_; }
		private:
			std::filesystem::path root_;
			std::filesystem::path path_;
		};

		BuildProfile IdentityProfile(std::string id, std::string name) {
			BuildProfile profile{};
			profile.id = std::move(id);
			profile.name = std::move(name);
			profile.outputDirectory = "Builds/Test";
			profile.startupScene = "Scenes/Test.tescene";
			profile.enableManagedCompilation = false;
			return profile;
		}

		std::string ReadIdentityFile(const std::filesystem::path& path) {
			std::ifstream stream(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
		}
	}

	void RegisterBuildProfileCollectionIdentityTests(TestRegistry& registry) {
		registry.Add("Editor.BuildProfile.SavePreservesLegacyFilenameIdentity", [](TestContext& context) {
			const IdentityCollectionDirectory directory;
			const auto legacy = directory.Path() / "legacy-name.json";
			context.Expect(SaveBuildProfile(IdentityProfile("stableid", "Before"), legacy), "The legacy-named profile must be persisted.");
			auto loaded = LoadBuildProfile(legacy);
			context.ExpectEqual(std::string("stableid"), loaded.id, "Loading must preserve authored identity.");
			context.Expect(loaded.filePath == legacy, "Loading must retain the original persistence location.");
			loaded.name = "After";
			const auto saved = SaveBuildProfileToCollection(directory.Path(), loaded);
			context.Expect(saved.succeeded && saved.profile.has_value(), "Saving an existing profile must succeed.");
			context.Expect(saved.profile->filePath == legacy, "Saving must reuse the collection-owned original file.");
			context.Expect(!std::filesystem::exists(directory.Path() / "stableid.json"), "Saving may not create a duplicate stable identity file.");
			const auto profiles = LoadAllBuildProfiles(directory.Path());
			context.ExpectEqual(std::size_t{1U}, profiles.size(), "The stable identity must appear exactly once after reopening.");
			context.ExpectEqual(std::string("After"), profiles.front().name, "The original file must contain the changed profile.");
		});

		registry.Add("Editor.BuildProfile.RemoveDoesNotTreatNameAsIdentity", [](TestContext& context) {
			const IdentityCollectionDirectory directory;
			const auto other = directory.Path() / "other.json";
			context.Expect(SaveBuildProfile(IdentityProfile("otherid", "targetid"), other), "The name-alias control must be persisted.");
			const auto before = ReadIdentityFile(other);
			context.Expect(!before.empty(), "The preservation oracle must read actual profile bytes.");
			// No targetid exists. This deterministically detects name-based deletion
			// without assuming the filesystem's directory enumeration order.
			const auto removed = RemoveBuildProfileFromCollection(directory.Path(), "targetid");
			context.Expect(!removed.succeeded, "An absent stable ID must not resolve through a different profile's name.");
			context.Expect(ReadIdentityFile(other) == before, "The alias profile must survive byte-for-byte.");
			const auto foundByName = FindBuildProfileInCollection(directory.Path(), "targetid");
			context.Expect(foundByName.succeeded && foundByName.profile && foundByName.profile->id == "otherid", "Interactive Find must retain its separate name-selector behavior.");
		});

		registry.Add("Editor.BuildProfile.RemoveStableIdPreservesCollidingName", [](TestContext& context) {
			const IdentityCollectionDirectory directory;
			const auto target = directory.Path() / "legacy-target.json";
			const auto alias = directory.Path() / "alias.json";
			context.Expect(SaveBuildProfile(IdentityProfile("targetid", "Target"), target), "The intended identity must exist.");
			context.Expect(SaveBuildProfile(IdentityProfile("otherid", "targetid"), alias), "The colliding name must exist.");
			const auto aliasBytes = ReadIdentityFile(alias);
			const auto removed = RemoveBuildProfileFromCollection(directory.Path(), "targetid");
			context.Expect(removed.succeeded && removed.profile && removed.profile->id == "targetid", "Removal must select the stable identity.");
			context.Expect(!std::filesystem::exists(target), "The intended legacy-named file must be removed.");
			context.Expect(ReadIdentityFile(alias) == aliasBytes, "The profile with the colliding display name must survive.");
		});

		registry.Add("Editor.BuildProfile.NewDraftUsesStableIdFilename", [](TestContext& context) {
			const IdentityCollectionDirectory directory;
			const auto saved = SaveBuildProfileToCollection(directory.Path(), IdentityProfile("freshid", "Fresh"));
			context.Expect(saved.succeeded && saved.profile.has_value(), "A fresh profile must still save normally.");
			context.Expect(saved.profile->filePath == directory.Path() / "freshid.json", "A new profile without a prior path uses its stable ID filename.");
		});
	}
}
