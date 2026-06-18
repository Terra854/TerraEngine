#include "TestHarness.h"

#include "Runtime/ContentMount/io/PathNormalizer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class TempContentDirectory {
		public:
			TempContentDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "ContentMountPathNormalizer") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempContentDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

			[[nodiscard]] std::filesystem::path WriteFile(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = root_ / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				stream << contents;
				return fullPath;
			}

		private:
			std::filesystem::path root_;
		};

		bool HasDirectory(const tepak::model::PackagingInventory& inventory, const std::string& normalizedPath) {
			return std::any_of(inventory.directories.begin(), inventory.directories.end(), [&normalizedPath](const tepak::DirectoryNode& directory) {
				return directory.normalizedPath == normalizedPath;
			});
		}

		bool HasStagedFile(const tepak::model::PackagingInventory& inventory, const std::string& normalizedPath, const std::string& assetClass) {
			return std::any_of(inventory.stagedFiles.begin(), inventory.stagedFiles.end(), [&](const tepak::model::StagedFile& file) {
				return file.normalizedPath == normalizedPath && file.assetClass == assetClass;
			});
		}
	}

	void RegisterContentMountPathNormalizerTests(TestRegistry& registry) {
		registry.Add("Engine.ContentMount.NormalizeLookupAndDirectoryHelpers", [](TestContext& context) {
			context.ExpectEqual(std::string("textures/hero/albedo.png"), tepak::io::NormalizeLookupPath(R"(\Textures\Hero\Albedo.PNG)"), "Lookup paths should use lowercase forward slashes without a leading separator.");
			context.ExpectEqual(std::string("materials/main.json"), tepak::io::NormalizeLookupPath("./Materials/Main.JSON"), "Lookup paths should strip a single current-directory prefix.");
			context.ExpectEqual(std::string("cube.temf"), tepak::io::ExtractLeafName("meshes/props/cube.temf"), "Leaf extraction should return the final normalized path segment.");

			const std::vector<std::string> directories = tepak::io::EnumerateDirectoryPaths("meshes/props/cube.temf");
			context.ExpectEqual(2u, static_cast<unsigned int>(directories.size()), "A nested file should contribute each implicit parent directory.");
			context.ExpectEqual(std::string("meshes"), directories[0], "The first implicit directory should be the top-level path.");
			context.ExpectEqual(std::string("meshes/props"), directories[1], "The second implicit directory should include the nested path.");
		});

		registry.Add("Engine.ContentMount.DetectAssetClassMapsKnownExtensions", [](TestContext& context) {
			context.ExpectEqual(std::string("shader"), tepak::io::DetectAssetClass("Default.FRAG"), "Shader extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("texture"), tepak::io::DetectAssetClass("Albedo.PNG"), "Texture extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("mesh"), tepak::io::DetectAssetClass("Cube.TEMF"), "Mesh extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("audio"), tepak::io::DetectAssetClass("Loop.WAV"), "Audio extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("video"), tepak::io::DetectAssetClass("Intro.MP4"), "Video extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("data"), tepak::io::DetectAssetClass("Config.JSON"), "Data extensions should be recognized case-insensitively.");
			context.ExpectEqual(std::string("other"), tepak::io::DetectAssetClass("Readme.unknown"), "Unknown extensions should remain in the other bucket.");
		});

		registry.Add("Engine.ContentMount.NormalizeRelativePathRejectsTraversal", [](TestContext& context) {
			TempContentDirectory tempDirectory;
			const std::filesystem::path sourcePath = tempDirectory.WriteFile("Assets/Sub/Foo.PNG", "abc");
			const std::filesystem::path outsidePath = tempDirectory.Root().parent_path() / "Outside" / "Escape.PNG";
			std::filesystem::create_directories(outsidePath.parent_path());
			std::ofstream(outsidePath, std::ios::binary) << "escape";

			context.ExpectEqual(std::string("assets/sub/foo.png"), tepak::io::NormalizeRelativePath(tempDirectory.Root(), sourcePath), "Relative archive paths should be lowercase and root-relative.");

			bool rejectedTraversal = false;
			try {
				(void)tepak::io::NormalizeRelativePath(tempDirectory.Root(), outsidePath);
			} catch(const std::runtime_error&) {
				rejectedTraversal = true;
			}

			context.Expect(rejectedTraversal, "Path normalization should reject files outside the declared input root.");
		});

		registry.Add("Engine.ContentMount.DiscoverPackagingInventoryBuildsStableFileAndDirectoryMetadata", [](TestContext& context) {
			TempContentDirectory tempDirectory;
			const std::filesystem::path shaderPath = tempDirectory.WriteFile("Shaders/Default.FRAG", "shader");
			const std::filesystem::path meshPath = tempDirectory.WriteFile("Meshes/Props/Cube.TEMF", "mesh");
			const std::filesystem::path audioPath = tempDirectory.WriteFile("Audio/Loop.WAV", "au");

			tepak::PackagingRequest request{};
			request.inputRoot = tempDirectory.Root();
			request.enableCompression = false;
			request.enableDictionaryTraining = false;

			const tepak::model::PackagingInventory inventory = tepak::io::DiscoverPackagingInventory(request);

			context.ExpectEqual(3u, static_cast<unsigned int>(inventory.stagedFiles.size()), "Inventory discovery should stage every file under the input root.");
			context.ExpectEqual(12ull, static_cast<unsigned long long>(inventory.totalInputBytes), "Inventory discovery should sum logical input bytes.");
			context.Expect(std::filesystem::exists(shaderPath) && std::filesystem::exists(meshPath) && std::filesystem::exists(audioPath), "The staged files should exist throughout inventory discovery.");
			context.Expect(HasDirectory(inventory, ""), "Inventory discovery should include the root directory node.");
			context.Expect(HasDirectory(inventory, "meshes/props"), "Inventory discovery should include nested implicit directories.");
			context.Expect(HasStagedFile(inventory, "shaders/default.frag", "shader"), "Shader files should be staged with normalized paths and classes.");
			context.Expect(HasStagedFile(inventory, "meshes/props/cube.temf", "mesh"), "Mesh files should be staged with normalized paths and classes.");
			context.Expect(inventory.dictionarySamples.empty(), "Dictionary sampling should stay disabled when compression training is disabled.");
		});
	}
}
