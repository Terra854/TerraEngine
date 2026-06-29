#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	void RegisterFbxPackageBrowserContractTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxPackageBrowserContract.SidecarContainsExpandableChildren", [](TestContext& context) {
			const auto result = ImportFbxFixture("box.fbx");
			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);

			context.Expect(sidecar["package"].value("packageKind", "") == "Fbx", "Browser sidecar should identify FBX packages.");
			context.Expect(sidecar["generatedMeshes"].is_array() && !sidecar["generatedMeshes"].empty(), "Browser sidecar should contain mesh child records.");
			context.Expect(sidecar["generatedMaterials"].is_array() && !sidecar["generatedMaterials"].empty(), "Browser sidecar should contain material child records.");
			context.Expect(sidecar["generatedMeshes"][0].contains("guid"), "Child records should expose GUIDs for selection/dragging.");
			context.Expect(sidecar["generatedMeshes"][0].contains("sourcePath"), "Child records should expose source paths for tooltips/inspection.");
		});
	}
}
