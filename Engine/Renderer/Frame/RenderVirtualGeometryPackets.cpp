// Typed virtual-geometry selectors and packet conversion preserve existing public compatibility entry points.
#include "RenderFrameState.h"
#include "RenderVirtualGeometrySelection.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometryGpuPlan.h"
#include "Renderer/VirtualGeometry/Selection/VirtualGeometrySelection.h"
#include "Renderer/VirtualGeometry/Visibility/VirtualGeometryRasterBins.h"
#include <glm/gtc/matrix_transform.hpp>
#include <bit>

bool RenderSystem::IsDirectMeshletRouteEligible(const Mesh& mesh)
{
	// Keep diagnostics on the exact production decision. A parallel terrain-only
	// approximation could pass tests while the frame router still rejects draws.
	return BuildCanonicalGeometryRouteFacts(mesh).directUsable;
}

std::vector<VirtualGeometryClusterPacket> RenderSystem::BuildVirtualGeometryClusterPackets(
	const VirtualGeometryAssetRuntime& asset,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	const TerraEngine::UUID& materialId,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint32_t packetIdBase)
{
	std::vector<VirtualGeometryClusterPacket> packets;
	packets.reserve(selectedClusters.size());
	for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selectedCluster : selectedClusters)
	{
		const VirtualGeometryClusterRuntime* cluster = FindVirtualGeometryClusterById(asset, selectedCluster.clusterId);
		if (cluster == nullptr)
		{
			continue;
		}

		const VirtualGeometryClusterMeshletRange meshlets = ResolveVirtualGeometryClusterMeshletRange(asset, *cluster);
		if (!meshlets.valid)
		{
			continue;
		}

		VirtualGeometryClusterPacket packet;
		packet.packetId = packetIdBase + static_cast<std::uint32_t>(packets.size());
		packet.viewId = viewId;
		packet.viewType = viewType;
		packet.objectId = selectedCluster.objectId;
		packet.meshId = selectedCluster.meshId;
		packet.materialId = materialId;
		packet.selectedLod = cluster->lodIndex;
		packet.clusterId = cluster->clusterId;
		packet.meshletOffset = meshlets.meshletOffset;
		packet.meshletCount = meshlets.meshletCount;
		const auto [boundsMin, boundsMax] = ResolveVirtualGeometryHiZPacketBounds(*cluster);
		packet.boundsMin = boundsMin;
		packet.boundsMax = boundsMax;
		packet.pageId = selectedCluster.pageId;
		packet.fallbackPacket = selectedCluster.usingFallback;
		const GeometryPageRuntime* page = FindGeometryPageById(asset, cluster->pageId);
		packet.fallbackPageId = page != nullptr ? page->fallbackPageId : (selectedCluster.usingFallback ? cluster->pageId : 0u);
		packet.payloadSource = ResolveVirtualGeometryPayloadSource(page, packet.fallbackPacket);
		packet.emittedSource = ResolveVirtualGeometryEmittedSource(packet.payloadSource);
		packet.pagePayloadByteOffset = page != nullptr ? page->payloadByteOffset : 0u;
		packet.pagePayloadByteSize = ResolveVirtualGeometryPayloadBytes(page, meshlets);
		packet.payloadBufferIndex = page != nullptr ? page->payloadBufferIndex : 0xffffffffu;
		packet.payloadDescriptorIndex = page != nullptr ? page->payloadDescriptorIndex : 0xffffffffu;

		// The debug identity is deliberately stable and source-data based. It
		// lets visualizers distinguish packet/batch ownership without depending
		// on pointer addresses or frame-local vector storage.
		std::uint64_t debugHash = kRenderHashOffset;
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.objectId)));
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.meshId)));
		HashCombine(debugHash, static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(packet.materialId)));
		HashCombine(debugHash, packet.clusterId);
		HashCombine(debugHash, packet.selectedLod);
		HashCombine(debugHash, packet.meshletOffset);
		packet.debugIdentity = static_cast<std::uint32_t>(debugHash ^ (debugHash >> 32u));
		packets.push_back(packet);
	}
	return packets;
}

std::vector<ClusterWorkBatch> RenderSystem::BuildVirtualGeometryClusterBatches(
	const std::vector<VirtualGeometryClusterPacket>& packets,
	std::uint32_t packetOffsetBase,
	std::uint32_t batchIdBase)
{
	std::vector<ClusterWorkBatch> batches;
	if (packets.empty())
	{
		return batches;
	}

	auto canMergePacket = [](const VirtualGeometryClusterPacket& previous, const VirtualGeometryClusterPacket& next) {
		const auto isPageLocalSource = [](VirtualGeometryPagePayloadSource source) {
			return source == VirtualGeometryPagePayloadSource::ResidentPagePayload
				|| source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		};
		const bool pageLocalRun = isPageLocalSource(previous.payloadSource)
			&& isPageLocalSource(next.payloadSource);
		const bool compatibleOwner =
			previous.viewId == next.viewId
			&& previous.viewType == next.viewType
			&& previous.objectId == next.objectId
			&& previous.meshId == next.meshId
			&& previous.materialId == next.materialId
			&& previous.selectedLod == next.selectedLod
			&& previous.payloadSource == next.payloadSource
			&& previous.emittedSource == next.emittedSource
			&& previous.fallbackPacket == next.fallbackPacket
			// Raster route and shading-bin policy are protected-boundary
			// execution state. A CPU-expanded batch must not span a route
			// transition that the GPU descriptor path intentionally splits.
			&& previous.rasterRoute == next.rasterRoute
			&& previous.materialBin == next.materialBin
			&& previous.packetId + 1u == next.packetId;
		// Embedded draws still share one payload binding for their entire run.
		// Page-local draws carry one descriptor id per emitted meshlet, so adjacent
		// packets may safely cross page boundaries without multiplying CPU draws.
		const bool compatiblePayload = pageLocalRun
			|| (previous.pageId == next.pageId
				&& previous.fallbackPageId == next.fallbackPageId
				&& previous.pagePayloadByteOffset == next.pagePayloadByteOffset
				&& previous.pagePayloadByteSize == next.pagePayloadByteSize
				&& previous.payloadBufferIndex == next.payloadBufferIndex
				&& previous.payloadDescriptorIndex == next.payloadDescriptorIndex);
		const bool contiguousMeshlets = previous.meshletOffset + previous.meshletCount == next.meshletOffset;
		return compatibleOwner && compatiblePayload && contiguousMeshlets;
	};

	auto startBatch = [&](std::size_t packetIndex) {
		const VirtualGeometryClusterPacket& packet = packets[packetIndex];
		ClusterWorkBatch batch;
		batch.batchId = batchIdBase + static_cast<std::uint32_t>(batches.size());
		batch.viewId = packet.viewId;
		batch.viewType = packet.viewType;
		batch.meshId = packet.meshId;
		batch.materialId = packet.materialId;
		batch.packetOffset = packetOffsetBase + static_cast<std::uint32_t>(packetIndex);
		batch.packetCount = 1u;
		batch.selectedMeshlets = packet.meshletCount;
		batch.submittedMeshlets = packet.meshletCount;
		batch.batchReason = "single packet started a compatible batch";
		return batch;
	};

	batches.push_back(startBatch(0u));
	for (std::size_t packetIndex = 1u; packetIndex < packets.size(); ++packetIndex)
	{
		ClusterWorkBatch& currentBatch = batches.back();
		const VirtualGeometryClusterPacket& previousPacket = packets[packetIndex - 1u];
		const VirtualGeometryClusterPacket& packet = packets[packetIndex];
		if (canMergePacket(previousPacket, packet))
		{
			++currentBatch.packetCount;
			currentBatch.selectedMeshlets += packet.meshletCount;
			currentBatch.submittedMeshlets += packet.meshletCount;
			currentBatch.batchReason = "contiguous meshlets share view, mesh, material, LOD, page, and fallback state";
			continue;
		}
		batches.push_back(startBatch(packetIndex));
	}

	for (ClusterWorkBatch& batch : batches)
	{
		if (batch.packetCount == 1u)
		{
			batch.fallbackMode = VirtualGeometryBatchFallbackMode::SinglePacketDraw;
			batch.fallbackReason = "Packet could not merge with a compatible contiguous neighbor, so it keeps the exact single-packet draw.";
		}
	}
	return batches;
}

