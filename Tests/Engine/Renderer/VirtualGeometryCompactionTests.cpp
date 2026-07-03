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

		registry.Add("VirtualGeometry.Compaction.US3.CandidateCompactedRejectedAndFailOpenCountersStayPerView", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> candidates;
			for (std::uint32_t index = 0u; index < 5u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.viewId = 31u;
				packet.viewType = RenderViewType::Game;
				packet.meshId = meshId;
				packet.clusterId = index;
				packet.meshletOffset = index * 4u;
				packet.meshletCount = 4u;
				packet.projectedBounds = glm::vec4(0.1f, 0.1f, 0.2f, 0.2f);
				candidates.push_back(packet);
			}
			candidates[1].projectedBounds = glm::vec4(-0.5f, 0.1f, -0.25f, 0.2f);
			candidates[2].meshletCount = 0u;
			candidates[3].fallbackPacket = true;

			std::vector<VirtualGeometryClusterPacket> compacted;
			const CompactedVisibilitySet compaction =
				RenderSystem::CompactVirtualGeometryClusterPackets(candidates, compacted, RenderViewType::Game, 31u);
			const VirtualGeometryCounterReadbackRecord counters =
				RenderSystem::BuildVirtualGeometryCounterReadbackRecord(compaction, 9900u, VirtualGeometryCounterSource::Prepared, 0u, "prepared counters");

			context.ExpectEqual(5u, counters.candidatePackets, "Counter record should preserve all candidate packets.");
			context.ExpectEqual(3u, counters.compactedPackets, "Counter record should preserve visible/fail-open compacted packets.");
			context.ExpectEqual(2u, counters.rejectedPackets, "Counter record should aggregate rejected packet domains.");
			context.ExpectEqual(1u, counters.failOpenPackets, "Counter record should preserve fail-open packet count.");
			context.ExpectEqual(31u, counters.viewId, "Counter record should stay scoped to the owning view.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryCounterSource::Prepared), static_cast<std::uint32_t>(counters.source), "Prepared compaction counters should be labeled honestly until GPU readback is available.");
		});

		registry.Add("VirtualGeometry.Compaction.US3.DispatchPlansFailOpenAndCarryHiZBaseline", [](TestContext& context) {
			GpuVirtualGeometryPacketFrameSet validFrameSet;
			validFrameSet.viewId = 42u;
			validFrameSet.viewType = RenderViewType::Scene;
			validFrameSet.frameId = 9901u;
			validFrameSet.packetCount = 8u;
			validFrameSet.batchCount = 2u;
			validFrameSet.validity = VirtualGeometryGpuFrameSetValidity::Valid;

			GpuVirtualGeometryPacketFrameSet invalidFrameSet = validFrameSet;
			invalidFrameSet.viewId = 43u;
			invalidFrameSet.validity = VirtualGeometryGpuFrameSetValidity::FailOpen;
			invalidFrameSet.fallbackReason = "overflow test frame set";

			VirtualGeometryHiZValidationState hiZ;
			hiZ.state = VirtualGeometryHiZState::Active;
			const std::vector<GpuPacketCompactionDispatch> dispatches =
				RenderSystem::BuildGpuPacketCompactionDispatches({ validFrameSet, invalidFrameSet }, hiZ, VirtualGeometryDebugVisualizationMode::HiZ, 10u);

			context.ExpectEqual(static_cast<std::size_t>(2u), dispatches.size(), "Each packet frame set should produce one compaction dispatch plan.");
			context.ExpectEqual(10u, dispatches[0].dispatchId, "Dispatch ids should honor the caller-provided base.");
			context.ExpectEqual(8u, dispatches[0].candidatePacketCount, "Dispatch should preserve candidate packet count.");
			context.ExpectEqual(2u, dispatches[0].candidateBatchCount, "Dispatch should preserve candidate batch count.");
			context.ExpectEqual(8u, dispatches[0].outputCapacity, "Dispatch output capacity should default to the packet count.");
			context.Expect(dispatches[0].failOpenOnInvalid, "Compaction dispatch should fail open if generated output is invalid.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryHiZState::Active), static_cast<std::uint32_t>(dispatches[0].hiZBaselineState), "Dispatch should carry Hi-Z baseline state without changing Hi-Z policy.");
			context.Expect(!dispatches[1].fallbackReason.empty(), "Invalid frame sets should carry a fail-open fallback reason into compaction dispatch diagnostics.");
		});

		registry.Add("VirtualGeometry.Compaction.US3.DiagnosticsInterpretCompactionCountersAndFallbackRecords", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility;
			visibility.valid = true;
			visibility.frameId = 9902u;
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 55u;

			CompactedVisibilitySet compaction;
			compaction.viewId = visibility.outputId;
			compaction.viewType = visibility.viewType;
			compaction.candidatePackets = 12u;
			compaction.visiblePackets = 7u;
			compaction.rejectedByFrustum = 2u;
			compaction.rejectedByHiZ = 1u;
			compaction.rejectedByResidency = 2u;
			compaction.failOpenPackets = 3u;
			compaction.submittedBatches = 4u;
			visibility.compactionResults.push_back(compaction);
			visibility.gpuCounterReadbacks.push_back(RenderSystem::BuildVirtualGeometryCounterReadbackRecord(compaction, visibility.frameId, VirtualGeometryCounterSource::Prepared));
			visibility.gpuFallbackRecords.push_back(RenderSystem::BuildVirtualGeometryGpuFallbackRecord(visibility.viewType, visibility.outputId, visibility.frameId, "missing page fallback", 2u, 1u, "fallback coverage preserved"));

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(12u, diagnostics.candidateClusterCount, "Diagnostics should expose candidate packet count.");
			context.ExpectEqual(7u, diagnostics.compactedClusterCount, "Diagnostics should expose compacted packet count.");
			context.ExpectEqual(5u, diagnostics.rejectedClusterCount, "Diagnostics should expose rejected packet count.");
			context.ExpectEqual(3u, diagnostics.failOpenClusterCount, "Diagnostics should expose fail-open packet count.");
			context.ExpectEqual(1u, diagnostics.gpuCounterReadbackCount, "Diagnostics should expose prepared counter records with a source label.");
			context.ExpectEqual(2u, diagnostics.gpuFallbackAffectedPackets, "Diagnostics should expose fallback-affected packet count.");
			context.Expect(diagnostics.gpuFallbackReason.find("missing page") != std::string::npos, "Diagnostics should retain the fallback reason.");
		});

		registry.Add("VirtualGeometry.Compaction.US4.GeneratedSubmissionsValidateFrameViewAndBatchCompatibility", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<GpuVirtualGeometryPacketRecord> packets;
			for (std::uint32_t index = 0u; index < 4u; ++index)
			{
				GpuVirtualGeometryPacketRecord packet;
				packet.packetId = index;
				packet.viewId = 60u;
				packet.viewType = RenderViewType::Game;
				packet.frameId = 10000u;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.meshletOffset = index * 8u;
				packet.meshletCount = 8u;
				packets.push_back(packet);
			}
			const std::vector<GpuVirtualGeometryBatchDescriptor> batches =
				RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(packets, 10000u, VirtualGeometryDebugVisualizationMode::Cluster);
			const std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions =
				RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords(batches, packets, 10000u);

			std::string validationReason;
			context.ExpectEqual(static_cast<std::size_t>(1u), submissions.size(), "One contiguous GPU packet batch should produce one generated submission.");
			context.Expect(
				RenderSystem::ValidateGeneratedVirtualGeometrySubmissionRecords(submissions, batches, 10000u, RenderViewType::Game, 60u, static_cast<std::uint32_t>(packets.size()), &validationReason),
				"Generated submissions should validate against frame, view, packet capacity, and batch compatibility.");
			context.Expect(validationReason.empty(), "Successful generated submission validation should not leave stale failure text.");
		});

		registry.Add("VirtualGeometry.Compaction.US4.InvalidGeneratedSubmissionFailsOpen", [](TestContext& context) {
			GpuVirtualGeometryBatchDescriptor batch;
			batch.batchId = 4u;
			batch.viewId = 61u;
			batch.viewType = RenderViewType::Scene;
			batch.frameId = 10001u;
			batch.packetOffset = 0u;
			batch.packetCount = 2u;
			batch.meshId = TerraEngine::GenerateUUID();
			batch.materialId = TerraEngine::GenerateUUID();
			batch.submittedMeshlets = 64u;

			std::vector<GpuVirtualGeometryPacketRecord> packets(1u);
			packets.front().viewId = batch.viewId;
			packets.front().viewType = batch.viewType;
			packets.front().frameId = batch.frameId;
			std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions =
				RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords({ batch }, packets, batch.frameId);

			std::string validationReason;
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryGeneratedSubmissionValidationState::FailOpen), static_cast<std::uint32_t>(submissions.front().validationState), "Out-of-range generated submissions should be marked fail-open at build time.");
			context.Expect(
				!RenderSystem::ValidateGeneratedVirtualGeometrySubmissionRecords(submissions, { batch }, batch.frameId, batch.viewType, batch.viewId, static_cast<std::uint32_t>(packets.size()), &validationReason),
				"Out-of-range generated submissions should fail backend validation.");
			context.Expect(!validationReason.empty(), "Invalid generated submission validation should return a fallback reason.");
		});
	}
}
