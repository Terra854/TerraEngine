#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	void RegisterGltfImportDiagnosticsTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfDiagnostics.WarnsForMissingTexture", [](TestContext& context) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/missing-texture.gltf";
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfDiagnosticsTests";

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = "Models/missing-texture.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();

			const auto result = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			bool sawTextureWarning = false;
			for (const auto& diagnostic : result.diagnostics) {
				sawTextureWarning = sawTextureWarning || diagnostic.code == "TextureCompileFailed";
			}
			context.Expect(result.status == TerraEngine::AssetPipeline::GltfImportStatus::SucceededWithWarnings, "Missing texture fixture should import with warnings.");
			context.Expect(sawTextureWarning, "Missing texture should remain machine-readable in diagnostics.");
		});
	}
}
