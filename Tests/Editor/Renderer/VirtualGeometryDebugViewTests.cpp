#include "TestHarness.h"

#include "Engine/Renderer/RenderTypes.h"

#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryDebugViewTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.DebugView.ExposesExpectedVisualizerModes", [](TestContext& context) {
			const VirtualGeometryDebugVisualizationMode modes[] = {
				VirtualGeometryDebugVisualizationMode::None,
				VirtualGeometryDebugVisualizationMode::LOD,
				VirtualGeometryDebugVisualizationMode::MeshletId,
				VirtualGeometryDebugVisualizationMode::Bounds,
				VirtualGeometryDebugVisualizationMode::Culled,
				VirtualGeometryDebugVisualizationMode::HiZ,
				VirtualGeometryDebugVisualizationMode::Residency,
				VirtualGeometryDebugVisualizationMode::Error
			};

			for (const VirtualGeometryDebugVisualizationMode mode : modes)
			{
				const std::string label = ToString(mode);
				context.Expect(!label.empty(), "Every virtual geometry visualizer mode should have a Game View label.");
				context.Expect(label != "Unknown", "Known virtual geometry visualizer modes should not report Unknown.");
			}

			context.ExpectEqual(std::string("Hi-Z"), std::string(ToString(VirtualGeometryDebugVisualizationMode::HiZ)), "The Game View visualizer inventory should include the Hi-Z mode.");
			context.ExpectEqual(std::string("Meshlet Id"), std::string(ToString(VirtualGeometryDebugVisualizationMode::MeshletId)), "The Game View visualizer inventory should include the meshlet identity mode.");
		});
	}
}