std::vector<GpuVirtualGeometryPacketRecord> RenderSystem::BuildGpuVirtualGeometryPacketRecords(
	const std::vector<VirtualGeometryClusterPacket>& packets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t packetIdBase)
{
	(void)packetIdBase;
	std::vector<GpuVirtualGeometryPacketRecord> records;
	records.reserve(packets.size());
	for (const VirtualGeometryClusterPacket& packet : packets)
	{
		GpuVirtualGeometryPacketRecord record;
		// Page-local geometry payloads are keyed by the stable CPU packet id.
		// Re-numbering compacted GPU records here would make the task/mesh
		// shader decode a different packet's stream after culling removes a
		// preceding packet from the submitted set.
		record.packetId = packet.packetId;
		record.viewId = packet.viewId;
		record.viewType = packet.viewType;
		record.frameId = frameId;
		record.objectId = packet.objectId;
		record.renderableIndex = packet.renderableIndex;
		record.meshId = packet.meshId;
		record.materialId = packet.materialId;
		record.selectedLod = packet.selectedLod;
		record.clusterId = packet.clusterId;
		record.meshletOffset = packet.meshletOffset;
		record.meshletCount = packet.meshletCount;
		record.boundsMin = packet.boundsMin;
		record.boundsMax = packet.boundsMax;
		record.projectedBounds = packet.projectedBounds;
		record.pageId = packet.pageId;
		record.fallbackPageId = packet.fallbackPageId;
		record.payloadSource = packet.payloadSource;
		record.emittedSource = packet.emittedSource;
		record.pagePayloadByteOffset = packet.pagePayloadByteOffset;
		record.pagePayloadByteSize = packet.pagePayloadByteSize;
		record.payloadBufferIndex = packet.payloadBufferIndex;
		record.payloadDescriptorIndex = packet.payloadDescriptorIndex;
		record.fallbackReason = packet.fallbackReason;
		record.debugIdentity = packet.debugIdentity;
		// Bit 0 marks fail-open coverage; higher bits mirror the active debug
		// mode so generated submissions can reject stale visualizer ownership.
		record.flags = (packet.fallbackPacket ? 1u : 0u)
			| (static_cast<std::uint32_t>(NormalizeVirtualGeometryDebugVisualizationMode(debugMode)) << 8u)
			| ((packet.rasterRoute & kVirtualGeometryRasterRouteMask)
				<< kVirtualGeometryRasterRouteShift)
			| ((packet.materialBin & kVirtualGeometryMaterialBinMask)
				<< kVirtualGeometryMaterialBinShift);
		records.push_back(record);
	}
	return records;
}

std::vector<GpuVirtualGeometryBatchDescriptor> RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
	const std::vector<GpuVirtualGeometryPacketRecord>& packets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t batchIdBase)
{
	std::vector<GpuVirtualGeometryBatchDescriptor> batches;
	if (packets.empty())
	{
		return batches;
	}

	auto canMergePacket = [](const GpuVirtualGeometryPacketRecord& previous, const GpuVirtualGeometryPacketRecord& next) {
		const auto isPageLocalSource = [](VirtualGeometryPagePayloadSource source) {
			return source == VirtualGeometryPagePayloadSource::ResidentPagePayload
				|| source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		};
		const bool pageLocalRun = isPageLocalSource(previous.payloadSource)
			&& isPageLocalSource(next.payloadSource);
		const bool compatibleOwner = previous.viewId == next.viewId
			&& previous.viewType == next.viewType
			&& previous.objectId == next.objectId
			&& previous.meshId == next.meshId
			&& previous.materialId == next.materialId
			&& previous.selectedLod == next.selectedLod
			&& previous.payloadSource == next.payloadSource
			&& previous.emittedSource == next.emittedSource
			&& previous.flags == next.flags
			&& previous.packetId + 1u == next.packetId;
		// Match the CPU batching contract. Page-local packet rows own their stream
		// bindings independently; non-page-local runs still require one binding.
		const bool compatiblePayload = pageLocalRun
			|| (previous.pageId == next.pageId
				&& previous.fallbackPageId == next.fallbackPageId
				&& previous.pagePayloadByteOffset == next.pagePayloadByteOffset
				&& previous.pagePayloadByteSize == next.pagePayloadByteSize
				&& previous.payloadBufferIndex == next.payloadBufferIndex
				&& previous.payloadDescriptorIndex == next.payloadDescriptorIndex);
		return compatibleOwner
			&& compatiblePayload
			&& previous.meshletOffset + previous.meshletCount == next.meshletOffset;
	};

	auto startBatch = [&](std::size_t packetIndex) {
		const GpuVirtualGeometryPacketRecord& packet = packets[packetIndex];
		GpuVirtualGeometryBatchDescriptor batch;
		batch.batchId = batchIdBase + static_cast<std::uint32_t>(batches.size());
		batch.viewId = packet.viewId;
		batch.viewType = packet.viewType;
		batch.frameId = frameId;
		batch.packetOffset = static_cast<std::uint32_t>(packetIndex);
		batch.packetCount = 1u;
		batch.meshId = packet.meshId;
		batch.materialId = packet.materialId;
		batch.selectedMeshlets = packet.meshletCount;
		batch.submittedMeshlets = packet.meshletCount;
		batch.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
		batch.flags = packet.flags;
		return batch;
	};

	batches.push_back(startBatch(0u));
	for (std::size_t packetIndex = 1u; packetIndex < packets.size(); ++packetIndex)
	{
		GpuVirtualGeometryBatchDescriptor& currentBatch = batches.back();
		if (canMergePacket(packets[packetIndex - 1u], packets[packetIndex]))
		{
			++currentBatch.packetCount;
			currentBatch.selectedMeshlets += packets[packetIndex].meshletCount;
			currentBatch.submittedMeshlets += packets[packetIndex].meshletCount;
			continue;
		}
		batches.push_back(startBatch(packetIndex));
	}

	for (GpuVirtualGeometryBatchDescriptor& batch : batches)
	{
		if (batch.packetCount == 1u)
		{
			batch.fallbackMode = VirtualGeometryBatchFallbackMode::SinglePacketDraw;
		}
	}
	return batches;
}

