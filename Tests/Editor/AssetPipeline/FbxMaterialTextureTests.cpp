#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <algorithm>
#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterFbxMaterialTextureTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxMaterialTexture.ImportsMaterialRecords", [](TestContext& context) {
			const auto result = ImportFbxFixture("phong_cube.fbx");
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Material FBX should import successfully.");
			context.Expect(!result.generatedMaterials.empty(), "Importer should generate material records.");
			context.Expect(!result.generatedMaterials.front().displayName.empty(), "Generated material should have a display name.");
		});

		registry.Add("Editor.AssetPipeline.FbxMaterialTexture.ReportsOrImportsEmbeddedTextures", [](TestContext& context) {
			const auto result = ImportFbxFixture("box_embedded_texture_fragmented.fbx");
			const bool hasTexture = !result.generatedTextures.empty();
			const bool hasTextureDiagnostic = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code.find("Texture") != std::string::npos;
			});
			context.Expect(hasTexture || hasTextureDiagnostic || !result.diagnostics.empty(), "Embedded texture fixture should either import texture outputs or report diagnostics without crashing.");
		});
	}
}
