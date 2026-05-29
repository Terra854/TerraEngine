#include "TestHarness.h"

#include "io/PathNormalizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempExportAssetDirectory {
		public:
			TempExportAssetDirectory()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "ExportPathNormalizer") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(root_);
			}

			~TempExportAssetDirectory() {
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

		bool HasStagedFile(const tepak::model::PackagingInventory& inventory, const std::string& normalizedPath, const std::string& assetClass) {
			return std::any_of(inventory.stagedFiles.begin(), inventory.stagedFiles.end(), [&](const tepak::model::StagedFile& file) {
				return file.normalizedPath == normalizedPath && file.assetClass == assetClass;
			});
		}
	}

	void RegisterExportPathNormalizerTests(TestRegistry& registry) {
		registry.Add("Editor.ExportPath.NormalizeLookupAndRejectTraversal", [](TestContext& context) {
			TempExportAssetDirectory tempDirectory;
			const std::filesystem::path sourcePath = tempDirectory.WriteFile("Textures/Hero/Albedo.PNG", "texture");
			const std::filesystem::path outsidePath = tempDirectory.Root().parent_path() / "OutsideExport" / "Escape.PNG";
			std::filesystem::create_directories(outsidePath.parent_path());
			std::ofstream(outsidePath, std::ios::binary) << "escape";

			context.ExpectEqual(std::string("textures/hero/albedo.png"), tepak::io::NormalizeLookupPath(R"(\Textures\Hero\Albedo.PNG)"), "Export lookup paths should normalize casing and slash direction.");
			context.ExpectEqual(std::string("textures/hero/albedo.png"), tepak::io::NormalizeRelativePath(tempDirectory.Root(), sourcePath), "Export relative paths should be stable and root-relative.");

			bool rejectedTraversal = false;
			try {
				(void)tepak::io::NormalizeRelativePath(tempDirectory.Root(), outsidePath);
			} catch(const std::runtime_error&) {
				rejectedTraversal = true;
			}

			context.Expect(rejectedTraversal, "Export path normalization should reject files outside the declared asset root.");
		});

		registry.Add("Editor.ExportPath.DiscoverPackagingInventoryHonorsSelectionRules", [](TestContext& context) {
			TempExportAssetDirectory tempDirectory;
			const std::filesystem::path shaderPath = tempDirectory.WriteFile("Shaders/Default.frag", "shader");
			const std::filesystem::path meshPath = tempDirectory.WriteFile("Meshes/Cube.temf", "mesh");
			const std::filesystem::path texturePath = tempDirectory.WriteFile("Textures/Hero.png", "texture");

			tepak::PackagingRequest request{};
			request.inputRoot = tempDirectory.Root();
			request.entrySelectionRules = {".temf", ".png"};
			request.enableCompression = false;
			request.enableDictionaryTraining = false;

			const tepak::model::PackagingInventory inventory = tepak::io::DiscoverPackagingInventory(request);

			context.Expect(std::filesystem::exists(shaderPath) && std::filesystem::exists(meshPath) && std::filesystem::exists(texturePath), "All authored export assets should exist during inventory discovery.");
			context.ExpectEqual(2u, static_cast<unsigned int>(inventory.stagedFiles.size()), "Selection rules should include only matching asset suffixes.");
			context.Expect(HasStagedFile(inventory, "meshes/cube.temf", "mesh"), "Selected mesh files should be staged.");
			context.Expect(HasStagedFile(inventory, "textures/hero.png", "texture"), "Selected texture files should be staged.");
			context.Expect(!HasStagedFile(inventory, "shaders/default.frag", "shader"), "Unselected shader files should not be staged.");
		});

		registry.Add("Editor.ExportPath.EmptySelectionFailsClearly", [](TestContext& context) {
			TempExportAssetDirectory tempDirectory;
			const std::filesystem::path shaderPath = tempDirectory.WriteFile("Shaders/Default.FRAG", "shader");

			tepak::PackagingRequest request{};
			request.inputRoot = tempDirectory.Root();
			request.entrySelectionRules = {".png"};
			request.enableCompression = false;
			request.enableDictionaryTraining = false;

			bool rejectedEmptySelection = false;
			try {
				(void)tepak::io::DiscoverPackagingInventory(request);
			} catch(const std::runtime_error&) {
				rejectedEmptySelection = true;
			}

			context.Expect(std::filesystem::exists(shaderPath), "The source file should exist even when selection rules exclude it.");
			context.Expect(rejectedEmptySelection, "Inventory discovery should fail clearly when no files match the requested export selection.");
		});
	}
}
