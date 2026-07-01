#include "TestHarness.h"

#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderTypes.h"

#include <cstddef>
#include <type_traits>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryShaderLayoutTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.ShaderLayout.PushConstantsCarrySelectedLodRange", [](TestContext& context) {
			context.Expect(offsetof(PushConstants, virtualMeshletOffset) > offsetof(PushConstants, indirectDrawOffset), "Virtual geometry range should follow indirect draw metadata.");
			context.ExpectEqual(offsetof(PushConstants, virtualMeshletOffset) + sizeof(std::uint32_t), offsetof(PushConstants, virtualMeshletCount), "Virtual meshlet offset/count should be tightly packed.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualLod) + sizeof(std::uint32_t), offsetof(PushConstants, virtualGeometryFlags), "Selected LOD and flags should mirror shader uint fields.");
			context.ExpectEqual(offsetof(PushConstants, virtualGeometryFlags) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterOffset), "Virtual cluster payload should begin immediately after virtual geometry flags.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualClusterOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualClusterCount), "Selected cluster offset/count should be tightly packed for mesh shader decoding.");
			context.ExpectEqual(offsetof(PushConstants, selectedVirtualPageNeedOffset) + sizeof(std::uint32_t), offsetof(PushConstants, selectedVirtualPageNeedCount), "Page need offset/count should be tightly packed for streaming diagnostics.");
			context.Expect((offsetof(PushConstants, editorTint) % 16u) == 0u, "Editor tint should remain 16-byte aligned after virtual geometry push constants.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.ClusterSelectionRecordsStayCompact", [](TestContext& context) {
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord) <= 64u, "Selected cluster records should stay compact enough for per-view payload uploads.");
			context.Expect(sizeof(TerraEngine::Renderer::VirtualGeometryPageNeedRecord) <= 80u, "Page need records should stay compact enough for streaming handoff snapshots.");
		});

		registry.Add("VirtualGeometry.ShaderLayout.SelectedClusterRecordsCarrySparseIdentity", [](TestContext& context) {
			using TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord;
			context.Expect(std::is_standard_layout_v<VirtualGeometrySelectedClusterRecord>, "Selected cluster records should stay standard-layout for future GPU upload buffers.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, clusterId) < offsetof(VirtualGeometrySelectedClusterRecord, lodIndex), "Cluster id should be stored before LOD metadata for compact shader decoding.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, pageId) < offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Page id and fallback cluster id should both be present, not replaced by a selected-cluster counter only.");
			context.Expect(offsetof(VirtualGeometrySelectedClusterRecord, usingFallback) > offsetof(VirtualGeometrySelectedClusterRecord, fallbackClusterId), "Fallback state should travel with each sparse cluster record.");
		});
	}
}