GpuVirtualGeometryPacketFrameSet RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId,
	const glm::uvec2& targetSize,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t packetOffset,
	std::uint32_t packetCount,
	std::uint32_t batchOffset,
	std::uint32_t batchCount)
{
	GpuVirtualGeometryPacketFrameSet frameSet;
	frameSet.viewType = viewType;
	frameSet.viewId = viewId;
	frameSet.frameId = frameId;
	frameSet.targetSize = targetSize;
	frameSet.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
	frameSet.packetOffset = packetOffset;
	frameSet.packetCount = packetCount;
	frameSet.batchOffset = batchOffset;
	frameSet.batchCount = batchCount;
	frameSet.compactedPacketOffset = packetOffset;
	frameSet.compactedPacketCount = packetCount;
	frameSet.generatedSubmissionOffset = batchOffset;
	frameSet.generatedSubmissionCount = batchCount;
	frameSet.validity = packetCount == 0u
		? VirtualGeometryGpuFrameSetValidity::Delayed
		: VirtualGeometryGpuFrameSetValidity::Valid;
	frameSet.fallbackReason = packetCount == 0u ? "No virtual geometry packets were available for this view." : std::string();
	return frameSet;
}

bool RenderSystem::ValidateGpuVirtualGeometryPacketRecords(
	const std::vector<VirtualGeometryClusterPacket>& cpuPackets,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	const std::vector<GpuVirtualGeometryPacketRecord>& gpuPackets,
	std::uint64_t frameId,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::string* outReason)
{
	auto reject = [outReason](std::string reason) {
		if (outReason != nullptr)
		{
			*outReason = std::move(reason);
		}
		return false;
	};

	if (cpuPackets.size() != gpuPackets.size())
	{
		return reject("GPU packet mirror count does not match the CPU-selected cluster packet count.");
	}
	const std::uint32_t expectedDebugBits =
		static_cast<std::uint32_t>(NormalizeVirtualGeometryDebugVisualizationMode(debugMode)) << 8u;
	for (std::size_t index = 0u; index < cpuPackets.size(); ++index)
	{
		const VirtualGeometryClusterPacket& cpu = cpuPackets[index];
		const GpuVirtualGeometryPacketRecord& gpu = gpuPackets[index];
		if (gpu.frameId != frameId)
		{
			return reject("GPU packet frame ownership does not match the frame being published.");
		}
		if (gpu.packetId != cpu.packetId)
		{
			return reject("GPU packet id does not match the CPU packet id used by page-local descriptors.");
		}
		if (gpu.viewId != cpu.viewId || gpu.viewType != cpu.viewType)
		{
			return reject("GPU packet view ownership does not match the CPU cluster packet.");
		}
		if (gpu.objectId != cpu.objectId || gpu.meshId != cpu.meshId || gpu.materialId != cpu.materialId)
		{
			return reject("GPU packet object, mesh, or material ownership does not match the CPU packet.");
		}
		if (gpu.selectedLod != cpu.selectedLod || gpu.clusterId != cpu.clusterId)
		{
			return reject("GPU packet selected LOD or cluster identity does not match the CPU packet.");
		}
		if (gpu.meshletOffset != cpu.meshletOffset || gpu.meshletCount != cpu.meshletCount)
		{
			return reject("GPU packet meshlet range does not match the CPU packet range.");
		}
		if (gpu.pageId != cpu.pageId || gpu.fallbackPageId != cpu.fallbackPageId)
		{
			return reject("GPU packet page or fallback-page identity does not match the CPU packet.");
		}
		if (gpu.payloadSource != cpu.payloadSource || gpu.emittedSource != cpu.emittedSource)
		{
			return reject("GPU packet page source labels do not match the CPU packet.");
		}
		if (gpu.pagePayloadByteOffset != cpu.pagePayloadByteOffset || gpu.pagePayloadByteSize != cpu.pagePayloadByteSize)
		{
			return reject("GPU packet page-local payload byte range does not match the CPU packet.");
		}
		if (gpu.payloadBufferIndex != cpu.payloadBufferIndex || gpu.payloadDescriptorIndex != cpu.payloadDescriptorIndex)
		{
			return reject("GPU packet physical page payload binding does not match the CPU packet.");
		}
		const bool expectedFallback = cpu.fallbackPacket;
		const bool gpuFallback = (gpu.flags & 1u) != 0u;
		if (gpuFallback != expectedFallback)
		{
			return reject("GPU packet fallback flag does not match the CPU packet fallback state.");
		}
		if ((gpu.flags & (kVirtualGeometryDebugModeMask << kVirtualGeometryDebugModeShift)) != expectedDebugBits)
		{
			return reject("GPU packet debug-mode ownership is stale for the current view.");
		}
		if (!selectedClusters.empty())
		{
			const auto selectedIt = std::find_if(
				selectedClusters.begin(),
				selectedClusters.end(),
				[&cpu](const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selected) {
					return selected.objectId == cpu.objectId
						&& selected.meshId == cpu.meshId
						&& selected.clusterId == cpu.clusterId;
				});
			if (selectedIt == selectedClusters.end())
			{
				return reject("GPU packet does not preserve the CPU-selected cluster cut identity.");
			}
			if (selectedIt->lodIndex != cpu.selectedLod || selectedIt->pageId != cpu.pageId)
			{
				return reject("GPU packet does not preserve selected cluster LOD or page ownership.");
			}
			if (selectedIt->usingFallback != expectedFallback)
			{
				return reject("GPU packet fallback state does not match selected cluster fallback coverage.");
			}
		}
	}

	if (outReason != nullptr)
	{
		outReason->clear();
	}
	return true;
}

