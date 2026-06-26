#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	void RegisterGltfReimportTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfReimport.ReusesSourceHash", [](TestContext& context) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfReimportTests";

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = "Models/one-material.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();

			const auto first = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			const auto second = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			context.Expect(first.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "First import should succeed.");
			context.Expect(second.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "Second import should succeed.");
			context.ExpectEqual(first.package.sourceHashHex, second.package.sourceHashHex, "Reimport should report a stable source hash for unchanged source content.");
		});
	}
}
