#include "TestHarness.h"

#include "FbxTestHelpers.h"

namespace TerraEngine::EditorTests
{
	void RegisterFbxMaterialAssignmentTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxMaterialAssignment.PreservesMeshSlots", [](TestContext& context) {
			const auto result = ImportFbxFixture("cubes_with_names.fbx");
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Multi-node FBX should import successfully.");
			context.Expect(!result.materialAssignments.empty(), "Importer should create material assignments for imported mesh sections.");
			for (const TerraEngine::AssetPipeline::ModelMaterialAssignment& assignment : result.materialAssignments) {
				context.Expect(!assignment.slotName.empty(), "Assignment slot names should be readable.");
			}
		});
	}
}
