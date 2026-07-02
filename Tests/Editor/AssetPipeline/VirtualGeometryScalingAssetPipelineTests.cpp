#include "TestHarness.h"

#include "Renderer/RenderTypes.h"

namespace TerraEngine::EditorTests {

	void RegisterVirtualGeometryScalingAssetPipelineTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometryScaling.Foundation.ConeMetadataDefaultsSafe", [](TestContext& context) {
			ConeSafetyMetadata cone;

			context.Expect(cone.materialSidedness == VirtualGeometryConeMaterialSidedness::Unknown, "Imported cone metadata should start unknown until the asset pipeline validates sidedness.");
			context.Expect(cone.windingState == VirtualGeometryConeWindingState::Unknown, "Imported cone metadata should start unknown until winding validation runs.");
			context.Expect(!cone.coneValid, "Unknown imported metadata must keep cone culling disabled.");
		});
	}
}
