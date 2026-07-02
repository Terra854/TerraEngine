#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"

namespace TerraEngine::Tests {
namespace {
	TerraEngine::Renderer::VirtualGeometryViewSelectionState MakeBatchedSelectionState()
	{
		TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
		state.viewType = RenderViewType::Game;
		state.outputId = 4u;
		state.targetSize = glm::uvec2(1280u, 720u);

		VirtualGeometryClusterPacket packet;
		packet.packetId = 7u;
		packet.viewId = state.outputId;
		packet.viewType = state.viewType;
		packet.objectId = TerraEngine::GenerateUUID();
		packet.meshId = TerraEngine::GenerateUUID();
		packet.materialId = TerraEngine::GenerateUUID();
		packet.selectedLod = 2u;
		packet.clusterId = 31u;
		packet.meshletOffset = 64u;
		packet.meshletCount = 8u;
		packet.pageId = 3u;
		packet.fallbackPageId = 0u;
		packet.debugIdentity = 99u;
		state.clusterPackets.push_back(packet);

		ClusterWorkBatch batch;
		batch.batchId = 2u;
		batch.viewId = state.outputId;
		batch.viewType = state.viewType;
		batch.meshId = packet.meshId;
		batch.materialId = packet.materialId;
		batch.packetOffset = 0u;
		batch.packetCount = 1u;
		batch.selectedMeshlets = 8u;
		batch.submittedMeshlets = 8u;
		batch.batchReason = "same mesh and material";
		state.clusterBatches.push_back(batch);

		CompactedVisibilitySet compaction;
		compaction.viewId = state.outputId;
		compaction.viewType = state.viewType;
		compaction.candidatePackets = 1u;
		compaction.visiblePackets = 1u;
		compaction.submittedBatches = 1u;
		state.compactionResults.push_back(compaction);
		return state;
	}
	}

	void RegisterVirtualGeometryBatchingTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Batching.US1.DenseSelectedClustersCollapseIntoBoundedBatches", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> packets;
			packets.reserve(500u);
			for (std::uint32_t index = 0u; index < 500u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.viewId = 9u;
				packet.viewType = RenderViewType::Game;
				packet.objectId = objectId;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = 2u;
				packet.clusterId = index;
				packet.meshletOffset = index * 4u;
				packet.meshletCount = 4u;
				packet.pageId = 3u;
				packets.push_back(packet);
			}

			const std::vector<ClusterWorkBatch> batches = RenderSystem::BuildVirtualGeometryClusterBatches(packets);

			context.Expect(!batches.empty(), "Dense selected clusters should produce at least one batch.");
			context.Expect(batches.size() <= packets.size() / 10u, "Dense contiguous clusters should submit at most 10% as many batches as selected packets.");
			context.ExpectEqual(500u, batches.front().packetCount, "One compatible contiguous packet run should collapse into one submitted batch.");
			context.ExpectEqual(2000u, batches.front().submittedMeshlets, "Submitted meshlets should preserve the exact contiguous packet range.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryBatchFallbackMode::None), static_cast<std::uint32_t>(batches.front().fallbackMode), "Contiguous dense packets should not use the legacy one-cluster fallback.");
		});

		registry.Add("VirtualGeometry.Batching.US1.SparsePacketsKeepExplicitLegacyFallback", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> packets;
			for (std::uint32_t index = 0u; index < 3u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = 1u;
				packet.clusterId = index;
				packet.meshletOffset = index * 16u;
				packet.meshletCount = 4u;
				packets.push_back(packet);
			}

			const std::vector<ClusterWorkBatch> batches = RenderSystem::BuildVirtualGeometryClusterBatches(packets);

			context.ExpectEqual(static_cast<std::size_t>(3u), batches.size(), "Sparse packet ranges should avoid drawing the gaps between clusters.");
			for (const ClusterWorkBatch& batch : batches)
			{
				context.ExpectEqual(1u, batch.packetCount, "Sparse packet fallback should stay one exact packet per draw.");
				context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryBatchFallbackMode::LegacyClusterDraw), static_cast<std::uint32_t>(batch.fallbackMode), "Sparse packet fallback should be explicitly labeled for diagnostics.");
				context.Expect(!batch.fallbackReason.empty(), "Legacy fallback batches should explain why they did not merge.");
			}
		});

		registry.Add("VirtualGeometry.Batching.Foundation.CopiesPacketTablesIntoVisibilitySnapshots", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3001u;
			snapshot.virtualGeometryViewSelections.push_back(MakeBatchedSelectionState());

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility.front());

			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.size(), "One view selection should publish one immutable visibility snapshot.");
			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().clusterPackets.size(), "Visibility snapshot should copy selected cluster packets.");
			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().clusterBatches.size(), "Visibility snapshot should copy cluster work batches.");
			context.ExpectEqual(1u, diagnostics.clusterPacketCount, "Diagnostics should expose copied cluster packet count.");
			context.ExpectEqual(1u, diagnostics.submittedBatchCount, "Diagnostics should expose copied batch count.");
			context.ExpectEqual(8u, diagnostics.submittedMeshlets, "Diagnostics should report submitted meshlets from the batch.");
		});

		registry.Add("VirtualGeometry.Batching.Foundation.HashChangesWhenPacketTableChanges", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot first;
			first.frameId = 3002u;
			first.virtualGeometryViewSelections.push_back(MakeBatchedSelectionState());
			TerraEngine::Renderer::RenderFrameSnapshot second = first;
			second.virtualGeometryViewSelections.front().clusterPackets.front().packetId += 1u;

			context.Expect(
				TerraEngine::Renderer::BuildSnapshotDeterministicHash(first) != TerraEngine::Renderer::BuildSnapshotDeterministicHash(second),
				"Packet-table changes should invalidate deterministic snapshot hashes.");
		});

		registry.Add("VirtualGeometry.Batching.Foundation.VulkanPreparationCountsPacketTables", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3003u;
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
			snapshot.virtualGeometryViewSelections.push_back(MakeBatchedSelectionState());
			TerraEngine::Renderer::BackendCapabilityProfile capabilities;
			capabilities.supportsParallelRecording = true;

			const TerraEngine::Renderer::Vulkan::IndirectDrawPreparationPlan plan =
				TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(snapshot, capabilities);

			context.ExpectEqual(static_cast<std::size_t>(1u), plan.virtualGeometryClusterPacketCount, "Vulkan prep should count VG packet tables.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.virtualGeometryBatchCount, "Vulkan prep should count VG batch tables.");
			context.Expect(plan.virtualGeometryBatchPacketSignature != 0u, "Vulkan prep should publish a stable packet-table signature.");
		});
	}
}