std::vector<GpuPacketCompactionDispatch> RenderSystem::BuildGpuPacketCompactionDispatches(
	const std::vector<GpuVirtualGeometryPacketFrameSet>& frameSets,
	const VirtualGeometryHiZValidationState& hiZState,
	VirtualGeometryDebugVisualizationMode debugMode,
	std::uint32_t dispatchIdBase)
{
	std::vector<GpuPacketCompactionDispatch> dispatches;
	dispatches.reserve(frameSets.size());
	for (const GpuVirtualGeometryPacketFrameSet& frameSet : frameSets)
	{
		GpuPacketCompactionDispatch dispatch;
		dispatch.dispatchId = dispatchIdBase + static_cast<std::uint32_t>(dispatches.size());
		dispatch.viewId = frameSet.viewId;
		dispatch.viewType = frameSet.viewType;
		dispatch.frameId = frameSet.frameId;
		dispatch.candidatePacketCount = frameSet.packetCount;
		dispatch.candidateBatchCount = frameSet.batchCount;
		dispatch.outputCapacity = frameSet.packetCount;
		dispatch.hiZBaselineState = hiZState.state;
		dispatch.debugMode = NormalizeVirtualGeometryDebugVisualizationMode(debugMode);
		dispatch.failOpenOnInvalid = true;
		if (frameSet.validity != VirtualGeometryGpuFrameSetValidity::Valid)
		{
			dispatch.fallbackReason = frameSet.fallbackReason.empty()
				? "GPU packet frame set is not valid, so compaction must fail open to CPU-selected coverage."
				: frameSet.fallbackReason;
		}
		dispatches.push_back(std::move(dispatch));
	}
	return dispatches;
}

std::vector<GeneratedVirtualGeometrySubmissionRecord> RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords(
	const std::vector<GpuVirtualGeometryBatchDescriptor>& batches,
	const std::vector<GpuVirtualGeometryPacketRecord>& packets,
	std::uint64_t frameId,
	std::uint32_t recordIdBase)
{
	std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions;
	submissions.reserve(batches.size());
	for (const GpuVirtualGeometryBatchDescriptor& batch : batches)
	{
		GeneratedVirtualGeometrySubmissionRecord submission;
		submission.recordId = recordIdBase + static_cast<std::uint32_t>(submissions.size());
		submission.viewId = batch.viewId;
		submission.viewType = batch.viewType;
		submission.frameId = frameId;
		submission.batchId = batch.batchId;
		submission.packetOffset = batch.packetOffset;
		submission.packetCount = batch.packetCount;
		submission.indirectTaskCount = batch.submittedMeshlets == 0u ? 0u : std::max(1u, (batch.submittedMeshlets + 127u) / 128u);
		submission.materialId = batch.materialId;
		submission.meshId = batch.meshId;
		submission.counterOffset = submission.recordId;
		submission.source = VirtualGeometryCounterSource::Prepared;

		const bool rangeValid = batch.packetCount > 0u
			&& batch.packetOffset <= packets.size()
			&& static_cast<std::size_t>(batch.packetOffset) + static_cast<std::size_t>(batch.packetCount) <= packets.size();
		if (!rangeValid)
		{
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::FailOpen;
			submission.fallbackReason = "Generated submission batch references packets outside the compacted packet table.";
		}
		submissions.push_back(std::move(submission));
	}
	return submissions;
}

bool RenderSystem::ValidateGeneratedVirtualGeometrySubmissionRecords(
	const std::vector<GeneratedVirtualGeometrySubmissionRecord>& submissions,
	const std::vector<GpuVirtualGeometryBatchDescriptor>& batches,
	std::uint64_t frameId,
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint32_t packetCapacity,
	std::string* outReason)
{
	auto reject = [outReason](std::string reason) {
		if (outReason != nullptr)
		{
			*outReason = std::move(reason);
		}
		return false;
	};

	if (submissions.empty())
	{
		if (packetCapacity == 0u)
		{
			if (outReason != nullptr)
			{
				outReason->clear();
			}
			return true;
		}
		return reject("No generated submissions were available for non-empty packet work.");
	}
	if (submissions.size() > batches.size())
	{
		return reject("Generated submissions outnumber compatible batch descriptors.");
	}

	for (const GeneratedVirtualGeometrySubmissionRecord& submission : submissions)
	{
		if (submission.frameId != frameId || submission.viewType != viewType || submission.viewId != viewId)
		{
			return reject("Generated submission frame or view ownership is stale.");
		}
		if (submission.validationState != VirtualGeometryGeneratedSubmissionValidationState::Valid)
		{
			return reject(submission.fallbackReason.empty()
				? "Generated submission was already marked invalid."
				: submission.fallbackReason);
		}
		if (submission.packetCount == 0u || submission.indirectTaskCount == 0u)
		{
			return reject("Generated submission contains no packet or task work.");
		}
		if (submission.packetOffset > packetCapacity
			|| submission.packetOffset + submission.packetCount > packetCapacity)
		{
			return reject("Generated submission packet range exceeds compacted packet capacity.");
		}
		const auto batchIt = std::find_if(
			batches.begin(),
			batches.end(),
			[&submission](const GpuVirtualGeometryBatchDescriptor& batch) {
				return batch.batchId == submission.batchId
					&& batch.packetOffset == submission.packetOffset
					&& batch.packetCount == submission.packetCount
					&& batch.meshId == submission.meshId
					&& batch.materialId == submission.materialId;
			});
		if (batchIt == batches.end())
		{
			return reject("Generated submission no longer matches a compatible GPU batch descriptor.");
		}
	}

	if (outReason != nullptr)
	{
		outReason->clear();
	}
	return true;
}

VirtualGeometryCounterReadbackRecord RenderSystem::BuildVirtualGeometryCounterReadbackRecord(
	const CompactedVisibilitySet& compaction,
	std::uint64_t frameId,
	VirtualGeometryCounterSource source,
	std::uint32_t latencyFrames,
	std::string unavailableReason)
{
	VirtualGeometryCounterReadbackRecord readback;
	readback.viewId = compaction.viewId;
	readback.viewType = compaction.viewType;
	readback.frameId = frameId;
	readback.source = source;
	readback.latencyFrames = latencyFrames;
	readback.candidatePackets = compaction.candidatePackets;
	readback.compactedPackets = compaction.visiblePackets;
	readback.submittedBatches = compaction.submittedBatches;
	readback.rejectedPackets = compaction.rejectedByFrustum
		+ compaction.rejectedByHiZ
		+ compaction.rejectedByResidency
		+ compaction.rejectedByCone;
	readback.failOpenPackets = compaction.failOpenPackets;
	readback.unavailableReason = std::move(unavailableReason);
	return readback;
}

