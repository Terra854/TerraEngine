#include "TestHarness.h"

#include "GltfPackageImporter.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	void RegisterGltfMaterialAssignmentTests(TestRegistry& registry) {
		registry.Add("Editor.AssetPipeline.GltfMaterialAssignment.PreservesPrimitiveSlots", [](TestContext& context) {
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/multi-material.gltf";
			const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "TerraEngineGltfAssignmentTests";

			TerraEngine::AssetPipeline::GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = "Models/multi-material.gltf";
			options.packageGuid = TerraEngine::GenerateUUID();

			const auto result = TerraEngine::AssetPipeline::ImportGltfPackage(options);
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed, "Multi-material glTF should import successfully.");
			context.ExpectEqual(static_cast<std::size_t>(2), result.generatedMaterials.size(), "Importer should report both source materials.");
			context.ExpectEqual(static_cast<std::size_t>(2), result.materialAssignments.size(), "Importer should create one assignment per primitive.");
			context.Expect(result.materialAssignments[0].materialGuid != result.materialAssignments[1].materialGuid, "Different source primitive materials should keep different generated ids.");
		});
	}
}
