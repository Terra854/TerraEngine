#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	void RegisterFbxPackageImporterTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxPackageImporter.ImportsStaticBox", [](TestContext& context) {
			const auto result = ImportFbxFixture("box.fbx");
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Static FBX box should import successfully.");
			context.Expect(result.package.packageKind == TerraEngine::AssetPipeline::ModelPackageKind::Fbx, "Package kind should be FBX.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedMeshes.size(), "Importer should report one package mesh.");
			context.Expect(!result.generatedMeshes.front().compiledPath.empty(), "Generated mesh should have a compiled TEMF path.");
			context.Expect(!result.materialAssignments.empty(), "Importer should preserve at least one material assignment.");
		});
	}
}
