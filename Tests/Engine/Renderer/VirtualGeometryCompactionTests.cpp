#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests {

	void RegisterVirtualGeometryCompactionTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Compaction.US2.CompactsRejectedAndFailOpenPackets", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> candidates;
			for (std::uint32_t index = 0u; index < 4u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.clusterId = index;
				packet.meshletOffset = index * 8u;
				packet.meshletCount = 8u;
				packet.projectedBounds = glm::vec4(0.1f, 0.1f, 0.2f, 0.2f);
				candidates.push_back(packet);
			}
			candidates[1].projectedBounds = glm::vec4(1.2f, 0.1f, 1.4f, 0.2f);
			candidates[2].meshletCount = 0u;
			candidates[3].fallbackPacket = true;

			std::vector<VirtualGeometryClusterPacket> compacted;
			const CompactedVisibilitySet compaction =
				RenderSystem::CompactVirtualGeometryClusterPackets(candidates, compacted, RenderViewType::Game, 17u);

			context.ExpectEqual(4u, compaction.candidatePackets, "Compaction should count every candidate packet.");
			context.ExpectEqual(2u, compaction.visiblePackets, "Only visible/fail-open packets should survive compaction.");
			context.ExpectEqual(1u, compaction.rejectedByFrustum, "Projected bounds outside the view should be frustum rejected.");
			context.ExpectEqual(1u, compaction.rejectedByResidency, "Zero meshlet packets should be treated as missing-resource rejections.");
			context.ExpectEqual(1u, compaction.failOpenPackets, "Fallback packets should publish fail-open coverage.");
			context.ExpectEqual(static_cast<std::size_t>(2u), compacted.size(), "Compacted packet list should only contain submitted packets.");
			context.ExpectEqual(0u, compacted[0].clusterId, "The first visible packet should stay in order.");
			context.ExpectEqual(3u, compacted[1].clusterId, "The fallback packet should remain visible.");
		});

		registry.Add("VirtualGeometry.Compaction.US2.MissingResourceOnlySelectionFailsOpen", [](TestContext& context) {
			std::vector<VirtualGeometryClusterPacket> candidates(3u);
			for (std::uint32_t index = 0u; index < 3u; ++index)
			{
				candidates[index].packetId = index;
				candidates[index].clusterId = index;
				candidates[index].meshletCount = 0u;
			}

			std::vector<VirtualGeometryClusterPacket> compacted;
			const CompactedVisibilitySet compaction =
				RenderSystem::CompactVirtualGeometryClusterPackets(candidates, compacted, RenderViewType::Scene, 18u);

			context.ExpectEqual(3u, compaction.visiblePackets, "All-resource-missing selections should fail open rather than create a hole.");
			context.ExpectEqual(3u, compaction.failOpenPackets, "Fail-open packet count should identify the restored coverage.");
			context.ExpectEqual(0u, compaction.rejectedByResidency, "Residency rejection should clear when the helper restores coverage.");
			context.ExpectEqual(static_cast<std::size_t>(3u), compacted.size(), "Fail-open compaction should submit the original packets.");
		});

		registry.Add("VirtualGeometry.Compaction.Foundation.DiagnosticsExposeRejectedPacketCounters", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.frameId = 3010u;
			visibility.outputId = 6u;
			visibility.viewType = RenderViewType::Game;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::Prepared;

			CompactedVisibilitySet compaction;
			compaction.viewId = visibility.outputId;
			compaction.viewType = visibility.viewType;
			compaction.candidatePackets = 10u;
			compaction.visiblePackets = 6u;
			compaction.rejectedByFrustum = 2u;
			compaction.rejectedByHiZ = 1u;
			compaction.rejectedByResidency = 1u;
			compaction.submittedBatches = 2u;
			visibility.compactionResults.push_back(compaction);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(10u, diagnostics.candidateClusterCount, "Compaction diagnostics should report candidate packets.");
			context.ExpectEqual(6u, diagnostics.compactedClusterCount, "Compaction diagnostics should report visible packets.");
			context.ExpectEqual(2u, diagnostics.culledByFrustum, "Compaction diagnostics should report frustum packet rejection.");
			context.ExpectEqual(1u, diagnostics.culledByHiZ, "Compaction diagnostics should preserve Hi-Z rejection counters.");
			context.ExpectEqual(1u, diagnostics.culledByResidency, "Compaction diagnostics should report residency substitutions or rejections.");
			context.ExpectEqual(4u, diagnostics.rejectedClusterCount, "Compaction diagnostics should aggregate rejected packet domains.");
			context.ExpectEqual(2u, diagnostics.submittedBatchCount, "Compaction diagnostics should report submitted batch count.");
		});
	}
}
