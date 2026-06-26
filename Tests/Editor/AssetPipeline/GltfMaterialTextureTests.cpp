#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	namespace {
		TerraEngine::AssetPipeline::GltfImportResult ImportFixture(std::string_view fixtureName) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf" / fixtureName;
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfMaterialTests" / fixtureName;

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / fixtureName;
			options.packageGuid = TerraEngine::GenerateUUID();
			return TerraEngine::AssetPipeline::ImportGltfPackage(options);
		}
	}

	void RegisterGltfMaterialTextureTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfMaterialTexture.RecordsEmbeddedBaseColorTexture", [](TestContext& context) {
			const auto result = ImportFixture("embedded-texture.gltf");
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "Embedded texture fixture should still import package geometry.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedTextures.size(), "Importer should report the embedded base-color texture.");
			context.Expect(result.generatedTextures[0].semantic == TerraEngine::AssetPipeline::GltfTextureSemantic::Color, "Base-color texture should use color semantics.");
			context.Expect(!result.generatedMaterials.empty() && !result.generatedMaterials[0].textureGuids.empty(), "Generated material should bind the generated texture id.");
		});
	}
}
