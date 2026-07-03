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

		registry.Add("VirtualGeometry.Batching.US1.GpuPacketRecordsMirrorCpuClusterPackets", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			VirtualGeometryClusterPacket packet;
			packet.packetId = 42u;
			packet.viewId = 3u;
			packet.viewType = RenderViewType::Game;
			packet.objectId = objectId;
			packet.meshId = meshId;
			packet.materialId = materialId;
			packet.selectedLod = 2u;
			packet.clusterId = 77u;
			packet.meshletOffset = 128u;
			packet.meshletCount = 16u;
			packet.boundsMin = glm::vec3(-1.0f);
			packet.boundsMax = glm::vec3(1.0f);
			packet.projectedBounds = glm::vec4(0.25f, 0.25f, 0.75f, 0.75f);
			packet.pageId = 9u;
			packet.fallbackPageId = 4u;
			packet.debugIdentity = 1234u;
			packet.fallbackPacket = true;

			const std::vector<GpuVirtualGeometryPacketRecord> records =
				RenderSystem::BuildGpuVirtualGeometryPacketRecords(
					{ packet },
					5000u,
					VirtualGeometryDebugVisualizationMode::Cluster,
					100u);

			context.ExpectEqual(static_cast<std::size_t>(1u), records.size(), "One CPU packet should produce one GPU packet mirror.");
			context.ExpectEqual(100u, records.front().packetId, "GPU packet ids should honor the caller-provided base.");
			context.ExpectEqual(5000ull, records.front().frameId, "GPU packet records should carry frame ownership.");
			context.ExpectEqual(packet.clusterId, records.front().clusterId, "GPU packets should preserve selected cluster identity.");
			context.ExpectEqual(packet.meshletOffset, records.front().meshletOffset, "GPU packets should preserve meshlet offset.");
			context.ExpectEqual(packet.meshletCount, records.front().meshletCount, "GPU packets should preserve meshlet count.");
			context.Expect((records.front().flags & 1u) != 0u, "GPU packet flags should mark fail-open coverage.");
			context.Expect(((records.front().flags >> 8u) & 0xffu) == static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Cluster), "GPU packet flags should mirror the view debug mode.");
		});

		registry.Add("VirtualGeometry.Batching.US1.GpuBatchDescriptorsSplitOnCompatibilityKeys", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialA = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialB = TerraEngine::GenerateUUID();
			std::vector<GpuVirtualGeometryPacketRecord> packets;
			for (std::uint32_t index = 0u; index < 5u; ++index)
			{
				GpuVirtualGeometryPacketRecord packet;
				packet.packetId = index;
				packet.viewId = 2u;
				packet.viewType = RenderViewType::Game;
				packet.frameId = 7000u;
				packet.meshId = meshId;
				packet.materialId = index < 3u ? materialA : materialB;
				packet.selectedLod = 1u;
				packet.clusterId = index;
				packet.meshletOffset = index * 4u;
				packet.meshletCount = 4u;
				packet.pageId = 3u;
				packet.flags = index == 4u ? 1u : 0u;
				packets.push_back(packet);
			}

			const std::vector<GpuVirtualGeometryBatchDescriptor> batches =
				RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
					packets,
					7000u,
					VirtualGeometryDebugVisualizationMode::LOD);

			context.ExpectEqual(static_cast<std::size_t>(3u), batches.size(), "GPU batches should split when material or fallback state changes.");
			context.ExpectEqual(3u, batches[0].packetCount, "The first material-compatible packet run should stay one batch.");
			context.Expect(materialA == batches[0].materialId, "The first batch should preserve material A.");
			context.Expect(materialB == batches[1].materialId, "The second batch should preserve material B.");
			context.ExpectEqual(1u, batches[2].packetCount, "Fallback packet state should not merge with a non-fallback neighbor.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryBatchFallbackMode::LegacyClusterDraw), static_cast<std::uint32_t>(batches[2].fallbackMode), "Single-packet fallback batches should stay explicitly labeled.");
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

		registry.Add("VirtualGeometry.Batching.US1.CopiesGpuPacketTablesIntoVisibilitySnapshots", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3010u;
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state = MakeBatchedSelectionState();
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				state.clusterPackets,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::MeshletId);
			state.gpuBatchDescriptors = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
				state.gpuPacketRecords,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::MeshletId);
			state.gpuPacketFrameSets.push_back(RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(
				state.viewType,
				state.outputId,
				snapshot.frameId,
				state.targetSize,
				VirtualGeometryDebugVisualizationMode::MeshletId,
				0u,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				0u,
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size())));
			snapshot.virtualGeometryViewSelections.push_back(state);

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility.front());

			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().gpuPacketRecords.size(), "Visibility snapshots should copy GPU packet mirrors.");
			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().gpuBatchDescriptors.size(), "Visibility snapshots should copy GPU batch descriptors.");
			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().gpuPacketFrameSets.size(), "Visibility snapshots should copy GPU packet frame-set ownership.");
			context.ExpectEqual(1u, diagnostics.gpuPacketRecordCount, "Diagnostics should expose GPU packet mirror count.");
			context.ExpectEqual(1u, diagnostics.gpuBatchDescriptorCount, "Diagnostics should expose GPU batch descriptor count.");
			context.ExpectEqual(1u, diagnostics.gpuValidFrameSetCount, "Diagnostics should expose valid GPU packet frame-set count.");
		});

		registry.Add("VirtualGeometry.Batching.Foundation.LightweightViewDiagnosticsExposePacketCounts", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3004u;
			snapshot.renderSettings.virtualGeometry = MakeDefaultVirtualGeometryQualityProfile();
			snapshot.virtualGeometryViewSelections.push_back(MakeBatchedSelectionState());

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					snapshot.renderSettings.virtualGeometry,
					VirtualGeometryInactiveReason::None);

			context.ExpectEqual(static_cast<std::size_t>(1u), diagnostics.size(), "One explicit selection state should produce one lightweight diagnostics snapshot.");
			context.ExpectEqual(1u, diagnostics.front().clusterPacketCount, "Lightweight diagnostics should expose cluster packet counts without requiring a visibility snapshot.");
			context.ExpectEqual(1u, diagnostics.front().submittedBatchCount, "Lightweight diagnostics should expose batch counts without running full asset inventory diagnostics.");
			context.ExpectEqual(1u, diagnostics.front().candidateClusterCount, "Lightweight diagnostics should expose candidate packet counts for overlays.");
			context.ExpectEqual(1u, diagnostics.front().compactedClusterCount, "Lightweight diagnostics should expose compacted packet counts for overlays.");
			context.ExpectEqual(8u, diagnostics.front().submittedMeshlets, "Lightweight diagnostics should preserve submitted meshlet counts from the batch table.");
			context.Expect(diagnostics.front().virtualGeometryPerformanceGates.empty(), "Lightweight diagnostics should not allocate performance-gate strings for normal overlay stats.");
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

		registry.Add("VirtualGeometry.Batching.US2.GpuPacketMetadataInvalidatesSnapshotAndPreparationSignatures", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot first;
			first.frameId = 3020u;
			first.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state = MakeBatchedSelectionState();
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				state.clusterPackets,
				first.frameId,
				VirtualGeometryDebugVisualizationMode::LOD);
			state.gpuBatchDescriptors = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
				state.gpuPacketRecords,
				first.frameId,
				VirtualGeometryDebugVisualizationMode::LOD);
			state.gpuPacketFrameSets.push_back(RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(
				state.viewType,
				state.outputId,
				first.frameId,
				state.targetSize,
				VirtualGeometryDebugVisualizationMode::LOD,
				0u,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				0u,
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size())));
			first.virtualGeometryViewSelections.push_back(state);

			TerraEngine::Renderer::RenderFrameSnapshot second = first;
			second.virtualGeometryViewSelections.front().gpuPacketRecords.front().debugIdentity += 1u;
			second.virtualGeometryViewSelections.front().gpuBatchDescriptors.front().debugMode = VirtualGeometryDebugVisualizationMode::Cluster;

			TerraEngine::Renderer::BackendCapabilityProfile capabilities;
			capabilities.supportsParallelRecording = true;
			capabilities.supportsVirtualGeometryGpuPacketBuffers = true;
			const TerraEngine::Renderer::Vulkan::IndirectDrawPreparationPlan firstPlan =
				TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(first, capabilities);
			const TerraEngine::Renderer::Vulkan::IndirectDrawPreparationPlan secondPlan =
				TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(second, capabilities);

			context.Expect(
				TerraEngine::Renderer::BuildSnapshotDeterministicHash(first) != TerraEngine::Renderer::BuildSnapshotDeterministicHash(second),
				"Changing GPU packet metadata should invalidate immutable visibility snapshot hashes.");
			context.Expect(
				firstPlan.virtualGeometryBatchPacketSignature != secondPlan.virtualGeometryBatchPacketSignature,
				"Vulkan preparation signatures should include GPU packet ownership and debug metadata.");
		});

		registry.Add("VirtualGeometry.Batching.US4.GeneratedSubmissionsReduceDenseClusterExpansion", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<GpuVirtualGeometryPacketRecord> packets;
			for (std::uint32_t index = 0u; index < 128u; ++index)
			{
				GpuVirtualGeometryPacketRecord packet;
				packet.packetId = index;
				packet.viewId = 2u;
				packet.viewType = RenderViewType::Game;
				packet.frameId = 10004u;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = 1u;
				packet.clusterId = index;
				packet.meshletOffset = index * 4u;
				packet.meshletCount = 4u;
				packet.pageId = 0u;
				packets.push_back(packet);
			}

			const std::vector<GpuVirtualGeometryBatchDescriptor> batches =
				RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(packets, 10004u, VirtualGeometryDebugVisualizationMode::LOD);
			const std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions =
				RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords(batches, packets, 10004u);

			context.ExpectEqual(static_cast<std::size_t>(1u), batches.size(), "Dense compatible GPU packets should collapse into one batch descriptor.");
			context.ExpectEqual(static_cast<std::size_t>(1u), submissions.size(), "Dense compatible GPU packets should produce one generated submission record.");
			context.Expect(submissions.size() < packets.size() / 16u, "Generated submission count should be dramatically smaller than one record per cluster.");
			context.ExpectEqual(128u, submissions.front().packetCount, "Generated submission should preserve the full dense packet run.");
			context.Expect(submissions.front().indirectTaskCount > 0u, "Generated submission should carry mesh-task work count metadata.");
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

		registry.Add("VirtualGeometry.Batching.US1.VulkanPreparationCountsGpuPacketTables", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3011u;
			snapshot.renderables.push_back({ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID() });
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state = MakeBatchedSelectionState();
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				state.clusterPackets,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::LOD);
			state.gpuBatchDescriptors = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
				state.gpuPacketRecords,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::LOD);
			state.gpuPacketFrameSets.push_back(RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(
				state.viewType,
				state.outputId,
				snapshot.frameId,
				state.targetSize,
				VirtualGeometryDebugVisualizationMode::LOD,
				0u,
				static_cast<std::uint32_t>(state.gpuPacketRecords.size()),
				0u,
				static_cast<std::uint32_t>(state.gpuBatchDescriptors.size())));
			snapshot.virtualGeometryViewSelections.push_back(state);
			TerraEngine::Renderer::BackendCapabilityProfile capabilities;
			capabilities.supportsParallelRecording = true;
			capabilities.supportsVirtualGeometryGpuPacketBuffers = true;
			capabilities.supportsVirtualGeometryGpuCompaction = true;
			capabilities.supportsVirtualGeometryGeneratedSubmissions = true;
			capabilities.supportsVirtualGeometryCounterReadback = true;

			const TerraEngine::Renderer::Vulkan::IndirectDrawPreparationPlan plan =
				TerraEngine::Renderer::Vulkan::BuildIndirectDrawPreparationPlan(snapshot, capabilities);

			context.ExpectEqual(static_cast<std::size_t>(1u), plan.virtualGeometryGpuPacketRecordCount, "Vulkan prep should count GPU packet records.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.virtualGeometryGpuBatchDescriptorCount, "Vulkan prep should count GPU batch descriptors.");
			context.ExpectEqual(static_cast<std::size_t>(1u), plan.virtualGeometryGpuPacketFrameSetCount, "Vulkan prep should count GPU packet frame sets.");
			context.Expect(plan.gpuPacketBuffersEligible, "Vulkan prep should mark GPU packet buffers eligible when packets and capability exist.");
			context.Expect(plan.gpuCompactionEligible, "Vulkan prep should mark GPU compaction eligible from packet-buffer readiness.");
			context.Expect(plan.generatedSubmissionEligible, "Vulkan prep should mark generated submissions eligible from compaction readiness.");
			context.Expect(plan.counterReadbackEligible, "Vulkan prep should mark counter readback eligible for packet-backed views.");
		});
	}
}
