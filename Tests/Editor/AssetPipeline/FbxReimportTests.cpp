#include "TestHarness.h"

#include "FbxTestHelpers.h"

namespace TerraEngine::EditorTests
{
	void RegisterFbxReimportTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxReimport.PreservesPackageChildIdentity", [](TestContext& context) {
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto first = ImportFbxFixture("box.fbx", packageGuid);
			const auto second = ImportFbxFixture("box.fbx", packageGuid);

			context.Expect(first.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "First FBX import should succeed.");
			context.Expect(second.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Second FBX import should succeed.");
			context.Expect(!first.generatedMeshes.empty() && !second.generatedMeshes.empty(), "Both imports should produce a mesh.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMeshes.front().guid), TerraEngine::ToHexString(second.generatedMeshes.front().guid), "Generated mesh GUID should be stable for the same package GUID.");
			context.Expect(!first.generatedMaterials.empty() && !second.generatedMaterials.empty(), "Both imports should produce material records.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMaterials.front().guid), TerraEngine::ToHexString(second.generatedMaterials.front().guid), "Generated material GUID should be stable for the same package GUID.");
		});
	}
}
