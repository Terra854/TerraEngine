#include "TestHarness.h"

#include "Renderer/RenderTypes.h"

#include <cstddef>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryShaderLayoutTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.ShaderLayout.PushConstantsCarrySelectedLodRange", [](TestContext& context) {
			context.Expect(offsetof(PushConstants, virtualMeshletOffset) > offsetof(PushConstants, indirectDrawOffset), "Virtual geometry range should follow indirect draw metadata.");
			context.ExpectEqual(offsetof(PushConstants, virtualMeshletOffset) + sizeof(std::uint32_t), offsetof(PushConstants, virtualMeshletCount), "Virtual meshlet offset/count should be tightly packed.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualLod) + sizeof(std::uint32_t), offsetof(PushConstants, virtualGeometryFlags), "Selected LOD and flags should mirror shader uint fields.");
			context.Expect((offsetof(PushConstants, editorTint) % 16u) == 0u, "Editor tint should remain 16-byte aligned after virtual geometry push constants.");
		});
	}
}