VirtualGeometryGpuFallbackRecord RenderSystem::BuildVirtualGeometryGpuFallbackRecord(
	RenderViewType viewType,
	std::uint32_t viewId,
	std::uint64_t frameId,
	std::string reason,
	std::uint32_t affectedPackets,
	std::uint32_t affectedBatches,
	std::string diagnostic)
{
	VirtualGeometryGpuFallbackRecord fallback;
	fallback.viewType = viewType;
	fallback.viewId = viewId;
	fallback.frameId = frameId;
	fallback.reason = std::move(reason);
	fallback.affectedPackets = affectedPackets;
	fallback.affectedBatches = affectedBatches;
	fallback.coveragePolicy = "FailOpenCoverage";
	fallback.diagnostic = std::move(diagnostic);
	return fallback;
}

bool RenderSystem::AreVirtualGeometryClustersConeCullingEligible(
	const VirtualGeometryAssetRuntime& asset,
	const std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord>& selectedClusters,
	std::string* outDisabledReason)
{
	if (outDisabledReason != nullptr)
	{
		outDisabledReason->clear();
	}
	if (selectedClusters.empty())
	{
		if (outDisabledReason != nullptr)
		{
			*outDisabledReason = "No selected virtual-geometry clusters were available for cone safety validation.";
		}
		return false;
	}

	for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& selectedCluster : selectedClusters)
	{
		const auto metadata = std::find_if(
			asset.coneSafety.begin(),
			asset.coneSafety.end(),
			[&selectedCluster](const ConeSafetyMetadata& candidate) {
				return candidate.clusterId == selectedCluster.clusterId;
			});
		if (metadata == asset.coneSafety.end())
		{
			if (outDisabledReason != nullptr)
			{
				*outDisabledReason = "Selected cluster is missing cone safety metadata.";
			}
			return false;
		}

		const ConeSafetyMetadata normalized = NormalizeVirtualGeometryConeSafetyMetadata(*metadata);
		if (!normalized.eligible)
		{
			if (outDisabledReason != nullptr)
			{
				*outDisabledReason = normalized.disabledReason.empty()
					? "Selected cluster is not eligible for cone culling."
					: normalized.disabledReason;
			}
			return false;
		}
	}
	return true;
}

