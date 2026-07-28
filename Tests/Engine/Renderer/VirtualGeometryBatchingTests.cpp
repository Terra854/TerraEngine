#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RHI/Vulkan/VulkanRendererWorkCache.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"

#include <iterator>
#include <limits>

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
		registry.Add("VirtualGeometry.Batching.DenseSelectedClustersCollapseIntoBoundedBatches", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.Batching.Regression.PageLocalPacketsShareOneDrawRunAcrossBindings", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> packets;
			// Deliberately vary every page binding that used to force one CPU Vulkan
			// submission per cluster, while retaining a contiguous page-local run.
			for (std::uint32_t index = 0u; index < 36u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = 100u + index;
				packet.viewId = 6u;
				packet.viewType = RenderViewType::Game;
				packet.objectId = objectId;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = 0u;
				packet.clusterId = index;
				packet.meshletOffset = index * 7u;
				packet.meshletCount = 7u;
				packet.pageId = 200u + index;
				packet.fallbackPageId = index;
				packet.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
				packet.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
				packet.pagePayloadByteOffset = 4096ull * index;
				packet.pagePayloadByteSize = 2048ull + index;
				packet.payloadBufferIndex = 300u + index;
				packet.payloadDescriptorIndex = 400u + index;
				packets.push_back(packet);
			}

			const std::vector<ClusterWorkBatch> cpuBatches = RenderSystem::BuildVirtualGeometryClusterBatches(packets);
			const std::vector<GpuVirtualGeometryPacketRecord> gpuPackets = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				packets,
				9101u,
				VirtualGeometryDebugVisualizationMode::None);
			const std::vector<GpuVirtualGeometryBatchDescriptor> gpuBatches = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
				gpuPackets,
				9101u,
				VirtualGeometryDebugVisualizationMode::None);

			context.ExpectEqual(static_cast<std::size_t>(1u), cpuBatches.size(), "Consecutive page-local camera clusters should collapse into one CPU draw run.");
			context.ExpectEqual(36u, cpuBatches.front().packetCount, "The CPU batch should retain every independently addressed camera packet.");
			context.ExpectEqual(252u, cpuBatches.front().submittedMeshlets, "Page-local batching should preserve the exact camera meshlet count.");
			context.ExpectEqual(static_cast<std::size_t>(1u), gpuBatches.size(), "GPU batch metadata should mirror the collapsed page-local draw run.");
			context.ExpectEqual(36u, gpuBatches.front().packetCount, "The GPU batch should expose the whole descriptor run to the task shader.");
		});

		registry.Add("VirtualGeometry.Batching.Regression.AntiqueCameraLeafCutCollapsesToOneDraw", [](TestContext& context) {
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			// These are the exact 16 leaf-cluster ranges from camera_uv.temf. Their
			// irregular sizes previously produced 16 draws when the camera entered
			// view even though they form one contiguous 267-meshlet cut.
			constexpr std::uint32_t meshletCounts[] = {
				17u, 17u, 16u, 19u, 14u, 14u, 15u, 16u,
				19u, 19u, 17u, 16u, 20u, 14u, 18u, 16u
			};
			std::vector<VirtualGeometryClusterPacket> packets;
			std::uint32_t meshletOffset = 0u;
			for (std::uint32_t clusterId = 0u; clusterId < std::size(meshletCounts); ++clusterId)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = clusterId;
				packet.viewId = 1u;
				packet.viewType = RenderViewType::Scene;
				packet.objectId = objectId;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = 0u;
				packet.clusterId = clusterId;
				packet.meshletOffset = meshletOffset;
				packet.meshletCount = meshletCounts[clusterId];
				packet.pageId = 0u;
				packet.fallbackPageId = 3u;
				packet.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
				packet.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
				packets.push_back(packet);
				meshletOffset += meshletCounts[clusterId];
			}

			const std::vector<ClusterWorkBatch> batches = RenderSystem::BuildVirtualGeometryClusterBatches(packets);

			context.ExpectEqual(static_cast<std::size_t>(1u), batches.size(), "The camera leaf cut should submit as one compatible draw run.");
			context.ExpectEqual(16u, batches.front().packetCount, "The camera draw should retain all 16 shader descriptor rows.");
			context.ExpectEqual(267u, batches.front().submittedMeshlets, "The collapsed camera draw should preserve every selected meshlet.");
		});

		registry.Add("VirtualGeometry.Batching.Regression.PagePayloadReuseIgnoresFrameIdAndInvalidatesGeometry", [](TestContext& context) {
			GpuVirtualGeometryPacketRecord packet;
			packet.packetId = 4u;
			packet.frameId = 100u;
			packet.meshId = TerraEngine::GenerateUUID();
			packet.selectedLod = 0u;
			packet.clusterId = 2u;
			packet.meshletOffset = 34u;
			packet.meshletCount = 16u;
			packet.pageId = 0u;
			packet.fallbackPageId = 3u;
			packet.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			packet.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			packet.pagePayloadByteOffset = 4096u;
			packet.pagePayloadByteSize = 2048u;
			packet.payloadBufferIndex = 8u;
			packet.payloadDescriptorIndex = 12u;

			const std::uint64_t originalSignature = TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadReuseSignature({ packet });
			packet.frameId = 103u;
			const std::uint64_t laterFrameSignature = TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadReuseSignature({ packet });
			context.ExpectEqual(originalSignature, laterFrameSignature, "Advancing a frame slot must reuse unchanged page-local geometry bytes.");
			context.Expect(
				TerraEngine::Renderer::Vulkan::CanReuseVirtualGeometryPagePayload(originalSignature, laterFrameSignature, true, true),
				"A valid live buffer with an unchanged packet signature should be reused.");

			packet.meshletCount += 1u;
			const std::uint64_t changedGeometrySignature = TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadReuseSignature({ packet });
			context.Expect(originalSignature != changedGeometrySignature, "Changing selected meshlet geometry must invalidate the retained payload.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::CanReuseVirtualGeometryPagePayload(originalSignature, changedGeometrySignature, true, true),
				"A geometry change must force page-local stream reconstruction and upload.");
			context.Expect(
				!TerraEngine::Renderer::Vulkan::CanReuseVirtualGeometryPagePayload(originalSignature, originalSignature, true, false),
				"A matching signature must not reuse a Vulkan buffer that was retired or never created.");
		});

		registry.Add("VirtualGeometry.Batching.GpuPacketRecordsMirrorCpuClusterPackets", [](TestContext& context) {
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
			packet.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			packet.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			packet.pagePayloadByteOffset = 8192u;
			packet.pagePayloadByteSize = 4096u;
			packet.payloadBufferIndex = 44u;
			packet.payloadDescriptorIndex = 12u;
			packet.debugIdentity = 1234u;
			packet.fallbackPacket = true;

			const std::vector<GpuVirtualGeometryPacketRecord> records =
				RenderSystem::BuildGpuVirtualGeometryPacketRecords(
					{ packet },
					5000u,
					VirtualGeometryDebugVisualizationMode::Cluster,
					100u);

			context.ExpectEqual(static_cast<std::size_t>(1u), records.size(), "One CPU packet should produce one GPU packet mirror.");
			context.ExpectEqual(packet.packetId, records.front().packetId, "GPU packet ids should preserve the CPU packet id used by page-local descriptors.");
			context.ExpectEqual(5000ull, records.front().frameId, "GPU packet records should carry frame ownership.");
			context.ExpectEqual(packet.clusterId, records.front().clusterId, "GPU packets should preserve selected cluster identity.");
			context.ExpectEqual(packet.meshletOffset, records.front().meshletOffset, "GPU packets should preserve meshlet offset.");
			context.ExpectEqual(packet.meshletCount, records.front().meshletCount, "GPU packets should preserve meshlet count.");
			context.Expect(records.front().payloadSource == packet.payloadSource, "GPU packets should preserve page payload source labels.");
			context.Expect(records.front().emittedSource == packet.emittedSource, "GPU packets should preserve emitted page source labels.");
			context.ExpectEqual(packet.pagePayloadByteOffset, records.front().pagePayloadByteOffset, "GPU packets should preserve page-local payload byte offsets.");
			context.ExpectEqual(packet.pagePayloadByteSize, records.front().pagePayloadByteSize, "GPU packets should preserve page-local payload byte sizes.");
			context.ExpectEqual(packet.payloadBufferIndex, records.front().payloadBufferIndex, "GPU packets should preserve physical page payload buffer bindings.");
			context.ExpectEqual(packet.payloadDescriptorIndex, records.front().payloadDescriptorIndex, "GPU packets should preserve physical page payload descriptor indices.");
			context.Expect((records.front().flags & 1u) != 0u, "GPU packet flags should mark fail-open coverage.");
			context.Expect(((records.front().flags >> 8u) & 0xffu) == static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Cluster), "GPU packet flags should mirror the view debug mode.");
		});

		registry.Add("VirtualGeometry.Batching.VulkanPageSourcePreparationUsesPageLocalRanges", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3033u;
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state = MakeBatchedSelectionState();
			state.clusterPackets.front().payloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			state.clusterPackets.front().emittedSource = VirtualGeometryEmittedPageSource::FallbackPage;
			state.clusterPackets.front().pagePayloadByteOffset = 16384u;
			state.clusterPackets.front().pagePayloadByteSize = 2048u;
			state.clusterPackets.front().payloadBufferIndex = 81u;
			state.clusterPackets.front().payloadDescriptorIndex = 2u;
			state.clusterPackets.front().fallbackPacket = true;
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				state.clusterPackets,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::Residency);
			snapshot.virtualGeometryViewSelections.push_back(state);

			const std::vector<TerraEngine::Renderer::Vulkan::VirtualGeometryPagePayloadUploadRecord> records =
				TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadUploadPlan(snapshot, RenderViewType::Game, state.outputId);

			context.ExpectEqual(static_cast<std::size_t>(1u), records.size(), "One page-local packet should produce one Vulkan payload upload record.");
			context.Expect(records.front().accepted, "Resident fallback page payloads should be accepted by upload planning.");
			context.Expect(records.front().payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload, "Upload planning should preserve fallback page payload source labels.");
			context.Expect(records.front().emittedSource == VirtualGeometryEmittedPageSource::FallbackPage, "Upload planning should preserve emitted fallback source labels.");
			context.ExpectEqual(16384ull, records.front().payloadByteOffset, "Upload planning should preserve page-local byte offset.");
			context.ExpectEqual(2048ull, records.front().payloadByteSize, "Upload planning should preserve page-local byte size.");
			context.ExpectEqual(81u, records.front().payloadBufferIndex, "Upload planning should preserve the physical page payload buffer binding.");
			context.ExpectEqual(2u, records.front().payloadDescriptorIndex, "Upload planning should preserve the physical page payload descriptor index.");
			context.Expect(records.front().pinnedFallback, "Fallback payload records should remain marked as protected.");
		});

		registry.Add("VirtualGeometry.Batching.UploadPlanningRejectsIncompletePhysicalPayloadBindings", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 3034u;
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state = MakeBatchedSelectionState();
			state.clusterPackets.front().payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			state.clusterPackets.front().emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			state.clusterPackets.front().pagePayloadByteOffset = 4096u;
			state.clusterPackets.front().pagePayloadByteSize = 1024u;
			state.clusterPackets.front().payloadBufferIndex = std::numeric_limits<std::uint32_t>::max();
			state.clusterPackets.front().payloadDescriptorIndex = std::numeric_limits<std::uint32_t>::max();
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(
				state.clusterPackets,
				snapshot.frameId,
				VirtualGeometryDebugVisualizationMode::Residency);
			snapshot.virtualGeometryViewSelections.push_back(state);

			const std::vector<TerraEngine::Renderer::Vulkan::VirtualGeometryPagePayloadUploadRecord> records =
				TerraEngine::Renderer::Vulkan::BuildVirtualGeometryPagePayloadUploadPlan(snapshot, RenderViewType::Game, state.outputId);

			context.ExpectEqual(static_cast<std::size_t>(1u), records.size(), "Incomplete resident page metadata should still produce one diagnostic upload record.");
			context.Expect(!records.front().accepted, "Resident page uploads must be rejected until a physical page descriptor binding exists.");
			context.Expect(records.front().reason.find("physical descriptor binding") != std::string::npos, "Rejected resident page uploads should explain the missing physical binding.");
		});

		registry.Add("VirtualGeometry.Batching.GpuBatchDescriptorsSplitOnCompatibilityKeys", [](TestContext& context) {
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
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryBatchFallbackMode::SinglePacketDraw), static_cast<std::uint32_t>(batches[2].fallbackMode), "Single-packet fallback batches should stay explicitly labeled.");
		});

		registry.Add("VirtualGeometry.Batching.SparsePacketsKeepExplicitSinglePacketFallback", [](TestContext& context) {
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
				context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryBatchFallbackMode::SinglePacketDraw), static_cast<std::uint32_t>(batch.fallbackMode), "Sparse packet fallback should be explicitly labeled for diagnostics.");
				context.Expect(!batch.fallbackReason.empty(), "Single-packet fallback batches should explain why they did not merge.");
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

		registry.Add("VirtualGeometry.Batching.CopiesGpuPacketTablesIntoVisibilitySnapshots", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.Batching.GpuPacketMetadataInvalidatesSnapshotAndPreparationSignatures", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.Batching.GeneratedSubmissionsReduceDenseClusterExpansion", [](TestContext& context) {
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

		registry.Add("VirtualGeometry.Batching.VulkanPreparationCountsGpuPacketTables", [](TestContext& context) {
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
