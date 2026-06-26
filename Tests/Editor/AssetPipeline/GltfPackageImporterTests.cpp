#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	namespace {
		TerraEngine::AssetPipeline::GltfImportResult ImportFixture(std::string_view fixtureName) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf" / fixtureName;
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfImporterTests" / fixtureName;

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / fixtureName;
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportGltfPackage(options);
		}
	}

	void RegisterGltfPackageImporterTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfPackageImporter.ImportsOneMaterialFixture", [](TestContext& context) {
			const auto result = ImportFixture("one-material.gltf");
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "One-material glTF should import successfully.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedMeshes.size(), "Importer should report one generated mesh.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedMaterials.size(), "Importer should report one generated material.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.materialAssignments.size(), "Importer should preserve the primitive material slot.");
		});
	}
}