CompactedVisibilitySet RenderSystem::CompactVirtualGeometryClusterPackets(
	const std::vector<VirtualGeometryClusterPacket>& candidatePackets,
	std::vector<VirtualGeometryClusterPacket>& compactedPackets,
	RenderViewType viewType,
	std::uint32_t viewId,
	bool failOpenForMissingResourcePackets)
{
	compactedPackets.clear();
	compactedPackets.reserve(candidatePackets.size());
	CompactedVisibilitySet result;
	result.viewId = viewId;
	result.viewType = viewType;
	result.candidatePackets = static_cast<std::uint32_t>(candidatePackets.size());
	result.source = VirtualGeometryCounterSource::Prepared;

	auto projectedBoundsProvided = [](const VirtualGeometryClusterPacket& packet) {
		return packet.projectedBounds != glm::vec4(0.0f);
	};
	auto projectedBoundsOutsideView = [](const VirtualGeometryClusterPacket& packet) {
		const float minX = std::min(packet.projectedBounds.x, packet.projectedBounds.z);
		const float maxX = std::max(packet.projectedBounds.x, packet.projectedBounds.z);
		const float minY = std::min(packet.projectedBounds.y, packet.projectedBounds.w);
		const float maxY = std::max(packet.projectedBounds.y, packet.projectedBounds.w);
		return maxX < 0.0f || maxY < 0.0f || minX > 1.0f || minY > 1.0f;
	};

	for (const VirtualGeometryClusterPacket& packet : candidatePackets)
	{
		if (packet.meshletCount == 0u)
		{
			++result.rejectedByResidency;
			continue;
		}
		if (projectedBoundsProvided(packet) && projectedBoundsOutsideView(packet))
		{
			++result.rejectedByFrustum;
			continue;
		}

		// Hi-Z remains shader-owned in this milestone. CPU compaction only
		// carries the current packet set forward so the established task shader
		// policy can cull or fail open exactly as before.
		if (packet.fallbackPacket)
		{
			++result.failOpenPackets;
		}
		compactedPackets.push_back(packet);
	}

	if (compactedPackets.empty()
		&& failOpenForMissingResourcePackets
		&& result.rejectedByResidency == result.candidatePackets
		&& result.candidatePackets > 0u)
	{
		compactedPackets = candidatePackets;
		result.failOpenPackets = result.candidatePackets;
		result.rejectedByResidency = 0u;
	}

	result.visiblePackets = static_cast<std::uint32_t>(compactedPackets.size());
	return result;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range{};
	if (asset.detailLevels.empty())
	{
		range.failOpen = true;
		range.fallbackReason = "missing virtual geometry detail metadata";
		return range;
	}

	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	const auto ApplyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	ApplyDetail(0u);
	if (!taskShaderCullingEnabled)
	{
		range.failOpen = true;
		range.fallbackReason = "task shader culling disabled; using highest-detail fallback";
		return range;
	}
	if (!boundsValid)
	{
		range.failOpen = true;
		range.fallbackReason = "invalid meshlet bounds; using highest-detail fallback";
		return range;
	}

	range.virtualGeometryActive = true;
	range.hiZSelfOcclusionGuardPassed = true;
	range.hiZNearPlaneGuardPassed = true;
	range.hiZThinGeometryGuardPassed = true;
	std::size_t desiredIndex = 0u;
	const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
		? std::max(projectedRadiusPixels, 0.0f)
		: 512.0f;
	// Mesh LOD Scale is documented as preserving detail for longer when raised.
	// Apply it to the whole-mesh screen-size bands as well as cluster error so
	// fallback and sparse-cluster paths obey the same project quality control.
	const float scaledProjectedRadiusPixels =
		finiteProjectedRadiusPixels * sanitized.lodErrorScale;
	constexpr float kFirstVirtualGeometryLodTransitionPixels = 480.0f;
	constexpr float kVirtualGeometryLodTransitionScale = 0.5f;
	// Walk every cooked detail level instead of using two fixed buckets. Each
	// lower LOD becomes valid when the mesh covers roughly half as much screen
	// radius as the previous transition, which makes LOD visualizer colors move
	// at ordinary editor camera distances while still keeping nearby meshes sharp.
	for (std::size_t detailIndex = 1u; detailIndex < asset.detailLevels.size(); ++detailIndex)
	{
		const float transitionPixels = kFirstVirtualGeometryLodTransitionPixels
			* std::pow(kVirtualGeometryLodTransitionScale, static_cast<float>(detailIndex - 1u));
		if (scaledProjectedRadiusPixels >= transitionPixels)
		{
			break;
		}
		desiredIndex = detailIndex;
	}

	if (sanitized.maxLodDrop > 0u)
	{
		desiredIndex = std::min<std::size_t>(desiredIndex, sanitized.maxLodDrop);
	}

	while (desiredIndex > 0u)
	{
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[desiredIndex];
		const float scaledError = detail.geometricError * sanitized.lodErrorScale;
		if (std::isfinite(scaledError) && scaledError <= 0.1f)
		{
			break;
		}
		--desiredIndex;
	}

	ApplyDetail(desiredIndex);
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectDirectResidentLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels)
{
	// Reuse the same artist-facing screen-size and error policy as paged VG, then
	// remove every virtual-only state bit. Direct LODs are ordinary ranges inside
	// an already-resident owner mesh and must never create hierarchy or streaming
	// work merely because they share the detail-record structure.
	TerraEngine::Renderer::VirtualGeometryDrawRange range =
		SelectVirtualGeometryLodRange(
			asset,
			quality,
			projectedRadiusPixels,
			true,
			true);
	range.virtualGeometryActive = false;
	range.failOpen = false;
	range.fallbackReason.clear();
	range.hiZSelfOcclusionGuardPassed = false;
	range.hiZNearPlaneGuardPassed = false;
	range.hiZThinGeometryGuardPassed = false;
	range.hiZInactiveReason.clear();
	return range;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryResidentLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryLodRange(
		asset,
		quality,
		projectedRadiusPixels,
		taskShaderCullingEnabled,
		boundsValid);
	const auto applyDetail = [&asset, &range](std::size_t detailIndex) {
		const VirtualGeometryDetailRuntime& detail = asset.detailLevels[detailIndex];
		range.selectedLodIndex = detail.lodIndex;
		range.meshletOffset = detail.meshletOffset;
		range.meshletCount = detail.meshletCount;
	};

	if (range.failOpen || !range.virtualGeometryActive || asset.pages.empty() || asset.detailLevels.empty())
	{
		return range;
	}

	const std::size_t desiredIndex = DetailIndexForLod(asset, range.selectedLodIndex);
	std::string fallbackReason;
	const std::size_t residentIndex = ResolveResidentVirtualGeometryDetailIndex(asset, desiredIndex, fallbackReason);
	if (residentIndex != desiredIndex)
	{
		applyDetail(residentIndex);
		range.failOpen = true;
		range.fallbackReason = fallbackReason;
	}
	return range;
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	return SelectVirtualGeometryClusterCut(
		asset,
		meshId,
		objectId,
		quality,
		projectedRadiusPixels,
		glm::mat4(1.0f),
		glm::vec3(0.0f),
		glm::uvec2(0u),
		taskShaderCullingEnabled,
		boundsValid);
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	const glm::mat4& modelMatrix,
	const glm::vec3& cameraPosition,
	const glm::uvec2& targetSize,
	bool taskShaderCullingEnabled,
	bool boundsValid)
{
	TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result;
	result.range = SelectVirtualGeometryResidentLodRange(
		asset,
		quality,
		projectedRadiusPixels,
		taskShaderCullingEnabled,
		boundsValid);
	if (!result.range.virtualGeometryActive || asset.clusters.empty())
	{
		return result;
	}
	if (!asset.clusterCoverageValidated)
	{
		if (!asset.detailLevels.empty())
		{
			const VirtualGeometryDetailRuntime& safestDetail = asset.detailLevels.front();
			result.range.selectedLodIndex = safestDetail.lodIndex;
			result.range.meshletOffset = safestDetail.meshletOffset;
			result.range.meshletCount = safestDetail.meshletCount;
		}
		result.range.failOpen = true;
		result.range.fallbackReason = "cluster coverage validation missing; using complete highest-detail range";
		result.range.selectedClusterOffset = 0u;
		result.range.selectedClusterCount = 0u;
		result.range.pageNeedOffset = 0u;
		result.range.pageNeedCount = 0u;
		result.invalidSelectionReasons |= 8u;
		return result;
	}

	// Prefer the least-error complete aggregate profile that can answer every
	// missing-page fallback through one existing cluster. This is a typed
	// coverage proof, not permission to pick an unrelated resident cluster.
	const VirtualGeometryAssetRuntime::AggregateProfileRuntime* aggregateFallback = nullptr;
	for (const VirtualGeometryAssetRuntime::AggregateProfileRuntime& profile : asset.aggregateProfiles)
	{
		if (!profile.Valid()
			|| FindVirtualGeometryClusterById(asset, profile.fallbackClusterId) == nullptr)
		{
			continue;
		}
		if (aggregateFallback == nullptr
			|| profile.representation.canonicalError
				< aggregateFallback->representation.canonicalError)
		{
			aggregateFallback = &profile;
		}
	}

	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	const float finiteProjectedRadiusPixels = std::isfinite(projectedRadiusPixels)
		? std::max(projectedRadiusPixels, 0.0f)
		: 512.0f;

	struct ClusterPick
	{
		std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
		bool substitutedAncestor = false;
		std::uint32_t substitutedAncestorId = kVirtualGeometryInvalidIndex;
	};

	const auto appendSelectedCluster = [&](const VirtualGeometryClusterRuntime& selectedCluster, const VirtualGeometryClusterRuntime& desiredCluster, bool usingFallback, ClusterPick& pick) {
		TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord record;
		record.meshId = meshId;
		record.objectId = objectId;
		record.clusterId = selectedCluster.clusterId;
		record.lodIndex = selectedCluster.lodIndex;
		record.pageId = selectedCluster.pageId;
		record.fallbackClusterId = desiredCluster.fallbackClusterId;
		record.usingFallback = usingFallback;
		pick.selectedClusters.push_back(record);
	};

	const auto appendPageNeed = [&](const VirtualGeometryClusterRuntime& desiredCluster) {
		TerraEngine::Renderer::VirtualGeometryPageNeedRecord need;
		need.meshId = meshId;
		need.objectId = objectId;
		need.pageId = desiredCluster.pageId;
		if (const VirtualGeometryClusterRuntime* fallbackCluster = FindVirtualGeometryClusterById(asset, desiredCluster.fallbackClusterId); fallbackCluster != nullptr)
		{
			need.fallbackPageId = fallbackCluster->pageId;
		}
		else if (aggregateFallback != nullptr)
		{
			need.fallbackPageId = FindVirtualGeometryClusterById(
				asset, aggregateFallback->fallbackClusterId)->pageId;
		}
		need.clusterId = desiredCluster.clusterId;
		need.desiredLodIndex = desiredCluster.lodIndex;
		need.screenImportance = EstimateProjectedClusterRadiusPixels(desiredCluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels)
			* std::max(desiredCluster.geometricError, 0.001f);
		need.fallbackRequired = true;
		need.desiredPayloadSource = VirtualGeometryPagePayloadSource::Missing;
		need.fallbackPayloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
		result.pageNeeds.push_back(need);
	};

	const auto resolveResidentCluster = [&](const VirtualGeometryClusterRuntime& desiredCluster) {
		struct ResolvedCluster
		{
			const VirtualGeometryClusterRuntime* cluster = nullptr;
			bool usingFallback = false;
		};

		if (VirtualGeometryClusterPageIsResident(asset, desiredCluster))
		{
			return ResolvedCluster{ &desiredCluster, false };
		}

		// Page streaming is intentionally fail-open. A missing high-detail page
		// records a need for ResourceStreaming, then walks resident ancestors so
		// the object remains visible instead of waiting on IO during render prep.
		appendPageNeed(desiredCluster);
		std::uint32_t fallbackId = desiredCluster.fallbackClusterId;
		for (std::size_t guard = 0u; guard < asset.clusters.size(); ++guard)
		{
			const VirtualGeometryClusterRuntime* fallbackCluster = FindVirtualGeometryClusterById(asset, fallbackId);
			if (fallbackCluster == nullptr)
			{
				break;
			}
			if (VirtualGeometryClusterPageIsResident(asset, *fallbackCluster))
			{
				return ResolvedCluster{ fallbackCluster, true };
			}
			if (fallbackCluster->fallbackClusterId == fallbackId)
			{
				break;
			}
			fallbackId = fallbackCluster->fallbackClusterId;
		}

		// The authored chain remains the first choice. Only after it cannot
		// produce a resident cluster may the independently validated aggregate
		// profile answer the request, and only when its canonical error bounds
		// the desired cluster's error.
		if (aggregateFallback != nullptr
			&& aggregateFallback->representation.canonicalError >= desiredCluster.geometricError)
		{
			const VirtualGeometryClusterRuntime* aggregateCluster =
				FindVirtualGeometryClusterById(asset, aggregateFallback->fallbackClusterId);
			if (aggregateCluster != nullptr
				&& VirtualGeometryClusterPageIsResident(asset, *aggregateCluster))
			{
				return ResolvedCluster{ aggregateCluster, true };
			}
		}

		// Never substitute an arbitrary resident cluster from another coverage
		// region. That keeps counters alive but renders the wrong island of the
		// mesh, which is worse than failing open to the owner draw range. Only the
		// authored fallback chain is allowed to answer a missing page.
		return ResolvedCluster{};
	};

	const auto shouldRefineCluster = [&](const VirtualGeometryClusterRuntime& cluster) {
		if (cluster.childCount == 0u)
		{
			return false;
		}
		const float clusterProjectedRadiusPixels = EstimateProjectedClusterRadiusPixels(cluster, modelMatrix, cameraPosition, targetSize, finiteProjectedRadiusPixels);
		const float errorPixels = clusterProjectedRadiusPixels
			* std::max(cluster.geometricError, 0.0f)
			* sanitized.lodErrorScale;
		// Cluster refinement is intentionally a little less eager than whole-mesh
		// LOD choice. A tiny error over one pixel can otherwise create noisy
		// mixed-detail cuts on flat or hard-surface assets without improving the
		// visible image. Nearby or high-error regions still refine independently.
		return std::isfinite(errorPixels) && errorPixels > kVirtualGeometryClusterRefinementErrorPixels;
	};

	const auto selectCluster = [&](auto&& self, const VirtualGeometryClusterRuntime& cluster) -> ClusterPick {
		if (shouldRefineCluster(cluster))
		{
			ClusterPick refinedPick;
			for (std::uint32_t childOffset = 0u; childOffset < cluster.childCount; ++childOffset)
			{
				const std::uint32_t childId = cluster.firstChildClusterId + childOffset;
				const VirtualGeometryClusterRuntime* childCluster = FindVirtualGeometryClusterById(asset, childId);
				if (childCluster == nullptr)
				{
					result.invalidSelectionReasons |= 1u;
					continue;
				}

				ClusterPick childPick = self(self, *childCluster);
				if (childPick.substitutedAncestor && childPick.substitutedAncestorId == cluster.clusterId)
				{
					// A desired child page is missing and resolved to this
					// parent. Selecting the parent for the whole subtree avoids
					// drawing a parent beside any of its refined children.
					ClusterPick parentPick;
					appendSelectedCluster(cluster, *childCluster, true, parentPick);
					parentPick.substitutedAncestor = true;
					parentPick.substitutedAncestorId = cluster.clusterId;
					return parentPick;
				}

				refinedPick.selectedClusters.insert(
					refinedPick.selectedClusters.end(),
					childPick.selectedClusters.begin(),
					childPick.selectedClusters.end());
			}

			if (!refinedPick.selectedClusters.empty())
			{
				return refinedPick;
			}
		}

		ClusterPick directPick;
		const auto resolved = resolveResidentCluster(cluster);
		if (resolved.cluster == nullptr)
		{
			result.invalidSelectionReasons |= 2u;
			return directPick;
		}

		appendSelectedCluster(*resolved.cluster, cluster, resolved.usingFallback, directPick);
		if (resolved.usingFallback && resolved.cluster->clusterId != cluster.clusterId)
		{
			directPick.substitutedAncestor = true;
			directPick.substitutedAncestorId = resolved.cluster->clusterId;
		}
		return directPick;
	};

	std::vector<const VirtualGeometryClusterRuntime*> rootClusters;
	rootClusters.reserve(asset.clusters.size());
	for (const VirtualGeometryClusterRuntime& cluster : asset.clusters)
	{
		if (cluster.parentClusterId == kVirtualGeometryInvalidIndex
			|| FindVirtualGeometryClusterById(asset, cluster.parentClusterId) == nullptr)
		{
			rootClusters.push_back(&cluster);
		}
	}
	if (rootClusters.empty())
	{
		rootClusters.push_back(&asset.clusters.front());
		result.invalidSelectionReasons |= 4u;
	}

	for (const VirtualGeometryClusterRuntime* rootCluster : rootClusters)
	{
		ClusterPick pick = selectCluster(selectCluster, *rootCluster);
		result.selectedClusters.insert(
			result.selectedClusters.end(),
			pick.selectedClusters.begin(),
			pick.selectedClusters.end());
	}

	// This selection runs during worker-safe render preparation on immutable
	// asset/page snapshots. Only the prepared draw range and page-need packet
	// cross into the protected backend submission and resource publication path.
	if (!result.selectedClusters.empty())
	{
		if (result.selectedClusters.size() == 1u)
		{
			result.range.selectedLodIndex = result.selectedClusters.front().lodIndex;
		}
	}

	result.range.selectedClusterOffset = 0u;
	result.range.selectedClusterCount = static_cast<std::uint32_t>(result.selectedClusters.size());
	result.range.pageNeedOffset = 0u;
	result.range.pageNeedCount = static_cast<std::uint32_t>(result.pageNeeds.size());
	return result;
}

TerraEngine::Renderer::VirtualGeometryClusterSelectionResult RenderSystem::SelectVirtualGeometryStableClusterCut(
	const VirtualGeometryAssetRuntime& asset,
	const TerraEngine::UUID& meshId,
	const TerraEngine::UUID& objectId,
	const VirtualGeometryQualityProfile& quality,
	float projectedRadiusPixels,
	const glm::mat4& modelMatrix,
	const glm::vec3& cameraPosition,
	const glm::uvec2& targetSize,
	bool taskShaderCullingEnabled,
	bool boundsValid,
	const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult* previousStableSelection,
	RenderViewType viewType,
	std::uint32_t outputId)
{
	const VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(quality);
	TerraEngine::Renderer::VirtualGeometryClusterSelectionResult candidate = SelectVirtualGeometryClusterCut(
		asset,
		meshId,
		objectId,
		sanitized,
		projectedRadiusPixels,
		modelMatrix,
		cameraPosition,
		targetSize,
		taskShaderCullingEnabled,
		boundsValid);

	const std::uint64_t transitionKey = BuildVirtualGeometryTransitionKey(
		RenderOutputKind::GameView, viewType, outputId, targetSize,
		objectId, meshId);
	const std::uint64_t candidateSignature = BuildVirtualGeometrySelectionSignature(candidate);
	const VirtualGeometryTransitionMetrics metrics = EvaluateVirtualGeometryTransitionMetrics(
		asset,
		sanitized,
		projectedRadiusPixels,
		modelMatrix,
		cameraPosition,
		targetSize);

	const VirtualGeometryLODTransitionState* previousState = previousStableSelection != nullptr && previousStableSelection->transitionState.valid
		? &previousStableSelection->transitionState
		: nullptr;
	const bool resetForDifferentView =
		previousState == nullptr
		|| previousState->viewStateKey != transitionKey
		|| previousState->viewType != viewType
		|| previousState->outputId != outputId
		|| previousState->objectId != objectId
		|| previousState->meshId != meshId;
	const bool resetForModeChange = previousState != nullptr
		&& previousState->debugVisualizationMode != sanitized.debugVisualizationMode;
	const bool resetThisFrame = resetForDifferentView || resetForModeChange;
	const std::uint32_t stableFrameCount = (!resetThisFrame
		&& previousState != nullptr
		&& previousState->candidateSelectionSignature == candidateSignature)
		? std::min(previousState->stableFrameCount + 1u, sanitized.lodTransitionStableFrames)
		: 1u;

	VirtualGeometryLODTransitionState nextState;
	nextState.viewType = viewType;
	nextState.outputId = outputId;
	nextState.objectId = objectId;
	nextState.meshId = meshId;
	nextState.viewStateKey = transitionKey;
	nextState.candidateSelectionSignature = candidateSignature;
	nextState.stableFrameCount = stableFrameCount;
	nextState.hysteresisBandPixels = sanitized.lodTransitionHysteresisPixels;
	nextState.transitionMarginPixels = metrics.transitionMarginPixels;
	nextState.candidateMaxErrorPixels = metrics.candidateMaxErrorPixels;
	nextState.debugVisualizationMode = sanitized.debugVisualizationMode;
	nextState.resetThisFrame = resetThisFrame;
	nextState.valid = true;

	const bool hasPreviousStableCut = previousStableSelection != nullptr
		&& previousState != nullptr
		&& previousState->lastStableSelectionSignature != 0u;
	const bool candidateDiffersFromStable = hasPreviousStableCut
		&& previousState->lastStableSelectionSignature != candidateSignature;
	const bool candidateIsNearThreshold = metrics.transitionCandidateCount > 0u
		&& metrics.transitionMarginPixels <= sanitized.lodTransitionHysteresisPixels;
	const bool shouldHoldPrevious = candidateDiffersFromStable
		&& candidateIsNearThreshold
		&& !resetThisFrame
		&& stableFrameCount < sanitized.lodTransitionStableFrames;

	if (shouldHoldPrevious)
	{
		TerraEngine::Renderer::VirtualGeometryClusterSelectionResult held = *previousStableSelection;
		nextState.lastStableSelectionSignature = previousState->lastStableSelectionSignature;
		nextState.heldPreviousSelection = true;
		nextState.heldFrameCount = previousState->heldFrameCount + 1u;
		nextState.acceptedFrameCount = previousState->acceptedFrameCount;
		held.transitionState = nextState;
		return held;
	}

	nextState.lastStableSelectionSignature = candidateSignature;
	nextState.heldPreviousSelection = false;
	nextState.heldFrameCount = 0u;
	nextState.acceptedFrameCount = hasPreviousStableCut
		? previousState->acceptedFrameCount + (candidateDiffersFromStable ? 1u : 0u)
		: 1u;
	candidate.transitionState = nextState;
	return candidate;
}

TerraEngine::Renderer::VirtualGeometryDrawRange RenderSystem::SelectVirtualGeometryShadowLodRange(
	const VirtualGeometryAssetRuntime& asset,
	const VirtualGeometryQualityProfile& quality,
	bool boundsValid)
{
	VirtualGeometryQualityProfile shadowQuality = SanitizeVirtualGeometryQualityProfile(quality);
	shadowQuality.maxLodDrop = 0u;

	TerraEngine::Renderer::VirtualGeometryDrawRange range = SelectVirtualGeometryResidentLodRange(
		asset,
		shadowQuality,
		1024.0f,
		true,
		boundsValid);
	range.hiZSelfOcclusionGuardPassed = true;
	range.hiZNearPlaneGuardPassed = true;
	range.hiZThinGeometryGuardPassed = true;
	range.hiZInactiveReason.clear();
	if (range.virtualGeometryActive)
	{
		range.selectedLodIndex = asset.detailLevels.empty() ? range.selectedLodIndex : asset.detailLevels.front().lodIndex;
		range.meshletOffset = asset.detailLevels.empty() ? range.meshletOffset : asset.detailLevels.front().meshletOffset;
		range.meshletCount = asset.detailLevels.empty() ? range.meshletCount : asset.detailLevels.front().meshletCount;
	}
	if (!range.failOpen)
	{
		range.fallbackReason = "shadow pass uses conservative virtual geometry visibility";
	}
	return range;
}
